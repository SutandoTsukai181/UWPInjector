/**
 * Copyright (C) 2014 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include <stdio.h>
#include <Windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib") 
#include <sddl.h>
#include <AclAPI.h>
#include <TlHelp32.h>

struct loading_data
{
	WCHAR load_path[MAX_PATH] = L"";
	decltype(&GetLastError) GetLastError = nullptr;
	decltype(&LoadLibraryW) LoadLibraryW = nullptr;
};

struct scoped_handle
{
	HANDLE handle;

	scoped_handle() :
		handle(INVALID_HANDLE_VALUE) {}
	scoped_handle(HANDLE handle) :
		handle(handle) {}
	scoped_handle(scoped_handle&& other) :
		handle(other.handle) {
		other.handle = NULL;
	}
	~scoped_handle() { if (handle != NULL && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }

	operator HANDLE() const { return handle; }

	HANDLE* operator&() { return &handle; }
	const HANDLE* operator&() const { return &handle; }
};

static void update_acl_for_uwp(LPWSTR path)
{
	OSVERSIONINFOEX verinfo_windows7 = { sizeof(OSVERSIONINFOEX), 6, 1 };
	const bool is_windows7 = VerifyVersionInfo(&verinfo_windows7, VER_MAJORVERSION | VER_MINORVERSION,
		VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_EQUAL), VER_MINORVERSION, VER_EQUAL)) != FALSE;
	if (is_windows7)
		return; // There is no UWP on Windows 7, so no need to update DACL

	PACL old_acl = nullptr, new_acl = nullptr;
	PSECURITY_DESCRIPTOR sd = nullptr;
	SECURITY_INFORMATION siInfo = DACL_SECURITY_INFORMATION;

	// Get the existing DACL for the file
	if (GetNamedSecurityInfo(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_acl, nullptr, &sd) != ERROR_SUCCESS)
		return;
	LocalFree(sd);

	// Get the SID for ALL_APPLICATION_PACKAGES
	PSID sid = nullptr;
	if (!ConvertStringSidToSid(TEXT("S-1-15-2-1"), &sid))
		return;

	EXPLICIT_ACCESS access = {};
	access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
	access.grfAccessMode = SET_ACCESS;
	access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
	access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
	access.Trustee.ptstrName = reinterpret_cast<LPTCH>(sid);

	// Update the DACL with the new entry
	if (SetEntriesInAcl(1, &access, old_acl, &new_acl) == ERROR_SUCCESS)
	{
		SetNamedSecurityInfo(path, SE_FILE_OBJECT, siInfo, NULL, NULL, new_acl, NULL);
		LocalFree(new_acl);
	}

	LocalFree(sid);
}

int wmain(int argc, wchar_t* argv[])
{
	wchar_t wcModulePath[MAX_PATH];
	GetModuleFileNameW(NULL, wcModulePath, _countof(wcModulePath) - 3); // Minus max required space for extension
	PathRenameExtension(wcModulePath, L".ini");
	
	wchar_t dll_name[MAX_PATH];
	wchar_t process_name[MAX_PATH];
	wchar_t app_id[MAX_PATH];
	GetPrivateProfileStringW(L"Injector", L"dll", L"", dll_name, MAX_PATH, wcModulePath);
	GetPrivateProfileStringW(L"Injector", L"process", L"", process_name, MAX_PATH, wcModulePath);
	GetPrivateProfileStringW(L"Injector", L"appid", L"", app_id, MAX_PATH, wcModulePath);

	wprintf(L"UWP Injector\n\n");

	if (wcslen(dll_name) == 0)
	{
		wprintf(L"DLL name cannot be empty. Aborting.\n");
		system("pause");
		return 1;
	}

	if (wcslen(process_name) == 0)
	{
		wprintf(L"Process name cannot be empty. Aborting.\n");
		system("pause");
		return 1;
	}

	if (wcslen(app_id) > 0)
	{
		wprintf(L"Attempting to launch %s through app ID %s ...\n", process_name, app_id);

		wchar_t command[MAX_PATH];
		wsprintfW(command, L"explorer.exe shell:AppsFolder\\%s", app_id);
		_wsystem(command);
	}
	else
	{
		wprintf(L"Waiting for a '%s' process to spawn ...\n", process_name);
	}

	DWORD pid = 0;

	// Wait for a process with the target name to spawn
	while (!pid)
	{
		const scoped_handle snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

		PROCESSENTRY32W process = { sizeof(process) };
		for (BOOL next = Process32FirstW(snapshot, &process); next; next = Process32NextW(snapshot, &process))
		{
			if (wcscmp(process.szExeFile, process_name) == 0)
			{
				pid = process.th32ProcessID;
				break;
			}
		}

		Sleep(1); // Sleep a bit to not overburden the CPU
	}

	printf("Found a matching process with PID %lu! Injecting DLL ... ", pid);

	// Wait just a little bit for the application to initialize
	Sleep(50);

	// Open target application process
	const scoped_handle remote_process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, FALSE, pid);
	if (remote_process == nullptr)
	{
		printf("\nFailed to open target application process!\n");
		return GetLastError();
	}

	// Check process architecture
	BOOL remote_is_wow64 = FALSE;
	IsWow64Process(remote_process, &remote_is_wow64);
#ifndef _WIN64
	if (remote_is_wow64 == FALSE)
#else
	if (remote_is_wow64 != FALSE)
#endif
	{
		printf("\nProcess architecture does not match injection tool! Cannot continue.\n");
		return ERROR_IMAGE_MACHINE_TYPE_MISMATCH;
	}

	loading_data arg;
	GetCurrentDirectoryW(MAX_PATH, arg.load_path);
	wcscat_s(arg.load_path, L"\\");
	wcscat_s(arg.load_path, dll_name);

	if (GetFileAttributesW(arg.load_path) == INVALID_FILE_ATTRIBUTES)
	{
		wprintf(L"\nFailed to find DLL at \"%s\"!\n", arg.load_path);
		return ERROR_FILE_NOT_FOUND;
	}

	// Make sure the DLL has permissions set up for "ALL_APPLICATION_PACKAGES"
	update_acl_for_uwp(arg.load_path);

	// This happens to work because kernel32.dll is always loaded to the same base address, so the address of 'LoadLibrary' is the same in the target application and the current one
	arg.GetLastError = GetLastError;
	arg.LoadLibraryW = LoadLibraryW;

	const auto loading_thread_func_size = 0;
	const auto load_param = VirtualAllocEx(remote_process, nullptr, loading_thread_func_size + sizeof(arg), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	const auto loading_thread_func_address = arg.LoadLibraryW;

	// Write thread entry point function and 'LoadLibrary' call argument to target application
	if (load_param == nullptr
		|| !WriteProcessMemory(remote_process, load_param, &arg, sizeof(arg), nullptr)
		)
	{
		printf("\nFailed to allocate and write 'LoadLibrary' argument in target application!\n");
		return GetLastError();
	}

	// Execute 'LoadLibrary' in target application
	const scoped_handle load_thread = CreateRemoteThread(remote_process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loading_thread_func_address), load_param, 0, nullptr);
	if (load_thread == nullptr)
	{
		printf("\nFailed to execute 'LoadLibrary' in target application!\n");
		return GetLastError();
	}

	// Wait for loading to finish and clean up parameter memory afterwards
	WaitForSingleObject(load_thread, INFINITE);
	VirtualFreeEx(remote_process, load_param, 0, MEM_RELEASE);

	// Thread thread exit code will contain the module handle
	if (DWORD exit_code; GetExitCodeThread(load_thread, &exit_code) &&
		exit_code != NULL)
	{
		printf("Succeeded!\n");
	}
	else
	{
		printf("\nFailed to load DLL in target application!\n");
		return ERROR_MOD_NOT_FOUND;
	}

	return 0;
}
