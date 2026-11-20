// One-off live offset recovery for the running cs2.exe build.
// Reads only. Prints candidate offsets recovered from ground-truth structures.
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

static HANDLE   gh = nullptr;
static uintptr_t client = 0;
static uintptr_t engine = 0;
static DWORD     gpid = 0;

static bool rp(uintptr_t a, void* b, size_t n) {
    return a && ReadProcessMemory(gh, (LPCVOID)a, b, n, nullptr) == TRUE;
}
template <class T>
static T rd(uintptr_t a) { T v{}; rp(a, &v, sizeof(v)); return v; }
static bool good(uintptr_t p) { return p >= 0x10000 && p <= 0x7FFFFFFFFFFFULL; }

static bool find_process() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    bool ok = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"cs2.exe") == 0) {
                gpid = pe.th32ProcessID; ok = true; break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!ok) return false;
    gh = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, gpid);
    return gh != nullptr;
}

static uintptr_t module_base(const wchar_t* name) {
    HMODULE mods[2048]{}; DWORD need = 0;
    if (!EnumProcessModulesEx(gh, mods, sizeof(mods), &need, LIST_MODULES_ALL)) return 0;
    int n = (int)(need / sizeof(HMODULE));
    wchar_t buf[MAX_PATH]{};
    for (int i = 0; i < n && i < 2048; ++i) {
        if (!GetModuleFileNameExW(gh, mods[i], buf, MAX_PATH)) continue;
        const wchar_t* slash = wcsrchr(buf, L'\\');
        if (_wcsicmp(slash ? slash + 1 : buf, name) == 0)
            return (uintptr_t)mods[i];
    }
    return 0;
}

struct Vec3 { float x, y, z; };
struct Ent { uintptr_t addr; uintptr_t sn; int sceneOff; Vec3 origin; };

// Try to interpret p as CGameEntitySystem; return entities with scene-node backref.
static std::vector<Ent> read_entities(uintptr_t sys, int chunkBase, int& usedBase) {
    std::vector<Ent> out;
    for (int cb : { chunkBase, 0x8, 0x10 }) {
        std::vector<Ent> cur;
        uintptr_t firstChunk = rd<uintptr_t>(sys + cb);
        if (!good(firstChunk)) continue;
        static const int sceneCands[] = { 0x330, 0x328, 0x338, 0x340, 0x348, 0x350 };
        for (int b = 0; b < 8; ++b) {
            uintptr_t chunk = rd<uintptr_t>(sys + cb + 8i64 * b);
            if (!good(chunk)) break;
            uint8_t blob[0xE000]{};
            if (!rp(chunk, blob, sizeof(blob))) break;
            for (int j = 0; j < 512; ++j) {
                uintptr_t e = 0;
                std::memcpy(&e, blob + 0x70 * j, 8);
                if (!good(e)) continue;
                for (int so : sceneCands) {
                    uintptr_t sn = rd<uintptr_t>(e + so);
                    if (!good(sn)) continue;
                    if (rd<uintptr_t>(sn + 0x30) != e) continue;
                    Vec3 o = rd<Vec3>(sn + 0xC8);
                    if (!std::isfinite(o.x) || !std::isfinite(o.y) || !std::isfinite(o.z)) continue;
                    cur.push_back({ e, sn, so, o });
                    break;
                }
            }
        }
        if ((int)cur.size() > (int)out.size()) { out = cur; usedBase = cb; }
    }
    return out;
}

static bool view_cam(uintptr_t maddr, Vec3& cam) {
    float m[16]{};
    if (!rp(maddr, m, sizeof(m))) return false;
    auto len = [&](int r) { return std::sqrt(m[r*4]*m[r*4] + m[r*4+1]*m[r*4+1] + m[r*4+2]*m[r*4+2]); };
    for (int r = 0; r < 3; ++r) if (len(r) < 0.5f || len(r) > 2.0f) return false;
    const float tx=m[3], ty=m[7], tz=m[11];
    cam.x = -(tx*m[0] + ty*m[4] + tz*m[8]);
    cam.y = -(tx*m[1] + ty*m[5] + tz*m[9]);
    cam.z = -(tx*m[2] + ty*m[6] + tz*m[10]);
    return cam.x != 0 || cam.y != 0 || cam.z != 0;
}

