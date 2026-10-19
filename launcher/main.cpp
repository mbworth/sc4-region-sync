/*
 * sc4-region-sync launcher
 *
 * main.cpp - the window: instance list or grid of DWM thumbnails, Launch,
 * Refresh and End Task, resource line. Discovery every 1 s, resource
 * sampling every 5 s (docs/PLAN-LAUNCHER.md). Host, Join, Re-sync, the LAN
 * strip and banner (docs/PLAN-LAN.md section 6).
 */

#include "Launcher.h"
#include "Lan.h"
#include <CommCtrl.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <TlHelp32.h>
#include <algorithm>
#include <set>

namespace
{
	using namespace launcher;

	constexpr UINT_PTR kDiscoveryTimer = 1;
	constexpr UINT_PTR kResourceTimer = 2;
	constexpr int kCmdLaunch = 101;
	constexpr int kCmdRefresh = 102;
	constexpr int kCmdList = 103;
	constexpr int kCmdGrid = 104;
	constexpr int kCmdSettings = 105;
	constexpr int kCmdEndTask = 106;
	constexpr int kCmdHost = 107;
	constexpr int kCmdJoin = 108;
	constexpr int kCmdResync = 109;
	constexpr int kCmdNewCode = 110;
	constexpr int kColumns = 5;
	constexpr size_t kMaxCells = 16;
	constexpr ULONGLONG kPendingTimeoutMs = 60000;
	constexpr ULONGLONG kOrphanAgeMs = 90000;

	struct Row
	{
		DWORD pid = 0;
		uint64_t start = 0;
		std::string host;     // empty for pending and orphan rows (this PC)
		bool remote = false;  // the other PC's instance: read-only
		HWND window = nullptr;
		bool conflict = false;
		bool orphan = false;  // game process with no status file and no pending launch
		std::wstring label, hostText, state, tile, pidText;
		std::string Key() const { return host + "|" + std::to_string(pid) + "|" + std::to_string(start); }
	};

	struct Thumb
	{
		HTHUMBNAIL handle = nullptr;
		RECT dest{ -1, -1, -1, -1 };  // last applied
		bool visible = false;
		bool failed = false;          // register failed; retried only for a new HWND
	};

	enum class Strip { Normal, Conflict, Grey };

	struct Cell
	{
		RECT cell{}, strip{}, area{};
		HWND window = nullptr;
		Strip kind = Strip::Normal;
		std::wstring caption;
	};

	struct App
	{
		HWND main = nullptr, list = nullptr, toolbar = nullptr, status = nullptr, tooltip = nullptr, banner = nullptr;
		LanController lan;
		LanStatusPart lanStatus;
		uint64_t lanVersion = 0;
		HFONT font = nullptr;
		UINT dpi = 96;
		Settings settings;
		PendingMap pending;
		ResourceSample resources;
		std::vector<Row> rows;
		std::vector<std::wstring> liveUserDirs;  // normalised
		std::map<std::wstring, InstanceStatus> lastGood;  // by file name
		std::map<DWORD, HWND> windows;
		std::map<HWND, Thumb> thumbs;  // Grid mode only
		std::vector<Cell> cells;
		bool grid = false;
		RECT gridRect{};
		int captionHeight = 0;
		bool downInCell = false;
		HWND downWindow = nullptr;
	} app;

	int Scale(int px) { return MulDiv(px, static_cast<int>(app.dpi), 96); }

