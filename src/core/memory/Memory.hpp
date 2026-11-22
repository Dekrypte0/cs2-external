#ifndef _PPROCESS_HPP_
#define _PPROCESS_HPP_

#include <algorithm>
#include <vector>
#include <math.h>
#include <Windows.h>
#include <TlHelp32.h>
#include <string>
#include <iostream>
#include <Psapi.h> 

#include "core/stealth/Stealth.hpp"

struct ProcessModule
{
	uintptr_t base, size;
};

class pProcess
{
public:
	DWORD		  pid_; // process id
	HANDLE		  handle_; // handle to process
	HWND		  hwnd_; // window handle
	ProcessModule base_client_;

public:
	bool AttachProcess(const char* process_name);
	bool AttachWindow(const char* window_name);
	bool UpdateHWND();
	void Close();

public:
	ProcessModule GetModule(const char* module_name);
	LPVOID		  Allocate(size_t size_in_bytes);
	uintptr_t	  FindCodeCave(uint32_t length_in_bytes);
	uintptr_t     FindSignature(std::vector<uint8_t> signature);
	uintptr_t     FindSignature(ProcessModule target_module, std::vector<uint8_t> signature);

	template<class T>
	uintptr_t ReadOffsetFromSignature(std::vector<uint8_t> signature, uint8_t offset) // offset example: "FF 05 ->22628B01<-" offset is 2
	{
		uintptr_t pattern_address = this->FindSignature(signature);
		if (!pattern_address)
			return 0x0;

		T offset_value = this->read<T>(pattern_address + offset);
		return pattern_address + offset_value + offset + sizeof(T);
	}

	// Large signature scans go through the same direct-syscall path as small
	// reads, split into bounded chunks so a single oversized request never
	// trips a size-based heuristic or a partial-read failure.
	bool ReadRegion(uintptr_t address, void* buffer, size_t size) const
	{
		constexpr size_t chunk = 1u << 20; // 1 MiB
		auto* out = static_cast<uint8_t*>(buffer);

		for (size_t offset = 0; offset < size; offset += chunk) {
			const size_t current = std::min(chunk, size - offset);
			if (!stealth::Read(handle_, address + offset, out + offset, current))
				return false;
		}
		return true;
	}

	bool read_raw(uintptr_t address, void* buffer, size_t size)
	{
		return stealth::Read(handle_, address, buffer, size);
	}

	template<class T>
	void write(uintptr_t address, T value)
	{
		stealth::Write(handle_, address, &value, sizeof(T));
	}

	template<class T>
	T read(uintptr_t address)
	{
		T buffer{};
		stealth::Read(handle_, address, &buffer, sizeof(T));
		return buffer;
	}

	void write_bytes(uintptr_t addr, std::vector<uint8_t> patch)
	{
		if (!patch.empty())
			stealth::Write(handle_, addr, patch.data(), patch.size());
	}

	uintptr_t read_multi_address(uintptr_t ptr, std::vector<uintptr_t> offsets)
	{
		uintptr_t buffer = ptr;
		for (int i = 0; i < offsets.size(); i++)
			buffer = this->read<uintptr_t>(buffer + offsets[i]);

		return buffer;
	}

	template <typename T>
	T read_multi(uintptr_t base, std::vector<uintptr_t> offsets) {
		uintptr_t buffer = base;
		for (int i = 0; i < offsets.size() - 1; i++)
		{
			buffer = this->read<uintptr_t>(buffer + offsets[i]);
		}
		return this->read<T>(buffer + offsets.back());
	}

private:
	uint32_t FindProcessIdByProcessName(const char* process_name);
	uint32_t FindProcessIdByWindowName(const char* window_name);
	HWND GetWindowHandleFromProcessId(DWORD ProcessId);
};
#endif