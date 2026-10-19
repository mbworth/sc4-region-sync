/*
 * sc4-region-sync launcher
 *
 * Launch.cpp - launch preconditions, free InstanceN, CreateProcessW.
 * Rules from docs/PLAN-LAUNCHER.md, "Launch".
 */

#include "Launcher.h"
#include "Lan.h"
#include <TlHelp32.h>
#include <algorithm>
#include <cwctype>

namespace
{
	bool FileExists(const std::wstring& path)
	{
		const DWORD attrs = GetFileAttributesW(path.c_str());
		return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
	}

	std::wstring Lower(std::wstring s)
	{
		for (wchar_t& c : s)
		{
			c = static_cast<wchar_t>(std::towlower(c));
		}
		return s;
	}

	std::wstring Folder(const std::wstring& path)
	{
		const size_t slash = path.find_last_of(L'\\');
		return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
	}

	bool SteamRunning()
	{
		const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE)
		{
			return true;  // cannot tell; let the launch decide
		}
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		bool found = false;
		for (BOOL ok = Process32FirstW(snap, &entry); ok && !found; ok = Process32NextW(snap, &entry))
		{
			found = _wcsicmp(entry.szExeFile, L"steam.exe") == 0;
		}
		CloseHandle(snap);
		return found;
	}

	// Instance<N> dirs under root, by N.
	std::vector<std::pair<int, std::wstring>> InstanceDirs(const std::wstring& root)
	{
		std::vector<std::pair<int, std::wstring>> dirs;
		WIN32_FIND_DATAW fd{};
		const HANDLE find = FindFirstFileW((root + L"\\Instance*").c_str(), &fd);
		if (find == INVALID_HANDLE_VALUE)
		{
			return dirs;
		}
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				continue;
			}
			const std::wstring name = fd.cFileName;
			const std::wstring digits = name.size() > 8 ? name.substr(8) : std::wstring();
			if (digits.empty() || digits.size() > 4 || !std::all_of(digits.begin(), digits.end(), iswdigit))
			{
				continue;
			}
			dirs.emplace_back(std::stoi(digits), root + L"\\" + name);
		} while (FindNextFileW(find, &fd));
		FindClose(find);
		std::sort(dirs.begin(), dirs.end());
		return dirs;
	}
}

namespace launcher
{
	bool LaunchInFlight(const PendingMap& pending)
	{
		return std::any_of(pending.begin(), pending.end(), [](const auto& p) { return !p.second.timedOut; });
	}

	void ShowMessage(HWND owner, const std::wstring& text, UINT icon)
	{
		ModalScope modal;
		MessageBoxW(owner, text.c_str(), L"SC4 Region Sync Launcher", MB_OK | icon);
	}

	bool Launch(HWND owner, const Settings& settings, const ResourceSample& resources,
		int running, const std::vector<std::wstring>& liveUserDirs, PendingMap& pending)
	{
		const std::wstring& exe = settings.gameExe;
		if (!FileExists(exe))
		{
			ShowMessage(owner, L"GameExe not found:\n" + exe + L"\n\nSet GameExe in launcher.ini.");
			return false;
		}
		const bool steam = Lower(exe).find(L"\\steamapps\\") != std::wstring::npos;
		if (steam && !FileExists(Folder(exe) + L"\\steam_appid.txt"))
		{
			ShowMessage(owner, L"steam_appid.txt is missing next to the game exe. Create it by hand with the text 24780 "
				L"(the launcher never writes under Program Files):\n" + Folder(exe));
			return false;
		}
		if (steam && !SteamRunning())
		{
			ShowMessage(owner, L"Steam is not running. Start Steam first.");
			return false;
		}
		if (!settings.windowed)
		{
			ShowMessage(owner, L"Turn on Windowed in Settings: a fullscreen instance switches display modes when raised.");
			return false;
		}
		if (LaunchInFlight(pending))
		{
			ShowMessage(owner, L"A launch is still in flight. Wait for it to write its status file.");
			return false;
		}
		if (resources.colour == ResourceSample::Colour::Red)
		{
			ShowMessage(owner, L"Not enough memory for another instance.\n\n" + resources.tooltip);
			return false;
		}
		if (running >= settings.maxInstances)
		{
			ShowMessage(owner, L"MaxInstances (" + std::to_wstring(settings.maxInstances) + L") reached.");
			return false;
		}

		std::wstring dir, label;
		std::wstring root = settings.userDirRoot;
		while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
		{
			root.pop_back();
		}
		for (const auto& [n, path] : InstanceDirs(root))
		{
			const std::wstring key = NormalizeDir(path);
			const bool used = pending.count(key) != 0
				|| std::find(liveUserDirs.begin(), liveUserDirs.end(), key) != liveUserDirs.end();
			if (!used)
			{
				dir = path;
				label = L"Instance" + std::to_wstring(n);
				break;
			}
		}
		if (dir.empty())
		{
			ShowMessage(owner, L"No free InstanceN folder under\n" + settings.userDirRoot
				+ L"\n\nRun scripts\\Setup-MultiInstance.ps1 -Setup -Instances 16.");
			return false;
		}

		// Built by hand: -UserDir needs its trailing backslash inside the quotes (Q5).
		std::wstring command = L"\"" + exe + L"\" -UserDir:\"" + dir + L"\\\""
			+ (settings.windowed ? L" -w" : L" -f") + L" -CustomResolution:enabled -r" + std::to_wstring(settings.width)
			+ L"x" + std::to_wstring(settings.height) + L"x" + std::to_wstring(settings.depth);
		if (settings.skipIntro)
		{
			command += L" -intro:off";
		}
		if (settings.singleCpu)
		{
			command += L" -CPUCount:1";
		}
		if (const std::wstring renderer = RendererName(settings.renderer); renderer != L"DirectX")
		{
			command += L" -d:" + renderer;
		}
		if (!settings.extraArgs.empty())
		{
			command += L" " + settings.extraArgs;
		}
		const std::wstring workDir = Folder(exe);
		STARTUPINFOW si{};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi{};
		if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
			workDir.c_str(), &si, &pi))
		{
			ShowMessage(owner, L"CreateProcessW failed, error " + std::to_wstring(GetLastError()) + L":\n" + command, MB_ICONERROR);
			return false;
		}
		CloseHandle(pi.hThread);
		pending[NormalizeDir(dir)] = PendingLaunch{ pi.dwProcessId, pi.hProcess, GetTickCount64(), false, label };
		return true;
	}
}
