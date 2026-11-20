// Live dump v3 — controller-driven local pawn discovery + raw field dumps.
#include <Windows.h>
#include <Psapi.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "Psapi.lib")

static HANDLE gh = nullptr;
static uintptr_t client = 0;
static DWORD gpid = 0;

static bool rp(uintptr_t a, void* b, size_t n) {
    return a && ReadProcessMemory(gh, (LPCVOID)a, b, n, nullptr) == TRUE;
}
template <class T> static T rd(uintptr_t a) { T v{}; rp(a, &v, sizeof(v)); return v; }
static bool good(uintptr_t p) { return p >= 0x10000 && p <= 0x7FFFFFFFFFFFULL; }

static bool find_process() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    bool ok = false;
    if (Process32FirstW(snap, &pe)) do {
        if (_wcsicmp(pe.szExeFile, L"cs2.exe") == 0) { gpid = pe.th32ProcessID; ok = true; break; }
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    if (!ok) return false;
    gh = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, gpid);
    return gh != nullptr;
}

static uintptr_t module_base(const wchar_t* name, size_t* sizeOut = nullptr) {
    HMODULE mods[2048]{}; DWORD need = 0;
    if (!EnumProcessModulesEx(gh, mods, sizeof(mods), &need, LIST_MODULES_ALL)) return 0;
    int n = (int)(need / sizeof(HMODULE));
    wchar_t buf[MAX_PATH]{};
    for (int i = 0; i < n && i < 2048; ++i) {
        MODULEINFO mi{};
        GetModuleInformation(gh, mods[i], &mi, sizeof(mi));
        if (!GetModuleFileNameExW(gh, mods[i], buf, MAX_PATH)) continue;
        const wchar_t* s = wcsrchr(buf, L'\\');
        if (_wcsicmp(s ? s + 1 : buf, name) == 0) {
            if (sizeOut) *sizeOut = mi.SizeOfImage;
            return (uintptr_t)mods[i];
        }
    }
    return 0;
}

struct Vec3 { float x, y, z; };
struct Ent { uintptr_t addr, sn; int idx, sceneOff; };

static std::vector<Ent> read_entities(uintptr_t sys) {
    std::vector<Ent> out;
    static const int sceneCands[] = { 0x330, 0x328, 0x338, 0x340, 0x348, 0x350 };
    for (int cb : { 0x8, 0x10, 0x0 }) {
        std::vector<Ent> cur;
        if (!good(rd<uintptr_t>(sys + cb))) continue;
        for (int b = 0; b < 32; ++b) {
            uintptr_t chunk = rd<uintptr_t>(sys + cb + 8i64 * b);
            if (!good(chunk)) break;
            static uint8_t blob[0xE000];
            if (!rp(chunk, blob, sizeof(blob))) break;
            for (int j = 0; j < 512; ++j) {
                uintptr_t e = 0;
                std::memcpy(&e, blob + 0x70 * j, 8);
                if (!good(e)) continue;
                for (int so : sceneCands) {
                    uintptr_t sn = rd<uintptr_t>(e + so);
                    if (!good(sn)) continue;
                    if (rd<uintptr_t>(sn + 0x30) != e) continue;
                    cur.push_back({ e, sn, (b << 9) | j, so });
                    break;
                }
            }
        }
        if (cur.size() > out.size()) out = cur;
    }
    return out;
}

