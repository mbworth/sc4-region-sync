/*
 * sc4-region-sync launcher
 *
 * Settings.cpp - launcher.ini next to the exe, created with defaults on
 * first run.
 */

#include "Launcher.h"
#include <ShlObj.h>
#include <cwchar>

namespace
{
	constexpr wchar_t kSection[] = L"Launcher";

	std::wstring IniPath()
	{
		wchar_t exe[MAX_PATH]{};
		const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring path(exe, n);
		const size_t slash = path.find_last_of(L'\\');
		return (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"launcher.ini";
	}

	std::wstring Documents()
	{
		wchar_t path[MAX_PATH]{};
		return SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, path)) ? path : L"";
	}

	std::wstring ReadString(const std::wstring& ini, const wchar_t* key)
	{
		wchar_t buffer[2048]{};
		GetPrivateProfileStringW(kSection, key, L"", buffer, 2048, ini.c_str());
		return buffer;
	}

	int ReadInt(const std::wstring& ini, const wchar_t* key, int fallback)
	{
		const std::wstring text = ReadString(ini, key);
		wchar_t* end = nullptr;
		const long value = std::wcstol(text.c_str(), &end, 10);
		return text.empty() || *end != L'\0' || value < 0 ? fallback : static_cast<int>(value);
	}

	void WriteDefaults(const std::wstring& ini, const launcher::Settings& d)
	{
		// A UTF-16 BOM makes WritePrivateProfileStringW keep non-ASCII paths.
		const HANDLE file = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			return;
		}
		const wchar_t bom = 0xFEFF;
		DWORD written = 0;
		WriteFile(file, &bom, sizeof(bom), &written, nullptr);
		CloseHandle(file);

		const auto put = [&ini](const wchar_t* key, const std::wstring& value)
		{
			WritePrivateProfileStringW(kSection, key, value.c_str(), ini.c_str());
		};
		put(L"GameExe", d.gameExe);
		put(L"UserDirRoot", d.userDirRoot);
		put(L"StatusDirectory", d.statusDirectory);
		put(L"ExtraArgs", d.extraArgs);
		put(L"WindowClass", d.windowClass);
		put(L"PerInstanceRamMb", std::to_wstring(d.perInstanceRamMb));
		put(L"ReserveCores", std::to_wstring(d.reserveCores));
		put(L"ReserveRamMb", std::to_wstring(d.reserveRamMb));
		put(L"MaxInstances", std::to_wstring(d.maxInstances));
	}
}

namespace launcher
{
	Settings LoadSettings()
	{
		const std::wstring docs = Documents();
		Settings d;
		d.gameExe = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SimCity 4 Deluxe\\Apps\\SimCity 4.exe";
		d.userDirRoot = docs + L"\\SimCity 4 Instances";
		d.statusDirectory = docs + L"\\SimCity 4\\Plugins\\RegionSync\\_instances";
		d.extraArgs = L"-w -CustomResolution:enabled -r1600x900x32 -intro:off";

		const std::wstring ini = IniPath();
		if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			WriteDefaults(ini, d);
		}

		Settings s;
		const auto str = [&ini](const wchar_t* key, const std::wstring& fallback)
		{
			const std::wstring v = ReadString(ini, key);
			return v.empty() ? fallback : v;
		};
		s.gameExe = str(L"GameExe", d.gameExe);
		for (wchar_t& c : s.gameExe)
		{
			if (c == L'/') c = L'\\';
		}
		s.userDirRoot = str(L"UserDirRoot", d.userDirRoot);
		s.statusDirectory = str(L"StatusDirectory", d.statusDirectory);
		s.extraArgs = str(L"ExtraArgs", d.extraArgs);
		s.windowClass = ReadString(ini, L"WindowClass");
		s.perInstanceRamMb = ReadInt(ini, L"PerInstanceRamMb", d.perInstanceRamMb);
		s.reserveCores = ReadInt(ini, L"ReserveCores", d.reserveCores);
		s.reserveRamMb = ReadInt(ini, L"ReserveRamMb", d.reserveRamMb);
		s.maxInstances = ReadInt(ini, L"MaxInstances", d.maxInstances);
		if (s.perInstanceRamMb == 0)
		{
			s.perInstanceRamMb = d.perInstanceRamMb;  // divisor
		}
		return s;
	}
}
