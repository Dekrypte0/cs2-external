#include "Stealth.hpp"
#include "Obfuscate.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <random>

// winnt.h exposes these only in some SDK configurations; define them locally
// so the build does not depend on which headers Windows.h happened to pull in.
#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif
#ifndef MAXULONG
#define MAXULONG 0xFFFFFFFFUL
#endif

namespace stealth {
namespace {

constexpr std::size_t kStubBytes = 64;
constexpr std::size_t kRandomNameLength = 14;
constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

// Mirrors the native layouts. Declared here so winternl.h never enters the
// translation unit and drags its own macros into the project.
struct UnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct ObjectAttributes {
    ULONG Length;
    HANDLE RootDirectory;
    UnicodeString* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};

struct ClientId {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
};

using OpenProcFn = NTSTATUS(NTAPI*)(HANDLE*, ACCESS_MASK, ObjectAttributes*, ClientId*);
// The count out-parameter is PSIZE_T, not PULONG. A ULONG slot gets eight
// bytes written on x64 and corrupts the caller's stack cookie.
using ReadVmFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
using WriteVmFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);

struct State {
    OpenProcFn open = nullptr;
    ReadVmFn read = nullptr;
    WriteVmFn write = nullptr;
    std::array<void*, 3> stubs{};

    FARPROC etw = nullptr;
    uint8_t etw_original = 0;

