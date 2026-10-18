/*
 * sc4-region-sync launcher
 *
 * main.cpp - the window: instance list or grid of DWM thumbnails, Launch and
 * Refresh, resource line. Discovery every 1 s, resource sampling every 5 s
 * (docs/PLAN-LAUNCHER.md).
 */

#include "Launcher.h"
#include <CommCtrl.h>
#include <dwmapi.h>
#include <windowsx.h>
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
	constexpr size_t kMaxCells = 16;
	constexpr ULONGLONG kPendingTimeoutMs = 60000;

	struct Row
	{
		DWORD pid = 0;
		HWND window = nullptr;
		bool conflict = false;
		std::wstring label, state, tile, pidText;
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
		HWND main = nullptr, list = nullptr, toolbar = nullptr, status = nullptr, tooltip = nullptr;
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

	std::vector<InstanceStatus> ReadStatuses()
	{
		std::vector<InstanceStatus> live;
		std::set<std::wstring> seen;
		const std::wstring dir = app.settings.statusDirectory;
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
					if (o.pid != s.pid && !o.conflict && o.state == L"city" && o.regionX == s.regionX
						&& o.regionZ == s.regionZ && _wcsicmp(o.region.c_str(), s.region.c_str()) == 0)
					{
						return L"CONFLICT, held by pid " + std::to_wstring(o.pid);
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
	}

	void UpdateLaunchButton()
	{
		SendMessageW(app.toolbar, TB_ENABLEBUTTON, kCmdLaunch, MAKELONG(!LaunchInFlight(app.pending), 0));
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
				[&p](const InstanceStatus& s) { return s.pid == p.pid; });
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

		std::set<DWORD> pids;
		for (const InstanceStatus& s : statuses)
		{
			pids.insert(s.pid);
		}
		for (const auto& [dir, p] : app.pending)
		{
			pids.insert(p.pid);
		}
		UpdateWindows(pids);

		std::vector<Row> rows;
		app.liveUserDirs.clear();
		for (const InstanceStatus& s : statuses)
		{
			Row r;
			r.pid = s.pid;
			r.pidText = std::to_wstring(s.pid);
			r.label = s.userDir.empty() ? r.pidText : LastComponent(s.userDir);
			const auto w = app.windows.find(s.pid);
			r.window = w == app.windows.end() ? nullptr : w->second;
			r.state = r.window ? s.state : L"starting (no window)";
			r.tile = TileText(s, statuses);
			r.conflict = s.state == L"city" && s.conflict;
			rows.push_back(std::move(r));
			if (!s.userDir.empty())
			{
				app.liveUserDirs.push_back(NormalizeDir(s.userDir));
			}
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
		std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b)
		{
			return a.label.size() != b.label.size() ? a.label.size() < b.label.size() : a.label < b.label;
		});
		app.rows = std::move(rows);

		ListView_SetItemCountEx(app.list, static_cast<int>(app.rows.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
		InvalidateRect(app.list, nullptr, FALSE);
		UpdateLaunchButton();
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
			pids.push_back(r.pid);
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
		SendMessageW(app.status, SB_SETTEXTW, SBT_OWNERDRAW, 0);
		InvalidateRect(app.status, nullptr, TRUE);

		TTTOOLINFOW ti{};
		ti.cbSize = sizeof(ti);
		ti.hwnd = app.main;
		ti.uId = reinterpret_cast<UINT_PTR>(app.status);
		ti.lpszText = app.resources.tooltip.data();
		SendMessageW(app.tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
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
		if (index >= 0 && index < static_cast<int>(app.rows.size()))
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
		for (const HWND h : { app.list, app.toolbar, app.status })
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
		const int widths[] = { 110, 150, 260, 70 };
		for (int i = 0; i < 4; ++i)
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
		const int top = tb.bottom - tb.top;
		const int bottom = client.bottom - (sb.bottom - sb.top);
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
		TBBUTTON buttons[5]{};
		const wchar_t* labels[] = { L"Launch", L"Refresh", nullptr, L"List", L"Grid" };
		const int ids[] = { kCmdLaunch, kCmdRefresh, 0, kCmdList, kCmdGrid };
		for (int i = 0; i < 5; ++i)
		{
			if (!labels[i])
			{
				buttons[i].fsStyle = BTNS_SEP;
				continue;
			}
			buttons[i].iBitmap = I_IMAGENONE;
			buttons[i].idCommand = ids[i];
			buttons[i].fsState = TBSTATE_ENABLED;
			buttons[i].fsStyle = (i >= 3 ? BTNS_CHECKGROUP : BTNS_BUTTON) | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
			buttons[i].iString = reinterpret_cast<INT_PTR>(labels[i]);
		}
		buttons[3].fsState |= TBSTATE_CHECKED;  // always starts in List
		SendMessageW(app.toolbar, TB_ADDBUTTONSW, 5, reinterpret_cast<LPARAM>(buttons));

		app.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
			WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
			0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
		ListView_SetExtendedListViewStyle(app.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
		const wchar_t* columns[] = { L"Instance", L"State", L"Tile", L"PID" };
		for (int i = 0; i < 4; ++i)
		{
			LVCOLUMNW col{};
			col.mask = LVCF_TEXT | LVCF_SUBITEM;
			col.pszText = const_cast<wchar_t*>(columns[i]);
			col.iSubItem = i;
			ListView_InsertColumn(app.list, i, &col);
		}

		app.status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE,
			0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
		int parts[] = { -1 };
		SendMessageW(app.status, SB_SETPARTS, 1, reinterpret_cast<LPARAM>(parts));

		app.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
			WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			hwnd, nullptr, inst, nullptr);
		TTTOOLINFOW ti{};
		ti.cbSize = sizeof(ti);
		ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
		ti.hwnd = hwnd;
		ti.uId = reinterpret_cast<UINT_PTR>(app.status);
		ti.lpszText = const_cast<wchar_t*>(L"");
		SendMessageW(app.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
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
				const std::wstring* cells[] = { &r.label, &r.state, &r.tile, &r.pidText };
				const std::wstring& text = item.iSubItem >= 0 && item.iSubItem < 4 ? *cells[item.iSubItem] : r.label;
				wcsncpy_s(item.pszText, static_cast<size_t>(item.cchTextMax), text.c_str(), _TRUNCATE);
			}
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
			try  // keep the window alive; there is no log to report to
			{
				if (msg == WM_TIMER)
				{
					if (wParam == kDiscoveryTimer)
					{
						Discover();
					}
					else if (wParam == kResourceTimer)
					{
						Sample();
					}
				}
				else if (LOWORD(wParam) == kCmdLaunch)
				{
					if (Launch(hwnd, app.settings, app.resources, static_cast<int>(app.rows.size()), app.liveUserDirs, app.pending))
					{
						Discover();
					}
				}
				else if (LOWORD(wParam) == kCmdRefresh)
				{
					app.settings = LoadSettings();
					Discover();
					Sample();
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
			if (dis->hwndItem == app.status)
			{
				DrawStatus(*dis);
				return TRUE;
			}
			break;
		}
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
	SetWindowPos(hwnd, nullptr, 0, 0, Scale(640), Scale(360), SWP_NOMOVE | SWP_NOZORDER);
	ShowWindow(hwnd, show);

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
