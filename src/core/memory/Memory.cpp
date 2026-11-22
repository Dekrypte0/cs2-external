#include "Memory.hpp"
#include <tlhelp32.h>
#include <utility>

uint32_t pProcess::FindProcessIdByProcessName(const char* ProcessName)
{
	std::wstring wideProcessName;
	int wideCharLength = MultiByteToWideChar(CP_UTF8, 0, ProcessName, -1, nullptr, 0);
	if (wideCharLength > 0)
	{
		wideProcessName.resize(wideCharLength);
		MultiByteToWideChar(CP_UTF8, 0, ProcessName, -1, &wideProcessName[0], wideCharLength);
	}

	HANDLE hPID = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
	PROCESSENTRY32W process_entry_{ };
	process_entry_.dwSize = sizeof(PROCESSENTRY32W);

	DWORD pid = 0;
	if (Process32FirstW(hPID, &process_entry_))
	{
		do
		{
			if (!wcscmp(process_entry_.szExeFile, wideProcessName.c_str()))
			{
				pid = process_entry_.th32ProcessID;
				break;
			}
		} while (Process32NextW(hPID, &process_entry_));
	}
	CloseHandle(hPID);
	return pid;
}

uint32_t pProcess::FindProcessIdByWindowName(const char* WindowName)
{
	DWORD process_id = 0;
	HWND windowHandle = FindWindowA(nullptr, WindowName);
	if (windowHandle)
		GetWindowThreadProcessId(windowHandle, &process_id);
	return process_id;
}

HWND pProcess::GetWindowHandleFromProcessId(DWORD ProcessId) {
	HWND hwnd = NULL;
	do {
		hwnd = FindWindowEx(NULL, hwnd, NULL, NULL);
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid == ProcessId) {
			TCHAR windowTitle[MAX_PATH];
			GetWindowText(hwnd, windowTitle, MAX_PATH);
			if (IsWindowVisible(hwnd) && windowTitle[0] != '\0') {
				return hwnd;
			}
		}
	} while (hwnd != NULL);
	return NULL; // No main window found for the given process ID
}

bool pProcess::AttachProcess(const char* ProcessName)
{
	this->pid_ = this->FindProcessIdByProcessName(ProcessName);

	if (pid_)
	{
		HMODULE modules[0xFF];
		MODULEINFO module_info;
		DWORD _;

		// Only VM_READ is needed for an external ESP. VM_OPERATION and VM_WRITE
		// add noise to the handle's access mask and are never used here.
		if (!stealth::OpenProcess(&handle_, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, pid_))
			return false;

		EnumProcessModulesEx(this->handle_, modules, sizeof(modules), &_, LIST_MODULES_64BIT);
		base_client_.base = (uintptr_t)modules[0];

		GetModuleInformation(this->handle_, modules[0], &module_info, sizeof(module_info));
		base_client_.size = module_info.SizeOfImage;

		hwnd_ = this->GetWindowHandleFromProcessId(pid_);

		return true;
	}

	return false;
}

bool pProcess::AttachWindow(const char* WindowName)
{
	this->pid_ = this->FindProcessIdByWindowName(WindowName);

	if (pid_)
	{
		HMODULE modules[0xFF];
		MODULEINFO module_info;
		DWORD _;

		if (!stealth::OpenProcess(&handle_, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, pid_))
			return false;

		EnumProcessModulesEx(this->handle_, modules, sizeof(modules), &_, LIST_MODULES_64BIT);
		base_client_.base = (uintptr_t)modules[0];

		GetModuleInformation(this->handle_, modules[0], &module_info, sizeof(module_info));
		base_client_.size = module_info.SizeOfImage;

		hwnd_ = this->GetWindowHandleFromProcessId(pid_);

		return true;
	}
	return false;
}

bool pProcess::UpdateHWND()
{
	hwnd_ = this->GetWindowHandleFromProcessId(pid_);
	return hwnd_ != nullptr;
}

ProcessModule pProcess::GetModule(const char* lModule)
{
	std::wstring wideModule;
	int wideCharLength = MultiByteToWideChar(CP_UTF8, 0, lModule, -1, nullptr, 0);
	if (wideCharLength > 0)
	{
		wideModule.resize(wideCharLength);
		MultiByteToWideChar(CP_UTF8, 0, lModule, -1, &wideModule[0], wideCharLength);
	}

	HANDLE handle_module = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid_);
	if (handle_module == INVALID_HANDLE_VALUE)
		return { 0, 0 };

	MODULEENTRY32W module_entry_{};
	module_entry_.dwSize = sizeof(module_entry_);

	if (Module32FirstW(handle_module, &module_entry_))
	{
		do
		{
			if (!wcscmp(module_entry_.szModule, wideModule.c_str()))
			{
				ProcessModule found{ (uintptr_t)module_entry_.modBaseAddr, module_entry_.dwSize };
				CloseHandle(handle_module);
				return found;
			}
		} while (Module32NextW(handle_module, &module_entry_));
	}

	CloseHandle(handle_module);
	return { 0, 0 };
}

LPVOID pProcess::Allocate(size_t size_in_bytes)
{
	// Requires VM_OPERATION, which AttachProcess deliberately no longer asks
	// for. Kept for debug tooling; release builds never allocate in the target.
	return VirtualAllocEx(this->handle_, NULL, size_in_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

uintptr_t pProcess::FindSignature(std::vector<uint8_t> signature)
{
	return FindSignature(base_client_, std::move(signature));
}

uintptr_t pProcess::FindSignature(ProcessModule target_module, std::vector<uint8_t> signature)
{
	if (!target_module.base || !target_module.size ||
		signature.empty() || signature.size() > target_module.size)
		return 0x0;

	auto data = std::make_unique<uint8_t[]>(target_module.size);
	if (!ReadRegion(target_module.base, data.get(), target_module.size))
		return 0x0;

	const size_t last_start = target_module.size - signature.size();
	for (size_t i = 0; i <= last_start; ++i)
	{
		size_t j = 0;
		for (; j < signature.size(); ++j)
		{
			// 0x00 is a wildcard in every signature in Offsets.hpp.
			if (signature[j] != 0 && data[i + j] != signature[j])
				break;
		}
		if (j == signature.size())
			return target_module.base + i;
	}
	return 0x0;
}

uintptr_t pProcess::FindCodeCave(uint32_t length_in_bytes)
{
	std::vector<uint8_t> cave_pattern = {};

	for (uint32_t i = 0; i < length_in_bytes; i++) {
		cave_pattern.push_back(0x00);
	}

	return FindSignature(cave_pattern);
}

void pProcess::Close()
{
	CloseHandle(handle_);
}
