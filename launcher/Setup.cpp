/*
 * sc4-region-sync launcher
 *
 * Setup.cpp - the Setup dialog: InstanceN folders, Regions and Plugins junctions, steam_appid.txt.
 * Its only delete is RemoveDirectoryW on the empty folder it just made when the junction fails.
 */

#include "Launcher.h"
#include "Lan.h"
#include "resource.h"
#include <CommCtrl.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <winioctl.h>
#include <memory>

namespace
{
	using namespace launcher;

	constexpr int kMaxCount = 16;

	struct SetupOptions
	{
		std::wstring gameExe, source, root, currentRoot, currentStatus;
		int count = 0;
		bool appId = false;
	};

	// REPARSE_DATA_BUFFER, mount point case (ntifs.h is not in the SDK).
	struct MountPointBuffer
	{
		DWORD tag;
		WORD dataLength;
		WORD reserved;
		WORD substituteOffset;
		WORD substituteLength;
		WORD printOffset;
		WORD printLength;
		wchar_t path[(MAXIMUM_REPARSE_DATA_BUFFER_SIZE - 16) / sizeof(wchar_t)];
	};
	constexpr DWORD kReparseHeader = 8;  // tag, dataLength, reserved
	constexpr DWORD kMountHeader = 8;  // dataLength counts from substituteOffset

