/*
 * sc4-region-sync launcher
 *
 * Settings.cpp - launcher.ini next to the exe, created with defaults on
 * first run, and the Settings dialog that edits it.
 */

#include "Launcher.h"
#include "Lan.h"
#include "resource.h"
#include <ShlObj.h>
#include <algorithm>
#include <cerrno>
#include <cwchar>
#include <cwctype>

namespace
{
	constexpr wchar_t kSection[] = L"Launcher";
	constexpr const wchar_t* kRenderers[] = { L"DirectX", L"Software", L"OpenGL" };

	using launcher::IniPath;

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

	// Whole string as a non-negative number, else false.
	bool ParseInt(const std::wstring& text, int& out)
	{
		wchar_t* end = nullptr;
		errno = 0;
		const long value = std::wcstol(text.c_str(), &end, 10);
		if (text.empty() || *end != L'\0' || errno == ERANGE || value < 0)
		{
			return false;
		}
		out = static_cast<int>(value);
		return true;
	}

	int ReadInt(const std::wstring& ini, const wchar_t* key, int fallback)
	{
		int value = 0;
		return ParseInt(ReadString(ini, key), value) ? value : fallback;
	}

	void CreateIni(const std::wstring& ini)
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
	}

	void WriteAll(const std::wstring& ini, const launcher::Settings& s)
	{
		const auto put = [&ini](const wchar_t* key, const std::wstring& value)
		{
			WritePrivateProfileStringW(kSection, key, value.c_str(), ini.c_str());
		};
		put(L"GameExe", s.gameExe);
		put(L"UserDirRoot", s.userDirRoot);
		put(L"StatusDirectory", s.statusDirectory);
		put(L"Width", std::to_wstring(s.width));
		put(L"Height", std::to_wstring(s.height));
		put(L"Depth", std::to_wstring(s.depth));
		put(L"Windowed", s.windowed ? L"1" : L"0");
		put(L"SkipIntro", s.skipIntro ? L"1" : L"0");
		put(L"SingleCpu", s.singleCpu ? L"1" : L"0");
		put(L"Renderer", s.renderer);
		put(L"ExtraArgs", s.extraArgs);
		put(L"WindowClass", s.windowClass);
		put(L"PerInstanceRamMb", std::to_wstring(s.perInstanceRamMb));
		put(L"ReserveCores", std::to_wstring(s.reserveCores));
		put(L"ReserveRamMb", std::to_wstring(s.reserveRamMb));
		put(L"MaxInstances", std::to_wstring(s.maxInstances));
	}

	// -r<W>x<H>x<D>
	bool ParseResolution(const std::wstring& token, launcher::Settings& s)
	{
		if (token.size() < 3 || token[0] != L'-' || std::towlower(token[1]) != L'r')
		{
			return false;
		}
		int v[3]{};
		const wchar_t* p = token.c_str() + 2;
		for (int i = 0; i < 3; ++i)
		{
			wchar_t* end = nullptr;
			const long n = std::wcstol(p, &end, 10);
			if (end == p || !std::iswdigit(*p) || (i < 2 ? std::towlower(*end) != L'x' : *end != L'\0'))
			{
				return false;
			}
			v[i] = static_cast<int>(n);
			p = end + 1;
		}
		s.width = v[0];
		s.height = v[1];
		s.depth = v[2];
		return true;
	}

	// Pre-dialog ExtraArgs carried the switches the dialog now owns; moves them
	// into their keys. A switch absent from the old args is off.
	bool Migrate(launcher::Settings& s)
	{
		launcher::Settings m = s;
		m.windowed = m.skipIntro = m.singleCpu = false;
		std::wstring rest;
		bool found = false;
		size_t pos = 0;
		while ((pos = s.extraArgs.find_first_not_of(L" \t", pos)) != std::wstring::npos)
		{
			const size_t end = std::min(s.extraArgs.find_first_of(L" \t", pos), s.extraArgs.size());
			const std::wstring t = s.extraArgs.substr(pos, end - pos);
			pos = end;
			bool known = true;
			if (_wcsicmp(t.c_str(), L"-w") == 0) m.windowed = true;
			else if (_wcsicmp(t.c_str(), L"-f") == 0) m.windowed = false;
			else if (_wcsicmp(t.c_str(), L"-CustomResolution:enabled") == 0) {}
			else if (_wcsicmp(t.c_str(), L"-intro:off") == 0) m.skipIntro = true;
			else if (_wcsicmp(t.c_str(), L"-CPUCount:1") == 0) m.singleCpu = true;
			else if (t.size() > 3 && _wcsnicmp(t.c_str(), L"-d:", 3) == 0) m.renderer = launcher::RendererName(t.substr(3));
			else known = ParseResolution(t, m);
			if (known)
			{
				found = true;
			}
			else
			{
				rest += (rest.empty() ? L"" : L" ") + t;
			}
		}
		if (!found)
		{
			return false;
		}
		m.extraArgs = rest;
		s = m;
		return true;
	}

	std::wstring GetText(HWND dlg, int id)
	{
		const HWND h = GetDlgItem(dlg, id);
		std::wstring text(static_cast<size_t>(GetWindowTextLengthW(h)) + 1, L'\0');
		text.resize(static_cast<size_t>(GetWindowTextW(h, text.data(), static_cast<int>(text.size()))));
		return text;
	}

	void InitDialog(HWND dlg, const launcher::Settings& s)
	{
		SetDlgItemTextW(dlg, IDC_GAME_EXE, s.gameExe.c_str());
		SetDlgItemTextW(dlg, IDC_USER_DIR_ROOT, s.userDirRoot.c_str());
		SetDlgItemInt(dlg, IDC_WIDTH, static_cast<UINT>(s.width), FALSE);
		SetDlgItemInt(dlg, IDC_HEIGHT, static_cast<UINT>(s.height), FALSE);
		SetDlgItemInt(dlg, IDC_DEPTH, static_cast<UINT>(s.depth), FALSE);
		CheckDlgButton(dlg, IDC_WINDOWED, s.windowed ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(dlg, IDC_SKIP_INTRO, s.skipIntro ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(dlg, IDC_SINGLE_CPU, s.singleCpu ? BST_CHECKED : BST_UNCHECKED);
		WPARAM selected = 0;
		const std::wstring renderer = launcher::RendererName(s.renderer);
		for (WPARAM i = 0; i < 3; ++i)
		{
			SendDlgItemMessageW(dlg, IDC_RENDERER, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kRenderers[i]));
			if (renderer == kRenderers[i])
			{
				selected = i;
			}
		}
		SendDlgItemMessageW(dlg, IDC_RENDERER, CB_SETCURSEL, selected, 0);
		SetDlgItemTextW(dlg, IDC_EXTRA_ARGS, s.extraArgs.c_str());
		SetDlgItemInt(dlg, IDC_PER_INSTANCE, static_cast<UINT>(s.perInstanceRamMb), FALSE);
		SetDlgItemInt(dlg, IDC_RESERVE_RAM, static_cast<UINT>(s.reserveRamMb), FALSE);
		SetDlgItemInt(dlg, IDC_RESERVE_CORES, static_cast<UINT>(s.reserveCores), FALSE);
		SetDlgItemInt(dlg, IDC_MAX_INSTANCES, static_cast<UINT>(s.maxInstances), FALSE);
	}

	// Reads the dialog into s. False after naming the first bad field.
	bool ReadDialog(HWND dlg, launcher::Settings& s)
	{
		struct Field { int id; const wchar_t* name; int* value; int min, max; const wchar_t* rule; };
		const Field fields[] = {
			{ IDC_WIDTH, L"Width", &s.width, 640, 7680, L"a whole number from 640 to 7680" },
			{ IDC_HEIGHT, L"Height", &s.height, 480, 4320, L"a whole number from 480 to 4320" },
			{ IDC_DEPTH, L"Colour depth", &s.depth, 16, 32, L"16 or 32" },
			{ IDC_PER_INSTANCE, L"Per-instance RAM", &s.perInstanceRamMb, 256, 1048576, L"a whole number from 256 to 1048576" },
			{ IDC_RESERVE_RAM, L"Reserve RAM", &s.reserveRamMb, 0, 1048576, L"a whole number from 0 to 1048576" },
			{ IDC_RESERVE_CORES, L"Reserve cores", &s.reserveCores, 0, 64, L"a whole number from 0 to 64" },
			{ IDC_MAX_INSTANCES, L"Max instances", &s.maxInstances, 1, 16, L"a whole number from 1 to 16" },
		};
		for (const Field& f : fields)
		{
			int v = 0;
			const bool ok = ParseInt(GetText(dlg, f.id), v) && v >= f.min && v <= f.max
				&& (f.id != IDC_DEPTH || v == 16 || v == 32);
			if (!ok)
			{
				launcher::ShowMessage(dlg, std::wstring(f.name) + L" must be " + f.rule + L".");
				SendMessageW(dlg, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(dlg, f.id)), TRUE);
				return false;
			}
			*f.value = v;
		}
		s.gameExe = GetText(dlg, IDC_GAME_EXE);
		s.userDirRoot = GetText(dlg, IDC_USER_DIR_ROOT);
		s.windowed = IsDlgButtonChecked(dlg, IDC_WINDOWED) == BST_CHECKED;
		s.skipIntro = IsDlgButtonChecked(dlg, IDC_SKIP_INTRO) == BST_CHECKED;
		s.singleCpu = IsDlgButtonChecked(dlg, IDC_SINGLE_CPU) == BST_CHECKED;
		const LRESULT sel = SendDlgItemMessageW(dlg, IDC_RENDERER, CB_GETCURSEL, 0, 0);
		s.renderer = kRenderers[sel >= 0 && sel < 3 ? sel : 0];
		s.extraArgs = GetText(dlg, IDC_EXTRA_ARGS);
		return true;
	}

	INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		try  // no exception may cross the dialog manager
		{
			if (msg == WM_INITDIALOG)
			{
				SetWindowLongPtrW(dlg, DWLP_USER, lParam);
				InitDialog(dlg, *reinterpret_cast<const launcher::Settings*>(lParam));
				return TRUE;
			}
			if (msg == WM_COMMAND && LOWORD(wParam) == IDOK)
			{
				launcher::Settings s = *reinterpret_cast<const launcher::Settings*>(GetWindowLongPtrW(dlg, DWLP_USER));
				if (ReadDialog(dlg, s))
				{
					const std::wstring ini = IniPath();
					CreateIni(ini);
					WriteAll(ini, s);
					EndDialog(dlg, 1);
				}
				return TRUE;
			}
			if (msg == WM_COMMAND && LOWORD(wParam) == IDCANCEL)
			{
				EndDialog(dlg, 0);
				return TRUE;
			}
		}
		catch (...)
		{
			if (msg == WM_COMMAND && LOWORD(wParam) == IDOK)
			{
				launcher::ShowMessage(dlg, L"Saving settings failed; launcher.ini may be partly written.", MB_ICONERROR);
			}
		}
		return FALSE;
	}
}