	// Tooltip text for a status part (uId 1, 2) or a window tool.
	void SetTip(HWND owner, UINT_PTR id, std::wstring text)
	{
		TTTOOLINFOW ti{};
		ti.cbSize = sizeof(ti);
		ti.hwnd = owner;
		ti.uId = id;
		ti.lpszText = text.data();
		SendMessageW(app.tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
	}

	// Part 0 is the LAN strip (hidden without a role), part 1 the resource line.
	void LayoutStatusParts(int width)
	{
		int lan = 0;
		if (!app.lanStatus.strip.empty())
		{
			const HDC dc = GetDC(app.status);
			const HGDIOBJ old = SelectObject(dc, app.font);
			SIZE size{};
			GetTextExtentPoint32W(dc, app.lanStatus.strip.c_str(), static_cast<int>(app.lanStatus.strip.size()), &size);
			SelectObject(dc, old);
			ReleaseDC(app.status, dc);
			lan = std::min<int>(size.cx + Scale(16), width / 2);
		}
		int parts[] = { lan, -1 };
		SendMessageW(app.status, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
		for (UINT_PTR id = 1; id <= 2; ++id)
		{
			TTTOOLINFOW ti{};
			ti.cbSize = sizeof(ti);
			ti.hwnd = app.status;
			ti.uId = id;
			SendMessageW(app.status, SB_GETRECT, id - 1, reinterpret_cast<LPARAM>(&ti.rect));
			SendMessageW(app.tooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
		}
	}

	void Layout();
	void UpdateLaunchButton();

	void UpdateLanUi(bool force = false)
	{
		const LanRole role = app.lan.Role();
		SendMessageW(app.toolbar, TB_CHECKBUTTON, kCmdHost, MAKELONG(role == LanRole::Host, 0));
		SendMessageW(app.toolbar, TB_ENABLEBUTTON, kCmdNewCode, MAKELONG(role == LanRole::Host, 0));
		SendMessageW(app.toolbar, TB_ENABLEBUTTON, kCmdResync, MAKELONG(app.lan.CanResync(), 0));
		UpdateLaunchButton();
		if (!force && app.lan.StatusVersion() == app.lanVersion)
		{
			return;
		}
		app.lanVersion = app.lan.StatusVersion();
		const LanStatusPart s = app.lan.Status();
		const bool relayout = force || s.banner.empty() != app.lanStatus.banner.empty() || s.strip.size() != app.lanStatus.strip.size();
		app.lanStatus = s;
		SetWindowTextW(app.main, (L"SC4 Region Sync Launcher" + (s.title.empty() ? L"" : L" (" + s.title + L")")).c_str());
		SendMessageW(app.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(app.lanStatus.strip.c_str()));
		SetWindowTextW(app.banner, app.lanStatus.banner.c_str());
		SetTip(app.status, 1, app.lanStatus.stripTip);
		SetTip(app.main, reinterpret_cast<UINT_PTR>(app.banner), app.lanStatus.bannerTip);
		if (relayout)
		{
			Layout();
		}
	}

	std::wstring LastComponent(const std::wstring& dir)
	{
		std::wstring d = dir;
		while (!d.empty() && d.back() == L'\\')
		{
			d.pop_back();
		}
		const size_t slash = d.find_last_of(L'\\');
		return slash == std::wstring::npos ? d : d.substr(slash + 1);
	}

	// A minimised window reports its restored client size.
	SIZE ClientSize(HWND hwnd)
	{
		RECT rc{};
		if (!IsIconic(hwnd))
		{
			GetClientRect(hwnd, &rc);
			return { rc.right - rc.left, rc.bottom - rc.top };
		}
		WINDOWPLACEMENT wp{};
		wp.length = sizeof(wp);
		GetWindowPlacement(hwnd, &wp);
		AdjustWindowRectEx(&rc, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE)), FALSE,
			static_cast<DWORD>(GetWindowLongW(hwnd, GWL_EXSTYLE)));
		const RECT& n = wp.rcNormalPosition;
		return { (n.right - n.left) - (rc.right - rc.left), (n.bottom - n.top) - (rc.bottom - rc.top) };
	}

	bool WindowMatches(HWND hwnd, DWORD pid)
	{
		DWORD owner = 0;
		GetWindowThreadProcessId(hwnd, &owner);
		if (owner != pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
		{
			return false;
		}
		if (app.settings.windowClass.empty())
		{
			const SIZE size = ClientSize(hwnd);
			return size.cx >= 640 && size.cy >= 480;  // never bind a splash or dialog
		}
		wchar_t cls[256]{};
		GetClassNameW(hwnd, cls, 256);
		if (_wcsicmp(app.settings.windowClass.c_str(), kDefaultWindowClass) == 0)
		{
			// The default also binds the other renderers' classes (suffix unverified).
			constexpr wchar_t prefix[] = L"GDriverClass--";
			return _wcsnicmp(cls, prefix, std::size(prefix) - 1) == 0;
		}
		return _wcsicmp(cls, app.settings.windowClass.c_str()) == 0;
	}

	struct EnumContext
	{
		const std::set<DWORD>* wanted;
		std::map<DWORD, HWND>* found;
	};

	BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lParam)
	{
		auto* ctx = reinterpret_cast<EnumContext*>(lParam);
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (ctx->wanted->count(pid) && !ctx->found->count(pid) && WindowMatches(hwnd, pid))
		{
			(*ctx->found)[pid] = hwnd;
		}
		return TRUE;
	}

	void UpdateWindows(const std::set<DWORD>& pids)
	{
		for (auto it = app.windows.begin(); it != app.windows.end();)
		{
			const bool keep = pids.count(it->first) && IsWindow(it->second) && WindowMatches(it->second, it->first);
			it = keep ? std::next(it) : app.windows.erase(it);
		}
		if (app.windows.size() < pids.size())
		{
			EnumContext ctx{ &pids, &app.windows };
			EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
		}
	}

	// Pids of running game processes, matched by GameExe's file name.
	std::set<DWORD> GameProcesses()
	{
		const std::wstring& exe = app.settings.gameExe;
		const size_t slash = exe.find_last_of(L"\\/");
		std::wstring name = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
		if (name.empty())
		{
			name = L"SimCity 4.exe";
		}
		std::set<DWORD> pids;
		const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE)
		{
			return pids;
		}
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry))
		{
			if (_wcsicmp(entry.szExeFile, name.c_str()) == 0)
			{
				pids.insert(entry.th32ProcessID);
			}
		}
		CloseHandle(snap);
		return pids;
	}

	uint64_t CreationTime(DWORD pid)
	{
		const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process)
		{
			return 0;
		}
		FILETIME created{}, exited{}, kernel{}, user{};
		const BOOL ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
		CloseHandle(process);
		return ok ? (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
	}

	// Unreadable creation time counts as old.
	bool OlderThan(DWORD pid, ULONGLONG ms)
	{
		const uint64_t created = CreationTime(pid);
		FILETIME now{};
		GetSystemTimeAsFileTime(&now);
		const uint64_t n = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
		return created == 0 || n < created || (n - created) / 10000 >= ms;
	}

	std::vector<InstanceStatus> ReadStatuses()
	{
		std::vector<InstanceStatus> live;
		std::set<std::wstring> seen;
		std::wstring dir = app.settings.statusDirectory;
		while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
		WIN32_FIND_DATAW fd{};
		const HANDLE find = FindFirstFileW((dir + L"\\*.status").c_str(), &fd);
		if (find != INVALID_HANDLE_VALUE)
		{
			do
			{
				const std::wstring name = fd.cFileName;
				if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name.size() < 7
					|| _wcsicmp(name.c_str() + name.size() - 7, L".status") != 0)
				{
					continue;
				}
				seen.insert(name);
				const std::wstring path = dir + L"\\" + name;
				InstanceStatus s;
				if (ReadStatusFile(path, s))
				{
					app.lastGood[name] = s;
				}
				else if (const auto it = app.lastGood.find(name); it != app.lastGood.end())
				{
					s = it->second;  // failed read keeps the previous row
				}
				else
				{
					continue;
				}
				if (IsAlive(s) == regionsync::writer::Liveness::Dead)
				{
					DeleteFileW(path.c_str());
					app.lastGood.erase(name);
					continue;
				}
				live.push_back(std::move(s));
			} while (FindNextFileW(find, &fd));
			FindClose(find);
		}
		for (auto it = app.lastGood.begin(); it != app.lastGood.end();)
		{
			it = seen.count(it->first) ? std::next(it) : app.lastGood.erase(it);
		}
		return live;
	}

	std::wstring TileText(const InstanceStatus& s, const std::vector<InstanceStatus>& all)
	{
		if (s.state == L"city")
		{
			if (s.conflict)
			{
				for (const InstanceStatus& o : all)
				{
					if (!(o.pid == s.pid && o.start == s.start && regionsync::SameHost(o.host, s.host)) && !o.conflict
						&& o.state == L"city" && o.regionX == s.regionX
						&& o.regionZ == s.regionZ && _wcsicmp(o.region.c_str(), s.region.c_str()) == 0)
					{
						return L"CONFLICT, held by pid " + std::to_wstring(o.pid) + (app.lan.IsOwnHost(o.host) ? L"" : L" on " + Widen(o.host));
					}
				}
				return L"CONFLICT";
			}
			return s.region + L" (" + std::to_wstring(s.regionX) + L"," + std::to_wstring(s.regionZ) + L") " + s.cityName;
		}
		return s.state == L"region" ? s.region : L"";
	}

	size_t CellCount() { return std::min(app.rows.size(), kMaxCells); }

	void UnregisterThumbs()
	{
		for (const auto& [hwnd, t] : app.thumbs)
		{
			if (t.handle)
			{
				DwmUnregisterThumbnail(t.handle);
			}
		}
		app.thumbs.clear();
	}

	void ReconcileThumbs()
	{
		std::set<HWND> wanted;
		for (size_t i = 0; i < CellCount(); ++i)
		{
			if (app.rows[i].window)
			{
				wanted.insert(app.rows[i].window);
			}
		}
		for (auto it = app.thumbs.begin(); it != app.thumbs.end();)
		{
			if (wanted.count(it->first))
			{
				++it;
				continue;
			}
			if (it->second.handle)
			{
				DwmUnregisterThumbnail(it->second.handle);  // erased whatever the HRESULT
			}
			it = app.thumbs.erase(it);
		}
		for (const HWND w : wanted)
		{
			if (app.thumbs.count(w))
			{
				continue;
			}
			Thumb t;
			if (FAILED(DwmRegisterThumbnail(app.main, w, &t.handle)))
			{
				t.handle = nullptr;
				t.failed = true;
			}
			app.thumbs[w] = t;
		}
	}

	RECT Letterbox(const RECT& area, SIZE source)
	{
		const int w = area.right - area.left, h = area.bottom - area.top;
		if (w <= 0 || h <= 0)
		{
			return area;
		}
		if (source.cx <= 0 || source.cy <= 0)
		{
			source = { 16, 9 };
		}
		int tw = w, th = MulDiv(w, source.cy, source.cx);
		if (th > h)
		{
			th = h;
			tw = MulDiv(h, source.cx, source.cy);
		}
		const int x = area.left + (w - tw) / 2, y = area.top + (h - th) / 2;
		return { x, y, x + tw, y + th };
	}

	void ApplyThumb(Thumb& t, const RECT& dest, bool visible)
	{
		if (!t.handle || (EqualRect(&t.dest, &dest) && t.visible == visible))
		{
			return;
		}
		DWM_THUMBNAIL_PROPERTIES p{};
		p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_OPACITY;
		p.rcDestination = dest;
		p.fVisible = visible;
		p.fSourceClientAreaOnly = TRUE;
		p.opacity = 255;
		if (SUCCEEDED(DwmUpdateThumbnailProperties(t.handle, &p)))
		{
			t.dest = dest;
			t.visible = visible;
		}
	}

	// Column count giving the largest letterboxed 16:9 thumbnail.
	int BestColumns(int n, int w, int h, int gutter)
	{
		int best = 1;
		long long bestArea = -1;
		for (int c = 1; c <= n; ++c)
		{
			const int r = (n + c - 1) / c;
			const int cw = (w - gutter * (c + 1)) / c;
			const int th = (h - gutter * (r + 1)) / r - app.captionHeight;
			const int tw = std::min(cw, th * 16 / 9);
			const long long area = tw > 0 ? static_cast<long long>(tw) * tw * 9 / 16 : 0;
			if (area > bestArea)
			{
				bestArea = area;
				best = c;
			}
		}
		return best;
	}

	// Rebuilds cells and thumbnail properties, then invalidates when forced or
	// when a caption, strip or the cell count changed.
	void LayoutGrid(bool force)
	{
		const RECT& g = app.gridRect;
		const int n = static_cast<int>(CellCount());
		const int gutter = Scale(8);
		const int w = g.right - g.left, h = g.bottom - g.top;
		const int cols = n ? BestColumns(n, w, h, gutter) : 1;
		const int lines = (n + cols - 1) / cols;
		const int cw = std::max(0, (w - gutter * (cols + 1)) / cols);
		const int ch = lines ? std::max(0, (h - gutter * (lines + 1)) / lines) : 0;
		const int stripH = std::min(app.captionHeight, ch);

		std::vector<Cell> cells;
		for (int i = 0; i < n; ++i)
		{
			const Row& r = app.rows[static_cast<size_t>(i)];
			Cell c;
			const int x = g.left + gutter + (i % cols) * (cw + gutter);
			const int y = g.top + gutter + (i / cols) * (ch + gutter);
			c.cell = { x, y, x + cw, y + ch };
			c.strip = { x, y, x + cw, y + stripH };
			c.area = { x, y + stripH, x + cw, y + ch };
			c.window = r.window;
			std::wstring detail = r.tile.empty() ? r.state : r.tile;
			const std::wstring prefix = r.conflict ? L"CONFLICT, " : L"";
			c.kind = r.conflict ? Strip::Conflict : Strip::Normal;
			const auto t = r.window ? app.thumbs.find(r.window) : app.thumbs.end();
			if (!r.window || t == app.thumbs.end() || t->second.failed)
			{
				c.kind = r.conflict ? Strip::Conflict : Strip::Grey;
				detail = prefix + (r.window ? L"no preview" : L"starting (no window)");
			}
			else
			{
				const bool iconic = IsIconic(r.window) != FALSE;
				ApplyThumb(t->second, Letterbox(c.area, ClientSize(r.window)), !iconic);
				if (iconic)
				{
					detail = prefix + L"minimised, click to restore";
				}
			}
			if (r.orphan)
			{
				c.kind = Strip::Grey;
				detail = r.tile;
			}
			if (r.remote)
			{
				c.kind = r.conflict ? Strip::Conflict : Strip::Grey;
				detail = L"on " + r.hostText + L"  " + (r.tile.empty() ? r.state : r.tile);
			}
			c.caption = r.label + L"  " + detail;
			if (i == n - 1 && app.rows.size() > kMaxCells)
			{
				c.caption += L"   +" + std::to_wstring(app.rows.size() - kMaxCells) + L" more, see List";
			}
			cells.push_back(std::move(c));
		}

		bool changed = force || cells.size() != app.cells.size();
		for (size_t i = 0; !changed && i < cells.size(); ++i)
		{
			changed = cells[i].caption != app.cells[i].caption || cells[i].kind != app.cells[i].kind;
		}
		app.cells = std::move(cells);
		if (changed)
		{
			InvalidateRect(app.main, &app.gridRect, FALSE);
		}
	}

	void PaintGrid()
	{
		PAINTSTRUCT ps{};
		const HDC dc = BeginPaint(app.main, &ps);
		const HGDIOBJ oldFont = SelectObject(dc, app.font);
		SetBkMode(dc, TRANSPARENT);
		for (const Cell& c : app.cells)
		{
			COLORREF fill = GetSysColor(COLOR_WINDOW), text = GetSysColor(COLOR_WINDOWTEXT);
			if (c.kind == Strip::Conflict)
			{
				fill = RGB(192, 0, 0);
				text = RGB(255, 255, 255);
			}
			else if (c.kind == Strip::Grey)
			{
				fill = RGB(128, 128, 128);
				text = RGB(255, 255, 255);
			}
			const HBRUSH brush = CreateSolidBrush(fill);
			FillRect(dc, &c.strip, brush);
			DeleteObject(brush);
			RECT tr = c.strip;
			tr.left += Scale(4);
			tr.right -= Scale(4);
			SetTextColor(dc, text);
			DrawTextW(dc, c.caption.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
			// Letterbox bands; the compositor draws the thumbnail over the rest.
			FillRect(dc, &c.area, GetSysColorBrush(COLOR_BTNSHADOW));  // letterbox bands
			ExcludeClipRect(dc, c.cell.left, c.cell.top, c.cell.right, c.cell.bottom);
		}
		FillRect(dc, &app.gridRect, GetSysColorBrush(COLOR_APPWORKSPACE));  // gutters
		SelectObject(dc, oldFont);
		EndPaint(app.main, &ps);
	}

	const Cell* CellAt(LPARAM lParam)
	{
		const POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		for (const Cell& c : app.cells)
		{
			if (PtInRect(&c.cell, pt))
			{
				return &c;
			}
		}
		return nullptr;
	}

	// The selected row when it is an orphan; never in Grid (no selection there).
	const Row* SelectedOrphan()
	{
		if (app.grid)
		{
			return nullptr;
		}
		const int i = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
		return i >= 0 && i < static_cast<int>(app.rows.size()) && app.rows[static_cast<size_t>(i)].orphan
			? &app.rows[static_cast<size_t>(i)] : nullptr;
	}

	void UpdateEndTaskButton()
	{
		SendMessageW(app.toolbar, TB_ENABLEBUTTON, kCmdEndTask, MAKELONG(SelectedOrphan() != nullptr, 0));
	}

	void SetView(bool grid)
	{
		SendMessageW(app.toolbar, TB_CHECKBUTTON, kCmdList, MAKELONG(!grid, 0));
		SendMessageW(app.toolbar, TB_CHECKBUTTON, kCmdGrid, MAKELONG(grid, 0));
		if (grid == app.grid)
		{
			return;
		}
		app.grid = grid;
		if (grid)
		{
			ShowWindow(app.list, SW_HIDE);
			SetFocus(app.main);
			ReconcileThumbs();
			LayoutGrid(true);
		}
		else
		{
			UnregisterThumbs();
			app.cells.clear();
			ShowWindow(app.list, SW_SHOW);
		}
		UpdateEndTaskButton();
	}

	void UpdateLaunchButton()
	{
		const bool allowed = !LaunchInFlight(app.pending) && app.lan.LaunchAllowed();
		SendMessageW(app.toolbar, TB_ENABLEBUTTON, kCmdLaunch, MAKELONG(allowed, 0));
	}

	int LocalRowCount()
	{
		return static_cast<int>(std::count_if(app.rows.begin(), app.rows.end(), [](const Row& r) { return !r.remote; }));
	}

	void Discover()
	{
		const std::vector<InstanceStatus> statuses = ReadStatuses();

		std::vector<std::wstring> messages;
		const ULONGLONG now = GetTickCount64();
		for (auto it = app.pending.begin(); it != app.pending.end();)
		{
			PendingLaunch& p = it->second;
			const bool reported = std::any_of(statuses.begin(), statuses.end(),
				[&p](const InstanceStatus& s) { return s.pid == p.pid && app.lan.IsOwnHost(s.host); });
			const bool exited = WaitForSingleObject(p.process, 0) == WAIT_OBJECT_0;
			if (!reported && !exited)
			{
				if (!p.timedOut && now - p.startedTick > kPendingTimeoutMs)
				{
					p.timedOut = true;
					messages.push_back(p.label + L" (pid " + std::to_wstring(p.pid)
						+ L") wrote no status file within 60 s. Launch is enabled again; the folder stays reserved until it exits.");
				}
				++it;
				continue;
			}
			if (exited && !reported && !p.timedOut)
			{
				messages.push_back(p.label + L" (pid " + std::to_wstring(p.pid) + L") exited before writing a status file.");
			}
			CloseHandle(p.process);
			it = app.pending.erase(it);
		}

		std::set<DWORD> pids;  // this PC's game processes only
		for (const InstanceStatus& s : statuses)
		{
			if (app.lan.IsOwnHost(s.host))
			{
				pids.insert(s.pid);
			}
		}
		for (const auto& [dir, p] : app.pending)
		{
			pids.insert(p.pid);
		}
		// A process is claimed only by a status file naming its pid and its creation time
		// (a remote status file can carry the same pid number).
		std::set<DWORD> orphans;
		std::set<std::pair<DWORD, uint64_t>> running;  // this PC's game processes by (pid, creation time)
		for (const DWORD pid : GameProcesses())
		{
			const uint64_t created = CreationTime(pid);
			running.emplace(pid, created);
			const bool claimed = std::any_of(statuses.begin(), statuses.end(),
				[pid, created](const InstanceStatus& s) { return s.pid == pid && s.start == created; })
				|| std::any_of(app.pending.begin(), app.pending.end(), [pid](const auto& p) { return p.second.pid == pid; });
			if (!claimed)
			{
				orphans.insert(pid);
			}
		}
		pids.insert(orphans.begin(), orphans.end());
		UpdateWindows(pids);

		std::vector<Row> rows;
		app.liveUserDirs.clear();
		for (const InstanceStatus& s : statuses)
		{
			Row r;
			r.pid = s.pid;
			r.start = s.start;
			r.host = s.host;
			r.remote = !app.lan.IsOwnHost(s.host);
			r.hostText = Widen(s.host);
			r.pidText = std::to_wstring(s.pid);
			r.label = s.userDir.empty() ? r.pidText : LastComponent(s.userDir);
			if (!r.remote)
			{
				const auto w = app.windows.find(s.pid);
				r.window = w == app.windows.end() ? nullptr : w->second;
			}
			r.state = r.window || r.remote ? s.state : L"starting (no window)";
			r.tile = TileText(s, statuses);
			r.conflict = s.state == L"city" && s.conflict;
			// A remote row whose writer runs here (loopback guest) still holds its folder.
			if ((!r.remote || running.count({ s.pid, s.start })) && !s.userDir.empty())
			{
				app.liveUserDirs.push_back(NormalizeDir(s.userDir));
			}
			rows.push_back(std::move(r));
		}
		for (const auto& [dir, p] : app.pending)
		{
			Row r;
			r.pid = p.pid;
			r.pidText = std::to_wstring(p.pid);
			r.label = p.label;
			r.state = L"starting (no status)";
			const auto w = app.windows.find(p.pid);
			r.window = w == app.windows.end() ? nullptr : w->second;
			rows.push_back(std::move(r));
		}
		for (const DWORD pid : orphans)
		{
			Row r;
			r.pid = pid;
			r.orphan = OlderThan(pid, kOrphanAgeMs);  // younger may still write its status file
			r.pidText = std::to_wstring(pid);
			r.label = L"pid " + r.pidText;
			r.state = r.orphan ? L"orphan" : L"starting";
			r.tile = r.orphan ? L"no plugin status (hung exit, or running without the plugin)" : L"no status yet";
			const auto w = app.windows.find(pid);
			r.window = w == app.windows.end() ? nullptr : w->second;
			rows.push_back(std::move(r));
		}
		std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b)
		{
			if (a.remote != b.remote) return b.remote;  // this PC's rows first
			return a.label.size() != b.label.size() ? a.label.size() < b.label.size() : a.label < b.label;
		});
		// Keep the selection on the same writer across the re-sort.
		const int oldSel = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
		const std::string selKey = oldSel >= 0 && oldSel < static_cast<int>(app.rows.size()) ? app.rows[static_cast<size_t>(oldSel)].Key() : "";
		app.rows = std::move(rows);
		int newSel = -1;
		for (size_t i = 0; !selKey.empty() && i < app.rows.size(); ++i)
		{
			if (app.rows[i].Key() == selKey)
			{
				newSel = static_cast<int>(i);
				break;
			}
		}

		ListView_SetItemCountEx(app.list, static_cast<int>(app.rows.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
		if (newSel != oldSel)
		{
			ListView_SetItemState(app.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
			if (newSel >= 0)
			{
				ListView_SetItemState(app.list, newSel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
			}
		}
		InvalidateRect(app.list, nullptr, FALSE);
		UpdateLaunchButton();
		UpdateEndTaskButton();
		if (app.grid)
		{
			ReconcileThumbs();
			LayoutGrid(false);
		}

		// After the thumbs map is consistent: MessageBoxW pumps the timer.
		for (const std::wstring& m : messages)
		{
			ShowMessage(app.main, m);
		}
	}

	void Sample()
	{
		std::vector<DWORD> pids;
		for (const Row& r : app.rows)
		{
			if (!r.remote)
			{
				pids.push_back(r.pid);
			}
		}
		app.resources = SampleResources(app.settings, pids);
		for (const Row& r : app.rows)
		{
			if (!r.window || !IsWindow(r.window)) continue;
			wchar_t cls[256] = L"";
			GetClassNameW(r.window, cls, 256);
			app.resources.tooltip += L"\r\nWindow class: ";
			app.resources.tooltip += cls;
			break;
		}
		const auto thumbs = std::count_if(app.thumbs.begin(), app.thumbs.end(), [](const auto& t) { return t.second.handle != nullptr; });
		app.resources.tooltip += L"\r\nthumbs=" + std::to_wstring(thumbs);
		const auto first = std::find_if(app.thumbs.begin(), app.thumbs.end(), [](const auto& t) { return t.second.handle != nullptr; });
		if (first != app.thumbs.end())
		{
			SIZE source{};
			DwmQueryThumbnailSourceSize(first->second.handle, &source);
			const SIZE client = ClientSize(first->first);
			app.resources.tooltip += L" source=" + std::to_wstring(source.cx) + L"x" + std::to_wstring(source.cy)
				+ L" client=" + std::to_wstring(client.cx) + L"x" + std::to_wstring(client.cy);
		}
		SendMessageW(app.status, SB_SETTEXTW, 1 | SBT_OWNERDRAW, 0);
		InvalidateRect(app.status, nullptr, TRUE);
		SetTip(app.status, 2, app.resources.tooltip);
	}

	// Orphans only: a row with a status file has a live plugin, and the in-game
	// Exit is its only safe way out.
	void EndTask()
	{
		const Row* row = SelectedOrphan();
		if (!row)
		{
			return;
		}
		const DWORD pid = row->pid;
		struct HandleGuard
		{
			HANDLE h;
			~HandleGuard() { if (h) CloseHandle(h); }
		};
		// Held across the prompt so the pid cannot be reused under us.
		const HandleGuard process{ OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid) };
		if (!process.h)
		{
			ShowMessage(app.main, L"OpenProcess on pid " + std::to_wstring(pid) + L" failed, error "
				+ std::to_wstring(GetLastError()) + L".", MB_ICONERROR);
			return;
		}
		std::wstring started = L"unknown";
		FILETIME created{}, exited{}, kernel{}, user{}, local{};
		SYSTEMTIME st{};
		if (GetProcessTimes(process.h, &created, &exited, &kernel, &user)
			&& FileTimeToLocalFileTime(&created, &local) && FileTimeToSystemTime(&local, &st))
		{
			wchar_t text[16];
			swprintf_s(text, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
			started = text;
		}
		const std::wstring prompt = L"End task on pid " + std::to_wstring(pid) + L" (started " + started
			+ L")? It has no plugin status, so any unsaved city is lost.";
		int answer = IDNO;
		{
			ModalScope modal;
			answer = MessageBoxW(app.main, prompt.c_str(), L"SC4 Region Sync Launcher", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
		}
		if (answer == IDYES)
		{
			// The box pumps the timer; re-check against a fresh read.
			const std::vector<InstanceStatus> statuses = ReadStatuses();
			const bool claimed = std::any_of(statuses.begin(), statuses.end(), [pid](const InstanceStatus& s) { return s.pid == pid; })
				|| std::any_of(app.pending.begin(), app.pending.end(), [pid](const auto& p) { return p.second.pid == pid; });
			if (claimed)
			{
				ShowMessage(app.main, L"pid " + std::to_wstring(pid) + L" now has a status file or pending launch; not ended.");
			}
			else if (WaitForSingleObject(process.h, 0) != WAIT_OBJECT_0)
			{
				if (TerminateProcess(process.h, 1))
				{
					WaitForSingleObject(process.h, 2000);  // so the rediscover below drops the row
				}
				else
				{
					ShowMessage(app.main, L"TerminateProcess on pid " + std::to_wstring(pid) + L" failed, error "
						+ std::to_wstring(GetLastError()) + L".", MB_ICONERROR);
				}
			}
		}
		Discover();
	}

	void RaiseWindow(HWND hwnd)
	{
		if (!hwnd || !IsWindow(hwnd))
		{
			return;
		}
		if (IsIconic(hwnd))
		{
			ShowWindowAsync(hwnd, SW_RESTORE);
		}
		SetForegroundWindow(hwnd);
	}

	void Raise(int index)
	{
		if (index >= 0 && index < static_cast<int>(app.rows.size()) && !app.rows[static_cast<size_t>(index)].remote)
		{
			RaiseWindow(app.rows[static_cast<size_t>(index)].window);
		}
	}

	void ApplyDpi()
	{
		NONCLIENTMETRICSW ncm{};
		ncm.cbSize = sizeof(ncm);
		SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, app.dpi);
		const HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
		for (const HWND h : { app.list, app.toolbar, app.status, app.banner })
		{
			SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		}
		if (app.font)
		{
			DeleteObject(app.font);
		}
		app.font = font;
		const HDC dc = GetDC(app.main);
		const HGDIOBJ old = SelectObject(dc, font);
		TEXTMETRICW tm{};
		GetTextMetricsW(dc, &tm);
		SelectObject(dc, old);
		ReleaseDC(app.main, dc);
		app.captionHeight = tm.tmHeight + Scale(6);
		const int widths[] = { 110, 100, 150, 260, 70 };
		for (int i = 0; i < kColumns; ++i)
		{
			ListView_SetColumnWidth(app.list, i, Scale(widths[i]));
		}
		SendMessageW(app.toolbar, TB_AUTOSIZE, 0, 0);
		SendMessageW(app.tooltip, TTM_SETMAXTIPWIDTH, 0, Scale(500));
	}

	void Layout()
	{
		RECT client{};
		GetClientRect(app.main, &client);
		SendMessageW(app.toolbar, TB_AUTOSIZE, 0, 0);
		SendMessageW(app.status, WM_SIZE, 0, 0);
		RECT tb{}, sb{};
		GetWindowRect(app.toolbar, &tb);
		GetWindowRect(app.status, &sb);
		int top = tb.bottom - tb.top;
		const int bottom = client.bottom - (sb.bottom - sb.top);
		const bool banner = !app.lanStatus.banner.empty();
		ShowWindow(app.banner, banner ? SW_SHOW : SW_HIDE);
		if (banner)
		{
			MoveWindow(app.banner, Scale(4), top + Scale(2), client.right - Scale(8), app.captionHeight, TRUE);
			top += app.captionHeight + Scale(4);
		}
		LayoutStatusParts(client.right);
		MoveWindow(app.list, 0, top, client.right, std::max(0, bottom - top), TRUE);
		app.gridRect = { 0, top, client.right, std::max(top, bottom) };
		if (app.grid)
		{
			LayoutGrid(true);
		}
	}

	void CreateControls(HWND hwnd, HINSTANCE inst)
	{
		app.main = hwnd;
		app.dpi = GetDpiForWindow(hwnd);

		app.toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
			WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | CCS_TOP,
			0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
		SendMessageW(app.toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
		SendMessageW(app.toolbar, TB_SETBITMAPSIZE, 0, MAKELONG(0, 0));
		constexpr int kButtons = 14;
		TBBUTTON buttons[kButtons]{};
		const wchar_t* labels[] = { L"Launch", L"Refresh", nullptr, L"List", L"Grid", nullptr, L"Settings", nullptr, L"End Task",
			nullptr, L"Host", L"Join", L"Re-sync", L"New code" };
		const int ids[] = { kCmdLaunch, kCmdRefresh, 0, kCmdList, kCmdGrid, 0, kCmdSettings, 0, kCmdEndTask,
			0, kCmdHost, kCmdJoin, kCmdResync, kCmdNewCode };
		for (int i = 0; i < kButtons; ++i)
		{
			if (!labels[i])
			{
				buttons[i].fsStyle = BTNS_SEP;
				continue;
			}
			buttons[i].iBitmap = I_IMAGENONE;
			buttons[i].idCommand = ids[i];
			buttons[i].fsState = TBSTATE_ENABLED;
			buttons[i].fsStyle = (i == 3 || i == 4 ? BTNS_CHECKGROUP : ids[i] == kCmdHost ? BTNS_CHECK : BTNS_BUTTON) | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
			buttons[i].iString = reinterpret_cast<INT_PTR>(labels[i]);
		}
		buttons[3].fsState |= TBSTATE_CHECKED;  // always starts in List
		buttons[8].fsState = 0;  // End Task: enabled for a selected orphan
		buttons[12].fsState = 0;  // Re-sync: guest, connected
		buttons[13].fsState = 0;  // New code: hosting
		SendMessageW(app.toolbar, TB_ADDBUTTONSW, kButtons, reinterpret_cast<LPARAM>(buttons));

		app.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
			WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
			0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
		ListView_SetExtendedListViewStyle(app.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
		const wchar_t* columns[] = { L"Instance", L"Host", L"State", L"Tile", L"PID" };
		for (int i = 0; i < kColumns; ++i)
		{
			LVCOLUMNW col{};
			col.mask = LVCF_TEXT | LVCF_SUBITEM;
			col.pszText = const_cast<wchar_t*>(columns[i]);
			col.iSubItem = i;
			ListView_InsertColumn(app.list, i, &col);
		}

		app.status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE,
			0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
		int parts[] = { 0, -1 };
		SendMessageW(app.status, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
		app.banner = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | SS_NOTIFY | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_CENTERIMAGE,
			0, 0, 0, 0, hwnd, nullptr, inst, nullptr);

		app.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
			WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			hwnd, nullptr, inst, nullptr);
		// Status parts 1 (LAN) and 2 (resources) by rectangle; the banner by window.
		for (UINT_PTR id = 1; id <= 2; ++id)
		{
			TTTOOLINFOW ti{};
			ti.cbSize = sizeof(ti);
			ti.uFlags = TTF_SUBCLASS;
			ti.hwnd = app.status;
			ti.uId = id;
			ti.lpszText = const_cast<wchar_t*>(L"");
			SendMessageW(app.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
		}
		TTTOOLINFOW bi{};
		bi.cbSize = sizeof(bi);
		bi.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
		bi.hwnd = hwnd;
		bi.uId = reinterpret_cast<UINT_PTR>(app.banner);
		bi.lpszText = const_cast<wchar_t*>(L"");
		SendMessageW(app.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&bi));
		SendMessageW(app.tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);

		ApplyDpi();
	}

	void DrawStatus(const DRAWITEMSTRUCT& dis)
	{
		COLORREF colour = RGB(198, 239, 206);
		if (app.resources.colour == ResourceSample::Colour::Amber)
		{
			colour = RGB(255, 229, 153);
		}
		else if (app.resources.colour == ResourceSample::Colour::Red)
		{
			colour = RGB(255, 180, 180);
		}
		const HBRUSH brush = CreateSolidBrush(colour);
		FillRect(dis.hDC, &dis.rcItem, brush);
		DeleteObject(brush);
		RECT text = dis.rcItem;
		text.left += Scale(4);
		SetBkMode(dis.hDC, TRANSPARENT);
		SetTextColor(dis.hDC, RGB(0, 0, 0));
		const HGDIOBJ old = SelectObject(dis.hDC, app.font);
		DrawTextW(dis.hDC, app.resources.text.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
		SelectObject(dis.hDC, old);
	}

	LRESULT OnNotify(const NMHDR& hdr)
	{
		if (hdr.hwndFrom != app.list)
		{
			return 0;
		}
		if (hdr.code == LVN_GETDISPINFOW)
		{
			LVITEMW& item = reinterpret_cast<NMLVDISPINFOW*>(const_cast<NMHDR*>(&hdr))->item;
			if ((item.mask & LVIF_TEXT) && item.iItem >= 0 && item.iItem < static_cast<int>(app.rows.size()))
			{
				const Row& r = app.rows[static_cast<size_t>(item.iItem)];
				const std::wstring* cells[] = { &r.label, &r.hostText, &r.state, &r.tile, &r.pidText };
				const std::wstring& text = item.iSubItem >= 0 && item.iSubItem < kColumns ? *cells[item.iSubItem] : r.label;
				wcsncpy_s(item.pszText, static_cast<size_t>(item.cchTextMax), text.c_str(), _TRUNCATE);
			}
		}
		else if (hdr.code == LVN_ITEMCHANGED)
		{
			UpdateEndTaskButton();
		}
		else if (hdr.code == NM_CLICK || hdr.code == NM_DBLCLK || hdr.code == LVN_ITEMACTIVATE)
		{
			Raise(reinterpret_cast<const NMITEMACTIVATE*>(&hdr)->iItem);
		}
		return 0;
	}

	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_CREATE:
			CreateControls(hwnd, reinterpret_cast<CREATESTRUCTW*>(lParam)->hInstance);
			return 0;
		case WM_SIZE:
			Layout();
			return 0;
		case WM_DPICHANGED:
		{
			app.dpi = HIWORD(wParam);
			ApplyDpi();
			const RECT* r = reinterpret_cast<const RECT*>(lParam);
			SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
			Layout();
			return 0;
		}
		case WM_TIMER:
		case WM_COMMAND:
		case WM_APP_LAN:
			try  // keep the window alive; LAN errors go to lan.log
			{
				if (msg == WM_APP_LAN)
				{
					app.lan.OnWorkerMessage();
					UpdateLanUi();
				}
				else if (msg == WM_TIMER)
				{
					if (wParam == kDiscoveryTimer)
					{
						Discover();
					}
					else if (wParam == kResourceTimer)
					{
						Sample();
					}
					else if (wParam == kLanTimer)
					{
						app.lan.OnTimer();
						UpdateLanUi();
					}
				}
				else if (LOWORD(wParam) == kCmdLaunch)
				{
					if (!app.lan.LaunchAllowed())
					{
						ShowMessage(hwnd, L"Launch waits until the sync with the host is done.");
					}
					else if (Launch(hwnd, app.settings, app.resources, LocalRowCount(), app.liveUserDirs, app.pending))
					{
						Discover();
					}
				}
				else if (LOWORD(wParam) == kCmdRefresh
					|| (LOWORD(wParam) == kCmdSettings && EditSettings(hwnd, app.settings)))
				{
					app.settings = LoadSettings();
					app.lan.SettingsChanged(app.settings);
					Discover();
					Sample();
				}
				else if (LOWORD(wParam) == kCmdHost)
				{
					app.lan.ToggleHost();
					UpdateLanUi(true);
				}
				else if (LOWORD(wParam) == kCmdJoin)
				{
					app.lan.ShowJoin();
					UpdateLanUi(true);
				}
				else if (LOWORD(wParam) == kCmdResync)
				{
					app.lan.Resync();
					UpdateLanUi(true);
				}
				else if (LOWORD(wParam) == kCmdNewCode)
				{
					app.lan.NewCode();
					UpdateLanUi(true);
				}
				else if (LOWORD(wParam) == kCmdEndTask)
				{
					EndTask();
				}
				else if (LOWORD(wParam) == kCmdList || LOWORD(wParam) == kCmdGrid)
				{
					SetView(LOWORD(wParam) == kCmdGrid);
				}
			}
			catch (...)
			{
			}
			return 0;
		case WM_ERASEBKGND:
		{
			// The flat toolbar relies on the parent's erase; paint it in both modes.
			// In grid mode the cells are painted by WM_PAINT, so skip that rect.
			HDC dc = reinterpret_cast<HDC>(wParam);
			RECT rc{};
			GetClientRect(hwnd, &rc);
			if (app.grid)
			{
				ExcludeClipRect(dc, app.gridRect.left, app.gridRect.top, app.gridRect.right, app.gridRect.bottom);
			}
			FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
			return 1;
		}
		case WM_PAINT:
			if (app.grid)
			{
				PaintGrid();
				return 0;
			}
			break;
		case WM_LBUTTONDOWN:
		{
			const Cell* c = app.grid ? CellAt(lParam) : nullptr;
			app.downInCell = c != nullptr;
			app.downWindow = c ? c->window : nullptr;
			if (c)
			{
				SetCapture(hwnd);
			}
			return 0;
		}
		case WM_CAPTURECHANGED:
			app.downInCell = false;
			return 0;
		case WM_LBUTTONUP:
		{
			const Cell* c = app.grid && app.downInCell ? CellAt(lParam) : nullptr;
			app.downInCell = false;
			if (GetCapture() == hwnd)
			{
				ReleaseCapture();
			}
			if (c && c->window == app.downWindow)
			{
				RaiseWindow(c->window);
			}
			return 0;
		}
		case WM_NOTIFY:
			return OnNotify(*reinterpret_cast<const NMHDR*>(lParam));
		case WM_DRAWITEM:
		{
			const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
			if (dis->hwndItem == app.status && dis->itemID == 1)
			{
				DrawStatus(*dis);
				return TRUE;
			}
			break;
		}
		case WM_CLOSE:
			if (InModal())
			{
				DeferClose();  // reposted when the last dialog closes
				return 0;
			}
			// Stop flag, signal, join 3 s; a stuck worker must not keep the process alive.
			if (!app.lan.Shutdown())
			{
				ExitProcess(0);  // launcher.ini is written as values change; nothing is pending
			}
			DestroyWindow(hwnd);
			return 0;
		case WM_DESTROY:
			for (const auto& [dir, p] : app.pending)
			{
				CloseHandle(p.process);
			}
			app.pending.clear();
			UnregisterThumbs();
			if (app.font)
			{
				DeleteObject(app.font);
			}
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
	INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_WIN95_CLASSES };
	InitCommonControlsEx(&icc);

	app.settings = LoadSettings();

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	wc.lpszClassName = L"SC4RegionSyncLauncher";
	RegisterClassExW(&wc);

	const HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SC4 Region Sync Launcher", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, inst, nullptr);
	if (!hwnd)
	{
		return 1;
	}
	SetWindowPos(hwnd, nullptr, 0, 0, Scale(760), Scale(400), SWP_NOMOVE | SWP_NOZORDER);
	ShowWindow(hwnd, show);

	// Before the first Discover: HostAlias decides which status files are ours.
	app.lan.Init(hwnd, app.settings);
	UpdateLanUi(true);
	Discover();
	Sample();
	SetTimer(hwnd, kDiscoveryTimer, 1000, nullptr);
	SetTimer(hwnd, kResourceTimer, 5000, nullptr);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return static_cast<int>(msg.wParam);
}