int main() {
    if (!find_process()) { std::puts("[-] no cs2"); return 1; }
    size_t csz = 0, esz = 0;
    client = module_base(L"client.dll", &csz);
    uintptr_t engine = module_base(L"engine2.dll", &esz);
    std::printf("[+] pid=%lu client=0x%llx size=0x%llx engine=0x%llx size=0x%llx\n",
                gpid, (unsigned long long)client, (unsigned long long)csz,
                (unsigned long long)engine, (unsigned long long)esz);

    uintptr_t sys = rd<uintptr_t>(client + 0x21BF9B0);
    auto ents = read_entities(sys);
    std::printf("[+] pawn-shaped entities=%zu\n", ents.size());

    uintptr_t ctrl = rd<uintptr_t>(client + 0x21C6E60);
    std::printf("[+] local controller=0x%llx\n", (unsigned long long)ctrl);

    // Find local pawn: scan controller for a CHandle whose low bits index a pawn-shaped entity.
    uintptr_t localPawn = 0; int hctrlOff = -1; uint32_t hval = 0; int localIdx = -1;
    if (good(ctrl)) {
        static uint8_t cb[0x1200];
        rp(ctrl, cb, sizeof(cb));
        for (int off = 0; off + 4 <= 0x1000; off += 4) {
            uint32_t h = 0; std::memcpy(&h, cb + off, 4);
            if (!h || h == 0xFFFFFFFFu) continue;
            for (uint32_t mask : { 0x3FFFu, 0x1FFFu, 0x7FFFu }) {
                int idx = (int)(h & mask);
                for (const Ent& e : ents) {
                    if (e.idx != idx) continue;
                    // sanity: this pawn must actually be pawn-shaped (it is, ents filtered)
                    localPawn = e.addr; hctrlOff = off; hval = h; localIdx = e.idx;
                    break;
                }
                if (localPawn) break;
            }
            if (localPawn) break;
        }
    }
    std::printf("[+] local pawn via controller handle=0x%llx (m_hPawn off=0x%x val=0x%x idx=%d)\n",
                (unsigned long long)localPawn, hctrlOff, hval, localIdx);

    // Dump controller fields that look like slot / team / handle.
    if (good(ctrl)) {
        static uint8_t cb[0x1200]; rp(ctrl, cb, sizeof(cb));
        std::printf("[+] controller field dump (interesting values):\n");
        for (int off = 0x300; off < 0xA00; off += 4) {
            uint32_t v = 0; std::memcpy(&v, cb + off, 4);
            if (v >= 1 && v <= 64) std::printf("    slot? 0x%x = %u\n", off, v);
            if (v == 2 || v == 3) std::printf("    team? 0x%x = %u\n", off, v);
        }
    }

    if (localPawn) {
        // Pawn body dump: print any field that looks like hp(1..100), team(2/3), life(0..2)
        static uint8_t pb[0x2000]; rp(localPawn, pb, sizeof(pb));
        std::printf("[+] local pawn body dump (0x300..0x600):\n");
        for (int off = 0x300; off < 0x600; off += 4) {
            int32_t v = 0; std::memcpy(&v, pb + off, 4);
            uint8_t b = pb[off];
            if ((v >= 1 && v <= 100) || b == 2 || b == 3)
                std::printf("    0x%x: i32=%d u8=%u\n", off, v, b);
        }
        // Scene node Vec3 dump — find the origin field by value pattern.
        uintptr_t sn = rd<uintptr_t>(localPawn + 0x330);
        std::printf("[+] local pawn scene node=0x%llx Vec3 dump:\n", (unsigned long long)sn);
        if (good(sn)) {
            static uint8_t sb[0x400]; rp(sn, sb, sizeof(sb));
            for (int off = 0; off + 12 <= 0x380; off += 4) {
                Vec3 o; std::memcpy(&o, sb + off, 12);
                float mag = std::fabs(o.x) + std::fabs(o.y) + std::fabs(o.z);
                if (std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.z) && mag > 10.0f && mag < 1e7f)
                    std::printf("    0x%x: (%.2f, %.2f, %.2f)\n", off, o.x, o.y, o.z);
            }
        }
        // Pawn -> controller-backref handle sweep (0x1400..0x1800)
        std::printf("[+] pawn controller-handle sweep:\n");
        for (int off = 0x1000; off < 0x1800; off += 4) {
            uint32_t h = 0; std::memcpy(&h, pb + off, 4);
            if (!h || h == 0xFFFFFFFFu) continue;
            uint32_t lo = h & 0x3FFF;
            if (lo >= 1 && lo <= 64) std::printf("    0x%x = 0x%x slot=%u\n", off, h, lo);
        }
    }

    // Dump all pawn-shaped entities with a coarse health guess.
    std::printf("[+] pawn entities dump:\n");
    for (const Ent& e : ents) {
        static uint8_t b[0x600]; rp(e.addr, b, sizeof(b));
        int hp = -1;
        for (int off = 0x300; off < 0x500; off += 4) {
            int32_t v = 0; std::memcpy(&v, b + off, 4);
            if (v >= 1 && v <= 100) { hp = v; break; }
        }
        std::printf("    idx=%d addr=0x%llx hp~%d\n", e.idx, (unsigned long long)e.addr, hp);
    }

    CloseHandle(gh);
    return 0;
}