namespace launcher
{
	std::wstring IniPath()
	{
		wchar_t exe[MAX_PATH]{};
		const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring path(exe, n);
		const size_t slash = path.find_last_of(L'\\');
		return (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"launcher.ini";
	}

	std::wstring DocumentsFolder()
	{
		return Documents();
	}

	Settings LoadSettings()
	{
		const std::wstring docs = Documents();
		Settings d;
		d.gameExe = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SimCity 4 Deluxe\\Apps\\SimCity 4.exe";
		d.userDirRoot = docs + L"\\SimCity 4 Instances";
		d.statusDirectory = docs + L"\\SimCity 4\\Plugins\\RegionSync\\_instances";

		const std::wstring ini = IniPath();
		if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			CreateIni(ini);
			WriteAll(ini, d);
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
		s.width = ReadInt(ini, L"Width", d.width);
		s.height = ReadInt(ini, L"Height", d.height);
		s.depth = ReadInt(ini, L"Depth", d.depth);
		s.windowed = ReadInt(ini, L"Windowed", d.windowed) != 0;
		s.skipIntro = ReadInt(ini, L"SkipIntro", d.skipIntro) != 0;
		s.singleCpu = ReadInt(ini, L"SingleCpu", d.singleCpu) != 0;
		s.renderer = RendererName(ReadString(ini, L"Renderer"));
		s.extraArgs = ReadString(ini, L"ExtraArgs");
		s.windowClass = ReadString(ini, L"WindowClass");
		s.perInstanceRamMb = ReadInt(ini, L"PerInstanceRamMb", d.perInstanceRamMb);
		s.reserveCores = ReadInt(ini, L"ReserveCores", d.reserveCores);
		s.reserveRamMb = ReadInt(ini, L"ReserveRamMb", d.reserveRamMb);
		s.maxInstances = ReadInt(ini, L"MaxInstances", d.maxInstances);
		if (s.perInstanceRamMb == 0)
		{
			s.perInstanceRamMb = d.perInstanceRamMb;  // divisor
		}
		if (ReadString(ini, L"Width").empty() && Migrate(s))
		{
			WriteAll(ini, s);  // once: the Width key now exists
		}
		return s;
	}

	const wchar_t* RendererName(const std::wstring& renderer)
	{
		for (const wchar_t* r : kRenderers)
		{
			if (_wcsicmp(renderer.c_str(), r) == 0)
			{
				return r;
			}
		}
		return kRenderers[0];
	}

	bool EditSettings(HWND owner, const Settings& current)
	{
		ModalScope modal;
		const INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), owner,
			SettingsProc, reinterpret_cast<LPARAM>(&current));
		if (result == -1)
		{
			ShowMessage(owner, L"The Settings dialog failed to open, error " + std::to_wstring(GetLastError()) + L".", MB_ICONERROR);
		}
		return result == 1;
	}
}