	bool IsReparse(const std::wstring& path)
	{
		const DWORD a = GetFileAttributesW(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
	}

	std::wstring TrimSlash(std::wstring path)
	{
		while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
		{
			path.pop_back();
		}
		return path;
	}

	std::wstring FullPath(const std::wstring& path)
	{
		if (path.empty())
		{
			return path;
		}
		const DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
		std::wstring full(n, L'\0');
		const DWORD m = n ? GetFullPathNameW(path.c_str(), n, full.data(), nullptr) : 0;
		if (m == 0 || m >= n)
		{
			return TrimSlash(path);
		}
		full.resize(m);
		return TrimSlash(full);
	}

	// Inside or equal, on folder boundaries, ignoring case.
	bool IsInside(const std::wstring& path, const std::wstring& folder)
	{
		const std::wstring p = path + L"\\", f = folder + L"\\";
		return p.size() >= f.size() && _wcsnicmp(p.c_str(), f.c_str(), f.size()) == 0;
	}

	// Full path with links resolved, no \\?\ and no trailing backslash. A missing
	// tail is kept and joined to its nearest existing ancestor. False for UNC.
	bool Resolve(const std::wstring& path, std::wstring& out)
	{
		std::wstring current = FullPath(path), tail;
		if (current.size() < 2 || current[1] != L':')
		{
			return false;
		}
		for (;;)
		{
			const std::wstring open = current.size() == 2 ? current + L"\\" : current;
			const HANDLE h = CreateFileW(open.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
			if (h != INVALID_HANDLE_VALUE)
			{
				std::wstring s(32768, L'\0');
				const DWORD n = GetFinalPathNameByHandleW(h, s.data(), 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
				CloseHandle(h);
				if (n == 0 || n >= 32768)
				{
					return false;
				}
				s.resize(n);
				if (s.rfind(L"\\\\?\\", 0) == 0)
				{
					s.erase(0, 4);
				}
				if (s.rfind(L"UNC\\", 0) == 0)
				{
					return false;
				}
				out = TrimSlash(s) + tail;
				return true;
			}
			const DWORD error = GetLastError();
			const size_t slash = current.find_last_of(L'\\');
			if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) || slash == std::wstring::npos)
			{
				return false;
			}
			tail = current.substr(slash) + tail;
			current.resize(slash);
		}
	}

	bool SamePath(const std::wstring& a, const std::wstring& b)
	{
		std::wstring ra, rb;
		return Resolve(a, ra) && Resolve(b, rb) && _wcsicmp(ra.c_str(), rb.c_str()) == 0;
	}

	bool DriveOk(const std::wstring& resolved)
	{
		const UINT type = GetDriveTypeW((resolved.substr(0, 2) + L"\\").c_str());
		return type == DRIVE_FIXED || type == DRIVE_REMOVABLE;
	}

	HANDLE OpenReparse(const std::wstring& path, DWORD access)
	{
		return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	}

	// The reparse tag at path and, for a junction, its target without \??\.
	bool ReadReparse(const std::wstring& path, DWORD& tag, std::wstring& target)
	{
		const HANDLE h = OpenReparse(path, FILE_READ_ATTRIBUTES);
		if (h == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		auto buffer = std::make_unique<MountPointBuffer>();
		DWORD bytes = 0;
		const BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.get(), sizeof(MountPointBuffer), &bytes, nullptr);
		CloseHandle(h);
		if (!ok || bytes < kReparseHeader)
		{
			return false;
		}
		tag = buffer->tag;
		target.clear();
		if (tag != IO_REPARSE_TAG_MOUNT_POINT)
		{
			return true;
		}
		if (bytes < kReparseHeader + kMountHeader
			|| kMountHeader + buffer->substituteOffset + buffer->substituteLength > bytes - kReparseHeader)
		{
			return false;
		}
		target.assign(buffer->path + buffer->substituteOffset / sizeof(wchar_t), buffer->substituteLength / sizeof(wchar_t));
		if (target.rfind(L"\\??\\", 0) == 0)
		{
			target.erase(0, 4);
		}
		target = TrimSlash(target);
		return true;
	}

	// Turns the empty folder at path into a junction with FSCTL_SET_REPARSE_POINT.
	DWORD SetJunction(const std::wstring& path, const std::wstring& target)
	{
		const std::wstring substitute = L"\\??\\" + target;
		const size_t chars = substitute.size() + 1 + target.size() + 1;
		if (chars * sizeof(wchar_t) > sizeof(MountPointBuffer::path))
		{
			return ERROR_FILENAME_EXCED_RANGE;
		}
		auto buffer = std::make_unique<MountPointBuffer>();
		buffer->tag = IO_REPARSE_TAG_MOUNT_POINT;
		buffer->substituteOffset = 0;
		buffer->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
		buffer->printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
		buffer->printLength = static_cast<WORD>(target.size() * sizeof(wchar_t));
		buffer->dataLength = static_cast<WORD>(kMountHeader + chars * sizeof(wchar_t));
		substitute.copy(buffer->path, substitute.size());
		target.copy(buffer->path + substitute.size() + 1, target.size());

		const HANDLE h = OpenReparse(path, GENERIC_WRITE);
		if (h == INVALID_HANDLE_VALUE)
		{
			return GetLastError();
		}
		DWORD bytes = 0;
		const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buffer.get(), kReparseHeader + buffer->dataLength,
			nullptr, 0, &bytes, nullptr);
		const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
		CloseHandle(h);
		return error;
	}

	std::wstring Code(DWORD error)
	{
		return L"error " + std::to_wstring(error);
	}

	// The log list and progress bar of an open Setup dialog.
	struct Log
	{
		HWND dlg = nullptr;
		int errors = 0;
		int width = 0;

		void Add(const std::wstring& text)
		{
			const HWND list = GetDlgItem(dlg, IDC_SETUP_LOG);
			const LRESULT i = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
			const HDC dc = GetDC(list);
			const HGDIOBJ old = SelectObject(dc, reinterpret_cast<HGDIOBJ>(SendMessageW(list, WM_GETFONT, 0, 0)));
			SIZE size{};
			GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
			SelectObject(dc, old);
			ReleaseDC(list, dc);
			if (size.cx + 8 > width)
			{
				width = size.cx + 8;
				SendMessageW(list, LB_SETHORIZONTALEXTENT, static_cast<WPARAM>(width), 0);
			}
			SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(i), 0);
			RedrawWindow(dlg, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
		}

