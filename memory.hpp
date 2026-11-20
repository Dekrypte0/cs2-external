#pragma once

#include <Windows.h>
#include <Psapi.h>
#include <cstdint>
#include <string>
#include <sstream>
#include <chrono>
#include "logger.hpp"
#include "strings.hpp"

#pragma comment(lib, "Psapi.lib")

/* memory access, tuned quiet:
 *  - process discovery via NtGetNextProcess (no Toolhelp process snapshot —
 *    CreateToolhelp32Snapshot is a well-worn AV/EDR behavioral tripwire)
 *  - module bases via EnumProcessModulesEx (loader enumeration from the
 *    existing handle — no module snapshot API either)
 *  - handle is read-only: VM_READ + query bits, never a single write
 *  - identifying strings XOR'd, never plaintext (strings.hpp) */

class Memory {
private:
    HANDLE process_handle = nullptr;
    uintptr_t client_base = 0;
    uintptr_t engine_base = 0;
    DWORD process_id = 0;
    std::chrono::steady_clock::time_point last_health_check{};

    static constexpr ACCESS_MASK kOpenRights =
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION;

    typedef NTSTATUS(__stdcall* NtGetNextProcess_t)(HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);

    static bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
        if (s.size() < suffix.size()) return false;
        return _wcsnicmp(s.c_str() + (s.size() - suffix.size()), suffix.c_str(),
                         suffix.size()) == 0;
    }

    /* no snapshot fallback: NtGetNextProcess hands us a live handle with the
     * exact rights we need, and that same handle becomes our memory handle */
    HANDLE OpenByNameNoSnapshots() {
        auto pNtGetNextProcess = (NtGetNextProcess_t)
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextProcess");
        if (!pNtGetNextProcess) return nullptr;

        HANDLE prev = nullptr;
        for (;;) {
            HANDLE next = nullptr;
            const NTSTATUS st = pNtGetNextProcess(prev, kOpenRights, 0, 0, &next);
            if (prev) CloseHandle(prev);
            if (st != 0 || !next) return nullptr;
            prev = next;

            wchar_t path[MAX_PATH]{};
            DWORD len = MAX_PATH;
            if (QueryFullProcessImageNameW(prev, 0, path, &len)) {
                if (_wcsicmp(BaseNameOf(std::wstring(path)).c_str(),
                             strs::Decode(strs::CS2EXE).c_str()) == 0) {
                    return prev;   // keep the enumeration handle — it's already ours
                }
            }
        }
    }

    /* exact module-name compare — suffix matching famously collides with
     * panoramauiclient.dll, which ends in "client.dll" and loads earlier */
    static std::wstring BaseNameOf(const std::wstring& path) {
        const size_t pos = path.find_last_of(L'\\');
        return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
    }

    uintptr_t FindModuleBaseEnum(const std::wstring& module_name) {
        if (!process_handle) return 0;

        HMODULE mods[1024]{};
        DWORD needed = 0;
        if (!EnumProcessModulesEx(process_handle, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) {
            return 0;
        }
        const int count = (int)(needed / sizeof(HMODULE));
        if (count <= 0 || count > 1024) return 0;

        wchar_t name[MAX_PATH]{};
        for (int i = 0; i < count; ++i) {
            const DWORD len = GetModuleFileNameExW(process_handle, mods[i], name, MAX_PATH);
            if (!len) continue;
            if (_wcsicmp(BaseNameOf(std::wstring(name)).c_str(), module_name.c_str()) == 0) {
                return reinterpret_cast<uintptr_t>(mods[i]);
            }
        }
        return 0;
    }

public:
    bool Initialize(bool verbose = true) {
        return Attach(verbose);
    }

private:
    /* attach pipeline shared by boot and re-attach. verbose=false keeps the
     * retry loop quiet (EnsureAttached logs state transitions itself). */
    bool Attach(bool verbose) {
        Cleanup();
        if (verbose) Log("[*] hunting game process");
        process_handle = OpenByNameNoSnapshots();
        if (!process_handle) {
            if (verbose) Log("[!] game process not found");
            return false;
        }

        process_id = ::GetProcessId(process_handle);
        Log("[+] attached, pid " + std::to_string(process_id));

        client_base = FindModuleBaseEnum(strs::Decode(strs::CLIENT_DLL));
        engine_base = FindModuleBaseEnum(strs::Decode(strs::ENGINE2_DLL));

        if (verbose) {
            std::ostringstream oss;
            oss << "[+] modules: 0x" << std::hex << client_base << " / 0x" << engine_base;
            Log(oss.str());
        }

        if (client_base == 0) {
            Log("[!] no code module base");
            return false;
        }
        return true;
    }

    void Detach() {
        if (process_handle) CloseHandle(process_handle);
        process_handle = nullptr;
        process_id = 0;
        client_base = 0;
        engine_base = 0;
    }

public:
    /* liveness + re-attach. throttled to one probe per ~2s; while detached
     * it re-hunts at the same rate so a CS2 restart is picked up within
     * seconds. cheap checks only: exit code + a probe read at the module
     * base — nothing a kernel AC would flag as process tampering. */
    bool EnsureAttached() {
        const auto now = std::chrono::steady_clock::now();
        const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_health_check).count();

        if (process_handle) {
            if (since < 2000) return true;
            last_health_check = now;

            DWORD code = 0;
            const bool got = GetExitCodeProcess(process_handle, &code) != FALSE;
            if (!got || code != STILL_ACTIVE) {
                Log("[!] game process lost - waiting for restart");
                Detach();
                return false;
            }
            uint64_t probe = 0;
            if (!ReadBuffer(client_base, &probe, sizeof(probe))) {
                Log("[!] game memory unreadable - waiting for restart");
                Detach();
                return false;
            }
            return true;
        }

        if (since < 2000) return false;
        last_health_check = now;
        if (Attach(false)) {
            Log("[+] game process reacquired, pid " + std::to_string(process_id));
            return true;
        }
        return false;
    }

    template <typename T>
    T Read(uintptr_t address) const {
        T value = {};
        if (process_handle) {
            ReadProcessMemory(process_handle, reinterpret_cast<LPCVOID>(address), &value, sizeof(T), nullptr);
        }
        return value;
    }

    bool ReadBuffer(uintptr_t address, void* buffer, size_t size) const {
        if (!process_handle || !buffer || size == 0) return false;
        return ReadProcessMemory(process_handle, reinterpret_cast<LPCVOID>(address), buffer, size, nullptr) == TRUE;
    }

    uintptr_t GetClientBase() const { return client_base; }
    uintptr_t GetEngineBase() const { return engine_base; }
    DWORD GetProcessId() const { return process_id; }
    HANDLE GetProcessHandle() const { return process_handle; }

    void Cleanup() {
        Detach();
    }

    ~Memory() {
        Cleanup();
    }
};
