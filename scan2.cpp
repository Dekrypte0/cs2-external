// Live offset recovery v2 — joint view-matrix / pawn-origin discovery.
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
static uint8_t* img = nullptr;
static size_t imgN = 0;

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

static uintptr_t module_base(const wchar_t* name) {
    HMODULE mods[2048]{}; DWORD need = 0;
    if (!EnumProcessModulesEx(gh, mods, sizeof(mods), &need, LIST_MODULES_ALL)) return 0;
    int n = (int)(need / sizeof(HMODULE));
    wchar_t buf[MAX_PATH]{};
    for (int i = 0; i < n && i < 2048; ++i) {
        if (!GetModuleFileNameExW(gh, mods[i], buf, MAX_PATH)) continue;
        const wchar_t* s = wcsrchr(buf, L'\\');
        if (_wcsicmp(s ? s + 1 : buf, name) == 0) return (uintptr_t)mods[i];
    }
    return 0;
}

struct Vec3 { float x, y, z; };
struct Ent { uintptr_t addr, sn; int sceneOff; };

static std::vector<Ent> read_entities(uintptr_t sys) {
    std::vector<Ent> out;
    static const int sceneCands[] = { 0x330, 0x328, 0x338, 0x340, 0x348, 0x350 };
    for (int cb : { 0x8, 0x10, 0x0 }) {
        std::vector<Ent> cur;
        if (!good(rd<uintptr_t>(sys + cb))) continue;
        for (int b = 0; b < 16; ++b) {
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
                    cur.push_back({ e, sn, so });
                    break;
                }
            }
        }
        if (cur.size() > out.size()) out = cur;
    }
    return out;
}

static float matlen(const float* m, int r) {
    return std::sqrt(m[r*4]*m[r*4] + m[r*4+1]*m[r*4+1] + m[r*4+2]*m[r*4+2]);
}
static float matdot(const float* m, int a, int b) {
    return m[a*4]*m[b*4] + m[a*4+1]*m[b*4+1] + m[a*4+2]*m[b*4+2];
}