static bool name_at(uintptr_t addr, std::string& out) {
    char buf[128]{};
    if (!rp(addr, buf, sizeof(buf)-1)) return false;
    size_t n = 0;
    while (n < 64 && buf[n] && (unsigned char)buf[n] >= 0x20 && (unsigned char)buf[n] != 0x7f) ++n;
    if (n < 2 || n > 40 || buf[n] != 0) return false;
    // reject arrays that are mostly zero/one value (e.g. padding)
    int distinct = 0;
    for (size_t i = 1; i < n; ++i) if (buf[i] != buf[i-1]) ++distinct;
    if (distinct < 1) return false;
    out.assign(buf, n);
    return true;
}

int main() {
    if (!find_process()) { std::puts("[-] cs2.exe not found"); return 1; }
    std::printf("[+] pid=%lu client hint base from module enum\n", gpid);
    client = module_base(L"client.dll");
    engine = module_base(L"engine2.dll");
    std::printf("[+] client=0x%llx engine=0x%llx\n", (unsigned long long)client, (unsigned long long)engine);
    if (!client) return 1;

    // --- entity system: validate the hint from offsets.hpp, else scan .data ---
    const uintptr_t hintEnt = 0x21BF9B0;
    int usedBase = -1;
    std::vector<Ent> ents = read_entities(client + hintEnt, 0x10, usedBase);
    uintptr_t entSysOff = hintEnt;
    if (ents.size() < 3) {
        std::printf("[!] entity hint weak (%zu), scanning client.dll .data ...\n", ents.size());
        // read a generous region of client.dll and scan aligned qwords as candidate sys pointers
        static uint8_t img[0x400000]{};
        if (!rp(client, img, sizeof(img))) { std::puts("[-] image read failed"); return 1; }
        int best = -1; size_t bestN = ents.size(); int bestCb = usedBase;
        for (size_t off = 0; off + 8 <= sizeof(img); off += 8) {
            uintptr_t cand = 0; std::memcpy(&cand, img + off, 8);
            if (!good(cand)) continue;
            int cb = -1;
            auto got = read_entities(cand, 0x10, cb);
            if (got.size() > bestN) { bestN = got.size(); best = (int)off; bestCb = cb; ents = got; }
            if (bestN > 40) break;
        }
        if (best >= 0) { entSysOff = best; usedBase = bestCb; }
        std::printf("[+] scanned entity sys: client+0x%llx chunkBase=0x%x ents=%zu\n",
                    (unsigned long long)entSysOff, usedBase, ents.size());
    } else {
        std::printf("[+] entity hint ok: client+0x%llx chunkBase=0x%x ents=%zu\n",
                    (unsigned long long)entSysOff, usedBase, ents.size());
    }

    // --- local controller hint ---
    const uintptr_t hintCtrl = 0x21C6E60;
    uintptr_t ctrl = rd<uintptr_t>(client + hintCtrl);
    std::printf("[+] local controller hint: client+0x%llx -> 0x%llx\n",
                (unsigned long long)hintCtrl, (unsigned long long)ctrl);
    std::string ctrlName; int nameOff = -1;
    if (good(ctrl)) {
        for (int off = 0x600; off < 0x800; off += 4) {
            std::string nm;
            if (name_at(ctrl + off, nm)) { nameOff = off; ctrlName = nm; break; }
        }
        std::printf("[+] controller name: off=0x%x name='%s'\n", nameOff, ctrlName.c_str());
    }

    // --- camera from view matrix hint(s) ---
    Vec3 cam{};
    uintptr_t viewOff = 0;
    for (uintptr_t off : { (uintptr_t)0x255CDD0, (uintptr_t)0x255CE50, (uintptr_t)0x255CE10 }) {
        Vec3 c{};
        if (view_cam(client + off, c)) { cam = c; viewOff = off; break; }
    }
    std::printf("[+] view matrix: client+0x%llx cam=(%.1f, %.1f, %.1f)\n",
                (unsigned long long)viewOff, cam.x, cam.y, cam.z);

    // --- local pawn = entity ~64u below camera ---
    uintptr_t localPawn = 0; Ent localEnt{}; float bestD = 1e9f;
    if (viewOff) {
        for (const Ent& e : ents) {
            float dx = e.origin.x - cam.x, dy = e.origin.y - cam.y, dz = e.origin.z - cam.z;
            float d = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (d < bestD) { bestD = d; localPawn = e.addr; localEnt = e; }
        }
        std::printf("[+] local pawn=0x%llx idx? dist-to-cam=%.1f sceneOff=0x%x origin=(%.1f,%.1f,%.1f)\n",
                    (unsigned long long)localPawn, bestD, localEnt.sceneOff,
                    localEnt.origin.x, localEnt.origin.y, localEnt.origin.z);
    }

    // --- sweep field offsets on the local pawn ---
    if (localPawn) {
        uint8_t buf[0x2000]{};
        rp(localPawn, buf, sizeof(buf));
        std::printf("[+] pawn field sweep (local pawn):\n");
        for (int off = 0x300; off < 0x600; off += 4) {
            int32_t v = 0; std::memcpy(&v, buf + off, 4);
            if (v >= 1 && v <= 100) std::printf("    hp? off=0x%x val=%d\n", off, v);
            uint8_t b = buf[off];
            if (b == 2 || b == 3) std::printf("    team? off=0x%x val=%u\n", off, b);
        }
        std::printf("[+] controller-handle sweep (low14 in 1..64):\n");
        for (int off = 0x1000; off < 0x1800; off += 4) {
            uint32_t h = 0; std::memcpy(&h, buf + off, 4);
            if (!h || h == 0xFFFFFFFFu) continue;
            uint32_t lo = h & 0x3FFF;
            if (lo >= 1 && lo <= 64 && (h >> 14) < 0x4000)
                std::printf("    ctrHandle? off=0x%x val=0x%x slot=%u\n", off, h, lo);
        }
        // lifestate: byte == 0 near 0x350
        std::printf("[+] lifeState sweep (byte==0):\n");
        for (int off = 0x340; off < 0x380; ++off)
            if (buf[off] == 0) std::printf("    life? off=0x%x\n", off);
    }

    // --- controller slot sweep ---
    if (good(ctrl)) {
        uint8_t cbuf[0x1000]{};
        rp(ctrl, cbuf, sizeof(cbuf));
        std::printf("[+] controller slot sweep (u32 1..64):\n");
        for (int off = 0x900; off < 0xA00; off += 4) {
            uint32_t v = 0; std::memcpy(&v, cbuf + off, 4);
            if (v >= 1 && v <= 64) std::printf("    slot? off=0x%x val=%u\n", off, v);
        }
        std::printf("[+] controller team sweep (byte 2/3):\n");
        for (int off = 0x3D0; off < 0x420; ++off)
            if (cbuf[off] == 2 || cbuf[off] == 3)
                std::printf("    team? off=0x%x val=%u\n", off, cbuf[off]);
    }

    // --- dump all pawn-shaped entities with recovered scene offset ---
    std::printf("[+] pawn-shaped entities (%zu):\n", ents.size());
    int so = localPawn ? localEnt.sceneOff : 0x330;
    for (const Ent& e : ents) {
        uint8_t b[0x600]{};
        rp(e.addr, b, sizeof(b));
        int hp = -1, team = -1;
        for (int off = 0x300; off < 0x500; off += 4) {
            int32_t v = 0; std::memcpy(&v, b + off, 4);
            if (hp < 0 && v >= 1 && v <= 100) hp = v;
        }
        for (int off = 0x3C0; off < 0x420; ++off) {
            if (b[off] == 2 || b[off] == 3) { team = b[off]; break; }
        }
        std::printf("    idx=%d addr=0x%llx sceneOff=0x%x hp~%d team~%d origin=(%.0f,%.0f,%.0f)\n",
                    e.addr ? 0 : 0, (unsigned long long)e.addr, e.sceneOff, hp, team,
                    e.origin.x, e.origin.y, e.origin.z);
    }

    std::puts("[+] scan complete");
    CloseHandle(gh);
    return 0;
}