    std::string window_class;
    std::string window_title;
    bool ready = false;
};

State g_state;

HMODULE Ntdll() noexcept {
    static HMODULE module = [] {
        const std::string name = OBF("ntdll.dll");
        return GetModuleHandleA(name.c_str());
    }();
    return module;
}

// On x64 the first instruction pair of every ntdll syscall stub is
// `mov r10, rcx; mov eax, <id>`. Pull the id out of the live image so the
// build does not hard-code IDs that change between Windows releases.
uint32_t SyscallId(const char* name) noexcept {
    HMODULE ntdll = Ntdll();
    if (!ntdll)
        return 0;

    FARPROC proc = GetProcAddress(ntdll, name);
    if (!proc)
        return 0;

    const auto* bytes = reinterpret_cast<const uint8_t*>(proc);
    if (bytes[0] != 0x4C || bytes[1] != 0x8B || bytes[2] != 0xD1 || bytes[3] != 0xB8)
        return 0; // stub is hooked or the layout changed; fail closed

    return *reinterpret_cast<const uint32_t*>(bytes + 4);
}

template <typename Fn>
Fn MakeStub(const char* name, void*& slot) noexcept {
    const uint32_t id = SyscallId(name);
    if (!id)
        return nullptr;

    void* memory = VirtualAlloc(nullptr, kStubBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory)
        return nullptr;

    const uint8_t stub[] = {
        0x4C, 0x8B, 0xD1, 0xB8,
        static_cast<uint8_t>(id),
        static_cast<uint8_t>(id >> 8),
        static_cast<uint8_t>(id >> 16),
        static_cast<uint8_t>(id >> 24),
        0x0F, 0x05,
        0xC3
    };
    std::memcpy(memory, stub, sizeof(stub));

    DWORD old = 0;
    if (!VirtualProtect(memory, kStubBytes, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(memory, 0, MEM_RELEASE);
        return nullptr;
    }

    slot = memory;
    return reinterpret_cast<Fn>(memory);
}

// Disables ETW event emission from this process. Userland RPM telemetry is a
// common detection feed; a one-byte ret in EtwEventWrite cuts it at the source.
void PatchEtw() noexcept {
    HMODULE ntdll = Ntdll();
    if (!ntdll)
        return;

    FARPROC proc = GetProcAddress(ntdll, OBF("EtwEventWrite").c_str());
    if (!proc)
        return;

    DWORD old = 0;
    if (!VirtualProtect(proc, 1, PAGE_EXECUTE_READWRITE, &old))
        return;

    g_state.etw = proc;
    g_state.etw_original = *reinterpret_cast<uint8_t*>(proc);
    *reinterpret_cast<uint8_t*>(proc) = 0xC3;

    VirtualProtect(proc, 1, old, &old);
}

std::string RandomName() {
    std::random_device device;
    std::mt19937_64 generator(device() ^ (static_cast<uint64_t>(GetTickCount64()) << 32));
    std::uniform_int_distribution<std::size_t> pick(0, sizeof(kAlphabet) - 2);

    std::string name;
    name.reserve(kRandomNameLength);
    for (std::size_t i = 0; i < kRandomNameLength; ++i)
        name.push_back(kAlphabet[pick(generator)]);
    return name;
}

bool InitImpl() noexcept {
    g_state.open = MakeStub<OpenProcFn>(OBF("NtOpenProcess").c_str(), g_state.stubs[0]);
    g_state.read = MakeStub<ReadVmFn>(OBF("NtReadVirtualMemory").c_str(), g_state.stubs[1]);
    g_state.write = MakeStub<WriteVmFn>(OBF("NtWriteVirtualMemory").c_str(), g_state.stubs[2]);

    if (!g_state.open || !g_state.read || !g_state.write)
        return false;

    PatchEtw();

    g_state.window_class = RandomName();
    g_state.window_title = RandomName();
    g_state.ready = true;
    return true;
}

} // namespace

volatile unsigned char g_decode_key = 0;

// The key lives in process environment state, which the optimizer cannot
// inspect. 'a' ^ ';' is 0x5A, the key the compile-time side XORs with.
bool InitDecodeKey() noexcept {
    if (!SetEnvironmentVariableA("ESP_KEY", "a;"))
        return false;

    char value[8]{};
    const DWORD length = GetEnvironmentVariableA("ESP_KEY", value, sizeof(value));
    if (length < 2 || length >= sizeof(value))
        return false;

    const unsigned char key = static_cast<unsigned char>(value[0] ^ value[1]);
    if (key != 0x5A) // fail closed: every OBF() string would decode to garbage
        return false;

    g_decode_key = key;
    return true;
}

bool Init() noexcept {
    if (!InitDecodeKey())
        return false;
    return InitImpl();
}

void Shutdown() noexcept {
    if (g_state.etw) {
        DWORD old = 0;
        if (VirtualProtect(g_state.etw, 1, PAGE_EXECUTE_READWRITE, &old)) {
            *reinterpret_cast<uint8_t*>(g_state.etw) = g_state.etw_original;
            VirtualProtect(g_state.etw, 1, old, &old);
        }
        g_state.etw = nullptr;
    }

    for (void* stub : g_state.stubs) {
        if (stub)
            VirtualFree(stub, 0, MEM_RELEASE);
    }
    g_state.stubs.fill(nullptr);

    g_state.open = nullptr;
    g_state.read = nullptr;
    g_state.write = nullptr;
    g_state.ready = false;
}

bool OpenProcess(HANDLE* out, ACCESS_MASK access, DWORD pid) noexcept {
    if (!g_state.ready || !out)
        return false;

    ObjectAttributes attributes{};
    attributes.Length = sizeof(attributes);

    ClientId id{};
    id.UniqueProcess = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid));

    HANDLE handle = nullptr;
    const NTSTATUS status = g_state.open(&handle, access, &attributes, &id);
    if (!NT_SUCCESS(status) || !handle)
        return false;

    *out = handle;
    return true;
}

bool Read(HANDLE process, uintptr_t address, void* buffer, std::size_t size) noexcept {
    if (!g_state.ready || !process || !buffer || size == 0 || size > MAXULONG)
        return false;

    SIZE_T bytes_read = 0;
    const NTSTATUS status = g_state.read(
        process, reinterpret_cast<PVOID>(address), buffer,
        static_cast<SIZE_T>(size), &bytes_read);

    return NT_SUCCESS(status) || bytes_read == size;
}

bool Write(HANDLE process, uintptr_t address, const void* buffer, std::size_t size) noexcept {
    if (!g_state.ready || !process || !buffer || size == 0 || size > MAXULONG)
        return false;

    SIZE_T bytes_written = 0;
    const NTSTATUS status = g_state.write(
        process, reinterpret_cast<PVOID>(address), const_cast<PVOID>(buffer),
        static_cast<SIZE_T>(size), &bytes_written);

    return NT_SUCCESS(status) || bytes_written == size;
}

const std::string& WindowClass() noexcept {
    return g_state.window_class;
}

const std::string& WindowTitle() noexcept {
    return g_state.window_title;
}

} // namespace stealth