		void Done(const std::wstring& text) { Add(L"Done: " + text); }
		void Skipped(const std::wstring& text) { Add(L"Skipped: " + text); }
		void Error(const std::wstring& text) { errors++; Add(L"Error: " + text); }

		void Step()
		{
			SendDlgItemMessageW(dlg, IDC_SETUP_PROGRESS, PBM_STEPIT, 0, 0);
			RedrawWindow(dlg, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
		}
	};

	bool MakeFolder(Log& log, const std::wstring& path)
	{
		if (CreateDirectoryW(path.c_str(), nullptr))
		{
			log.Done(L"Created " + path);
			return true;
		}
		const DWORD error = GetLastError();
		if (error == ERROR_ALREADY_EXISTS && !FileExists(path))
		{
			log.Skipped(path + L" already there");
			return true;
		}
		log.Error(L"Could not create " + path + L" (" + Code(error) + L")");
		return false;
	}

	// False when dst is not (or could not be made) our junction to target.
	bool Link(Log& log, const std::wstring& dst, const std::wstring& target)
	{
		const DWORD attrs = GetFileAttributesW(dst.c_str());
		DWORD tag = 0;
		std::wstring existing;
		if (attrs == INVALID_FILE_ATTRIBUTES)
		{
			if (!CreateDirectoryW(dst.c_str(), nullptr))
			{
				const DWORD error = GetLastError();
				log.Error(L"Could not create " + dst + L" (" + Code(error) + L")");
				return false;
			}
			const DWORD error = SetJunction(dst, target);
			if (error != ERROR_SUCCESS)
			{
				RemoveDirectoryW(dst.c_str());
				log.Error(L"Could not link " + dst + L" (" + Code(error) + L")");
				return false;
			}
			if (!ReadReparse(dst, tag, existing) || existing.empty())
			{
				log.Error(L"Linked " + dst + L", but its target is unreadable");
				return false;
			}
			log.Done(L"Linked " + dst + L" -> " + existing);
			return true;
		}
		if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT))
		{
			log.Error(dst + ((attrs & FILE_ATTRIBUTE_DIRECTORY) ? L" exists and is a real folder" : L" exists and is a file")
				+ L"; left alone. Move it away and run Setup again.");
			return false;
		}
		if (!ReadReparse(dst, tag, existing))
		{
			log.Error(dst + L" is a link Setup cannot read; left alone.");
			return false;
		}
		if (tag != IO_REPARSE_TAG_MOUNT_POINT)
		{
			wchar_t hex[16]{};
			swprintf_s(hex, L"0x%08lX", tag);
			log.Error(dst + L" is a reparse point of type " + hex + L" (cloud or other link); left alone.");
			return false;
		}
		if (SamePath(existing, target))
		{
			log.Skipped(dst + L" already linked");
			return true;
		}
		log.Error(dst + L" is a junction to " + existing + L", not " + target + L"; left alone.");
		return false;
	}

	void SetupInstance(Log& log, const std::wstring& dir, const std::wstring& source)
	{
		if (IsReparse(dir))
		{
			log.Error(dir + L" is a link; left alone.");
			return;
		}
		if (!MakeFolder(log, dir) || !MakeFolder(log, dir + L"\\Albums") || !Link(log, dir + L"\\Regions", source + L"\\Regions"))
		{
			log.Skipped(L"Rest of " + dir + L" not set up");
			return;
		}
		if (GetFileAttributesW((source + L"\\Plugins").c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			log.Skipped(L"No Plugins folder in " + source + L", not linked");
		}
		else if (!Link(log, dir + L"\\Plugins", source + L"\\Plugins"))
		{
			log.Skipped(L"Rest of " + dir + L" not set up");
			return;
		}
		const std::wstring cfg = source + L"\\SimCity 4.cfg";
		const std::wstring copy = dir + L"\\SimCity 4.cfg";
		if (!FileExists(cfg))
		{
			log.Skipped(L"No SimCity 4.cfg in " + source + L"; the game makes a fresh one");
		}
		else if (CopyFileW(cfg.c_str(), copy.c_str(), TRUE))
		{
			log.Done(L"Copied " + copy);
		}
		else if (const DWORD error = GetLastError(); error == ERROR_FILE_EXISTS)
		{
			log.Skipped(copy + L" already there, kept");
		}
		else
		{
			log.Error(L"Could not copy " + copy + L" (" + Code(error) + L")");
		}
	}

	void WriteAppIdElevated(Log& log, const std::wstring& exeFolder)
	{
		const std::wstring file = exeFolder + L"\\steam_appid.txt";
		if (FileExists(file))
		{
			log.Skipped(file + L" already there");
			return;
		}
		wchar_t self[MAX_PATH]{};
		const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
		{
			log.Error(L"steam_appid.txt: the launcher's own path is too long; create it by hand with the text 24780 in " + exeFolder);
			return;
		}
		const std::wstring params = L"--write-appid \"" + exeFolder + L"\"";
		SHELLEXECUTEINFOW info{ sizeof(info) };
		info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
		info.hwnd = log.dlg;
		info.lpVerb = L"runas";
		info.lpFile = self;
		info.lpParameters = params.c_str();
		info.nShow = SW_HIDE;
		if (!ShellExecuteExW(&info))
		{
			const DWORD error = GetLastError();
			if (error == ERROR_CANCELLED)
			{
				log.Skipped(L"steam_appid.txt: you declined the prompt, create it by hand with the text 24780 in " + exeFolder);
			}
			else
			{
				log.Error(L"steam_appid.txt: could not start the elevated launcher (" + Code(error) + L")");
			}
			return;
		}
		if (!info.hProcess)
		{
			log.Error(L"steam_appid.txt: no process handle from the elevated launcher");
			return;
		}
		const DWORD wait = WaitForSingleObject(info.hProcess, 30000);
		DWORD exitCode = 0;
		GetExitCodeProcess(info.hProcess, &exitCode);
		CloseHandle(info.hProcess);
		if (wait == WAIT_TIMEOUT)
		{
			log.Skipped(L"steam_appid.txt: still running, check for " + file + L" before launching");
		}
		else if ((exitCode == 0 || exitCode == 1) && FileExists(file))
		{
			log.Done(L"Created " + file);
		}
		else
		{
			log.Error(L"steam_appid.txt: the elevated launcher exited with code " + std::to_wstring(exitCode));
		}
	}

	// Logs every problem; true when nothing blocks the run. Fills the resolved paths.
	bool Validate(const SetupOptions& o, Log& log, std::wstring& source, std::wstring& root)
	{
		const int before = log.errors;
		if (o.appId && !FileExists(o.gameExe))
		{
			log.Error(L"Game exe not found: " + o.gameExe + L". Set it in Settings.");
		}
		if (o.count < 1 || o.count > kMaxCount)
		{
			log.Error(L"Number of instances must be a whole number from 1 to 16.");
		}
		if (!Resolve(o.source, source) || !DriveOk(source))
		{
			log.Error(L"Your SimCity 4 folder must be on a local fixed or removable drive: " + o.source);
			return false;
		}
		if (!Resolve(o.root, root) || !DriveOk(root))
		{
			log.Error(L"The instances folder must be on a local fixed or removable drive: " + o.root);
			return false;
		}
		const DWORD regions = GetFileAttributesW((source + L"\\Regions").c_str());
		if (regions == INVALID_FILE_ATTRIBUTES || !(regions & FILE_ATTRIBUTE_DIRECTORY))
		{
			log.Error(L"No Regions folder in " + source + L". Is this your SimCity 4 folder?");
		}
		for (const wchar_t* sub : { L"\\Regions", L"\\Plugins" })
		{
			if (IsReparse(source + sub))
			{
				log.Error(source + sub + L" is itself a link; Setup only links to real folders.");
			}
		}
		if (IsInside(root, source) || IsInside(source, root))
		{
			log.Error(L"The instances folder and your SimCity 4 folder must not contain each other.");
		}
		if (IsReparse(FullPath(o.root)))
		{
			log.Error(L"The instances folder is a link; pick a real folder.");
		}
		if ((root + L"\\Instance16\\Albums").size() >= 248)
		{
			log.Error(L"The instances folder path is too long; pick a shorter one such as C:\\SC4 Instances.");
		}
		return log.errors == before;
	}

	bool WriteIni(Log& log, const wchar_t* key, const std::wstring& value)
	{
		if (WritePrivateProfileStringW(kIniSection, key, value.c_str(), IniPath().c_str()))
		{
			log.Done(L"Wrote " + std::wstring(key) + L"=" + value + L" to launcher.ini");
			return true;
		}
		const DWORD error = GetLastError();
		log.Error(L"Could not write " + std::wstring(key) + L" to launcher.ini (" + Code(error) + L")");
		return false;
	}

	// False when validation or the root folder stopped it before any instance. iniChanged is set when launcher.ini was written.
	bool RunSetup(const SetupOptions& o, Log& log, bool& iniChanged)
	{
		std::wstring source, root;
		if (!Validate(o, log, source, root))
		{
			return false;
		}
		SendDlgItemMessageW(log.dlg, IDC_SETUP_PROGRESS, PBM_SETRANGE32, 0, o.count + 2);
		SendDlgItemMessageW(log.dlg, IDC_SETUP_PROGRESS, PBM_SETSTEP, 1, 0);

		const int rootResult = SHCreateDirectoryExW(nullptr, root.c_str(), nullptr);
		if (rootResult == ERROR_SUCCESS)
		{
			log.Done(L"Created " + root);
		}
		else if (rootResult == ERROR_ALREADY_EXISTS || rootResult == ERROR_FILE_EXISTS)
		{
			log.Skipped(root + L" already there");
		}
		else
		{
			log.Error(L"Could not create " + root + L" (" + Code(static_cast<DWORD>(rootResult)) + L")");
			return false;
		}
		log.Step();

		for (int i = 1; i <= o.count; ++i)
		{
			SetupInstance(log, root + L"\\Instance" + std::to_wstring(i), source);
			log.Step();
		}

		if (o.appId)
		{
			WriteAppIdElevated(log, launcher::Folder(o.gameExe));
		}

		std::wstring rootNow;
		if (!Resolve(o.currentRoot, rootNow) || _wcsicmp(rootNow.c_str(), root.c_str()) != 0)
		{
			iniChanged = WriteIni(log, L"UserDirRoot", root) || iniChanged;
		}
		const std::wstring documents = DocumentsFolder() + L"\\SimCity 4";
		const std::wstring status = (SamePath(documents, source) ? documents : source) + L"\\Plugins\\RegionSync\\_instances";
		if (!SamePath(o.currentStatus, status))
		{
			iniChanged = WriteIni(log, L"StatusDirectory", status) || iniChanged;
		}
		if (!iniChanged)
		{
			log.Skipped(L"launcher.ini already points at " + root);
		}
		log.Step();
		return true;
	}

	struct DialogState
	{
		const Settings* settings = nullptr;
		bool instancesRunning = false;
		bool changed = false;
	};

	bool UnderOneDrive(const std::wstring& path)
	{
		for (const wchar_t* name : { L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial" })
		{
			wchar_t value[MAX_PATH]{};
			const DWORD n = GetEnvironmentVariableW(name, value, MAX_PATH);
			if (n > 0 && n < MAX_PATH && IsInside(FullPath(path), FullPath(value)))
			{
				return true;
			}
		}
		return false;
	}

	bool AppIdShown(HWND dlg)
	{
		return IsWindowVisible(GetDlgItem(dlg, IDC_SETUP_APPID)) && IsDlgButtonChecked(dlg, IDC_SETUP_APPID) == BST_CHECKED;
	}

	void UpdatePlan(HWND dlg)
	{
		const std::wstring source = TrimSlash(GetText(dlg, IDC_SETUP_SOURCE));
		const std::wstring root = TrimSlash(GetText(dlg, IDC_SETUP_ROOT));
		const std::wstring exe = GetText(dlg, IDC_SETUP_EXE);
		BOOL ok = FALSE;
		const UINT count = GetDlgItemInt(dlg, IDC_SETUP_COUNT, &ok, FALSE);
		const std::wstring n = ok ? std::to_wstring(count) : L"N";
		int step = 1;
		const auto line = [&step](const std::wstring& text)
		{
			return std::to_wstring(step++) + L". " + text + L"\r\n";
		};
		std::wstring plan = line(L"Create " + root + L"\\Instance1 to Instance" + n + L".");
		plan += line(L"In each: an empty Albums folder; junctions Regions -> " + source + L"\\Regions and Plugins -> "
			+ source + L"\\Plugins (the game sees your real region and plugins; nothing is copied); a copy of "
			L"SimCity 4.cfg.");
		if (AppIdShown(dlg))
		{
			plan += line(L"Create " + launcher::Folder(exe) + L"\\steam_appid.txt with the text 24780, through an elevated copy "
				L"of the launcher, so a second copy of the game does not relaunch through Steam.");
		}
		plan += line(L"Write the instances folder into launcher.ini. Nothing in your SimCity 4 folder is changed.");
		plan += L"Folders and files that already exist are left alone and reported.";
		if (UnderOneDrive(source) || UnderOneDrive(root))
		{
			plan += L"\r\n\r\nWarning: a folder above is under OneDrive. Junctions sync badly there and on-demand "
				L"files stall the game; an instances folder such as C:\\SC4 Instances is safer.";
		}
		SetDlgItemTextW(dlg, IDC_SETUP_PLAN, plan.c_str());
	}

	void OnStart(HWND dlg, DialogState& state)
	{
		ShowWindow(GetDlgItem(dlg, IDC_SETUP_PROGRESS), SW_SHOW);
		ShowWindow(GetDlgItem(dlg, IDC_SETUP_LOG), SW_SHOW);
		SendDlgItemMessageW(dlg, IDC_SETUP_LOG, LB_RESETCONTENT, 0, 0);
		SendDlgItemMessageW(dlg, IDC_SETUP_PROGRESS, PBM_SETPOS, 0, 0);
		Log log;
		log.dlg = dlg;
		if (state.instancesRunning)
		{
			log.Add(L"SimCity 4 is running from this launcher. Close every instance, then open Setup again.");
			return;
		}
		BOOL ok = FALSE;
		SetupOptions o;
		o.gameExe = state.settings->gameExe;
		o.source = GetText(dlg, IDC_SETUP_SOURCE);
		o.root = GetText(dlg, IDC_SETUP_ROOT);
		o.currentRoot = state.settings->userDirRoot;
		o.currentStatus = state.settings->statusDirectory;
		o.count = static_cast<int>(GetDlgItemInt(dlg, IDC_SETUP_COUNT, &ok, FALSE));
		if (!ok)
		{
			o.count = 0;
		}
		o.appId = AppIdShown(dlg);

		if (!RunSetup(o, log, state.changed))
		{
			log.Add(L"Nothing was changed. Fix the above and click Start again.");
			return;
		}
		log.Add(log.errors == 0 ? L"Setup finished." : L"Setup finished with " + std::to_wstring(log.errors) + L" error(s); see above.");
		for (const int id : { IDC_SETUP_START, IDC_SETUP_SOURCE, IDC_SETUP_ROOT, IDC_SETUP_COUNT, IDC_SETUP_APPID })
		{
			EnableWindow(GetDlgItem(dlg, id), FALSE);
		}
		SetDlgItemTextW(dlg, IDCANCEL, L"Close");
		SendMessageW(dlg, DM_SETDEFID, IDCANCEL, 0);
		SendMessageW(dlg, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(dlg, IDCANCEL)), TRUE);
	}

	void InitDialog(HWND dlg, const Settings& s)
	{
		SetDlgItemTextW(dlg, IDC_SETUP_EXE, s.gameExe.c_str());
		SetDlgItemTextW(dlg, IDC_SETUP_SOURCE, (DocumentsFolder() + L"\\SimCity 4").c_str());
		SetDlgItemTextW(dlg, IDC_SETUP_ROOT, s.userDirRoot.c_str());
		SetDlgItemInt(dlg, IDC_SETUP_COUNT, 4, FALSE);
		const bool appId = IsSteamExe(s.gameExe) && !FileExists(launcher::Folder(s.gameExe) + L"\\steam_appid.txt");
		ShowWindow(GetDlgItem(dlg, IDC_SETUP_APPID), appId ? SW_SHOW : SW_HIDE);
		CheckDlgButton(dlg, IDC_SETUP_APPID, appId ? BST_CHECKED : BST_UNCHECKED);
		ShowWindow(GetDlgItem(dlg, IDC_SETUP_PROGRESS), SW_HIDE);
		ShowWindow(GetDlgItem(dlg, IDC_SETUP_LOG), SW_HIDE);
		UpdatePlan(dlg);
	}

	INT_PTR CALLBACK SetupProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		try  // no exception may cross the dialog manager
		{
			if (msg == WM_INITDIALOG)
			{
				SetWindowLongPtrW(dlg, DWLP_USER, lParam);
				InitDialog(dlg, *reinterpret_cast<DialogState*>(lParam)->settings);
				return TRUE;
			}
			if (msg != WM_COMMAND)
			{
				return FALSE;
			}
			DialogState& state = *reinterpret_cast<DialogState*>(GetWindowLongPtrW(dlg, DWLP_USER));
			const int id = LOWORD(wParam);
			if ((HIWORD(wParam) == EN_CHANGE && (id == IDC_SETUP_SOURCE || id == IDC_SETUP_ROOT || id == IDC_SETUP_COUNT))
				|| (HIWORD(wParam) == BN_CLICKED && id == IDC_SETUP_APPID))
			{
				UpdatePlan(dlg);
				return TRUE;
			}
			if (id == IDC_SETUP_START)
			{
				OnStart(dlg, state);
				return TRUE;
			}
			if (id == IDCANCEL)
			{
				EndDialog(dlg, state.changed ? 1 : 0);
				return TRUE;
			}
		}
		catch (...)
		{
			if (msg == WM_COMMAND && LOWORD(wParam) == IDC_SETUP_START)
			{
				ShowMessage(dlg, L"Setup stopped on an internal error; the log above shows what was done.", MB_ICONERROR);
			}
		}
		return FALSE;
	}
}

namespace launcher
{
	bool RunSetupDialog(HWND owner, const Settings& current, bool instancesRunning)
	{
		ModalScope modal;
		DialogState state;
		state.settings = &current;
		state.instancesRunning = instancesRunning;
		const INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETUP), owner,
			SetupProc, reinterpret_cast<LPARAM>(&state));
		if (result == -1)
		{
			ShowMessage(owner, L"The Setup dialog failed to open, error " + std::to_wstring(GetLastError()) + L".", MB_ICONERROR);
		}
		return state.changed;
	}

	int WriteAppId(const std::wstring& folder)
	{
		std::wstring resolved;
		if (!Resolve(folder, resolved) || !IsSteamExe(resolved + L"\\") || !FileExists(resolved + L"\\SimCity 4.exe"))
		{
			return 2;
		}
		const std::wstring path = resolved + L"\\steam_appid.txt";
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			return GetLastError() == ERROR_FILE_EXISTS ? 1 : 3;
		}
		const char text[] = "24780";
		DWORD written = 0;
		const BOOL ok = WriteFile(file, text, 5, &written, nullptr);
		CloseHandle(file);
		return ok && written == 5 ? 0 : 3;
	}
}