int main() {
    if (!find_process()) { std::puts("[-] no cs2"); return 1; }
    client = module_base(L"client.dll");
    std::printf("[+] pid=%lu client=0x%llx\n", gpid, (unsigned long long)client);
    if (!client) return 1;

    MODULEINFO mi{};
    GetModuleInformation(gh, (HMODULE)client, &mi, sizeof(mi));
    imgN = mi.SizeOfImage;
    std::printf("[+] client image size=0x%llx\n", (unsigned long long)imgN);
    img = (uint8_t*)VirtualAlloc(nullptr, imgN, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!img || !rp(client, img, imgN)) { std::puts("[-] image read failed"); return 1; }

    uintptr_t sys = rd<uintptr_t>(client + 0x21BF9B0);
    auto ents = read_entities(sys);
    std::printf("[+] entities=%zu\n", ents.size());

    // Joint discovery: find a world->camera matrix (3 orthonormal rows) whose
    // implied camera sits ~64u above one of the entities' origin fields.
    struct Found { size_t imgOff; Vec3 cam; uintptr_t ent; int originOff; };
    std::vector<Found> found;
    float f[12];
    for (size_t off = 0; off + 48 <= imgN; off += 4) {
        std::memcpy(f, img + off, sizeof(f));
        if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) continue;
        if (matlen(f, 0) < 0.5f || matlen(f, 0) > 2.0f) continue;
        if (matlen(f, 1) < 0.5f || matlen(f, 1) > 2.0f) continue;
        if (matlen(f, 2) < 0.5f || matlen(f, 2) > 2.0f) continue;
        if (std::fabs(matdot(f, 0, 1)) > 0.15f || std::fabs(matdot(f, 0, 2)) > 0.15f ||
            std::fabs(matdot(f, 1, 2)) > 0.15f) continue;
        const float tx = f[3], ty = f[7], tz = f[11];
        Vec3 cam{
            -(tx*f[0] + ty*f[4] + tz*f[8]),
            -(tx*f[1] + ty*f[5] + tz*f[9]),
            -(tx*f[2] + ty*f[6] + tz*f[10])
        };
        if (std::fabs(cam.x) > 1e6f || std::fabs(cam.y) > 1e6f || std::fabs(cam.z) > 1e6f) continue;
        if (std::fabs(cam.x) + std::fabs(cam.y) + std::fabs(cam.z) < 50.0f) continue;
        for (const Ent& e : ents) {
            static uint8_t snb[0x400];
            if (!rp(e.sn, snb, sizeof(snb))) continue;
            for (int oo = 0; oo + 12 <= 0x300; oo += 4) {
                Vec3 o; std::memcpy(&o, snb + oo, 12);
                if (!std::isfinite(o.x) || !std::isfinite(o.y) || !std::isfinite(o.z)) continue;
                float dx = o.x - cam.x, dy = o.y - cam.y, dz = o.z - cam.z;
                if (std::fabs(dx) < 30 && std::fabs(dy) < 30 && dz < -30 && dz > -120) {
                    found.push_back({ off, cam, e.addr, oo });
                }
            }
        }
    }
    std::printf("[+] joint matches=%zu\n", found.size());
    for (int i = 0; i < (int)found.size() && i < 12; ++i) {
        const auto& f0 = found[i];
        std::printf("    imgOff=0x%llx -> client+0x%llx cam=(%.1f,%.1f,%.1f) pawn=0x%llx originOff=0x%x dz=%.1f\n",
                    (unsigned long long)f0.imgOff,
                    (unsigned long long)(client + f0.imgOff - 0), f0.cam.x, f0.cam.y, f0.cam.z,
                    (unsigned long long)f0.ent, f0.originOff,
                    f0.cam.z - 0.0f);
    }

    // Local controller hint + plausibility name scan.
    uintptr_t ctrl = rd<uintptr_t>(client + 0x21C6E60);
    std::printf("[+] local controller=0x%llx\n", (unsigned long long)ctrl);
    if (good(ctrl)) {
        static uint8_t cb[0x1000];
        rp(ctrl, cb, sizeof(cb));
        for (int off = 0; off < 0x900; off += 4) {
            char* s = (char*)(cb + off);
            size_t n = 0;
            while (n < 32 && s[n] && (isalnum((unsigned char)s[n]) || s[n]=='_' || s[n]==' ' || s[n]=='-' || s[n]=='.')) ++n;
            if (n >= 3 && n <= 24 && s[n] == 0) {
                int letters = 0; for (size_t k=0;k<n;++k) if (isalpha((unsigned char)s[k])) ++letters;
                if (letters >= 2) std::printf("    name? off=0x%x '%.*s'\n", off, (int)n, s);
            }
        }
    }

    // Field sweep on the matched local pawn.
    if (!found.empty()) {
        uintptr_t lp = found[0].ent;
        static uint8_t pb[0x2000];
        rp(lp, pb, sizeof(pb));
        std::printf("[+] local pawn=0x%llx field sweep:\n", (unsigned long long)lp);
        for (int off = 0x300; off < 0x500; off += 4) {
            int32_t v = 0; std::memcpy(&v, pb + off, 4);
            if (v >= 1 && v <= 100) std::printf("    hp? 0x%x = %d\n", off, v);
            uint8_t b = pb[off];
            if (b == 2 || b == 3) std::printf("    team? 0x%x = %u\n", off, b);
        }
        std::printf("[+] controller handle sweep:\n");
        for (int off = 0x1000; off < 0x1800; off += 4) {
            uint32_t h = 0; std::memcpy(&h, pb + off, 4);
            if (!h || h == 0xFFFFFFFFu) continue;
            uint32_t lo = h & 0x3FFF;
            if (lo >= 1 && lo <= 64) std::printf("    hctrl? 0x%x = 0x%x slot=%u\n", off, h, lo);
        }
    }

    VirtualFree(img, 0, MEM_RELEASE);
    CloseHandle(gh);
    return 0;
}
