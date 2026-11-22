#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace stealth {

// Must be called before any process access. Builds direct-syscall stubs,
// suppresses ETW events from this process, and generates per-run identifiers.
bool Init() noexcept;
void Shutdown() noexcept;

bool OpenProcess(HANDLE* out, ACCESS_MASK access, DWORD pid) noexcept;
bool Read(HANDLE process, uintptr_t address, void* buffer, std::size_t size) noexcept;
bool Write(HANDLE process, uintptr_t address, const void* buffer, std::size_t size) noexcept;

const std::string& WindowClass() noexcept;
const std::string& WindowTitle() noexcept;

} // namespace stealth
