/*
 * sc4-region-sync launcher
 *
 * LanUi.cpp - the Join dialog (docs/PLAN-LAN.md section 6) and the Yes/No prompt.
 * The strip and banner live in main.cpp; their text comes from LanController::Status.
 */

#include "Lan.h"
#include "resource.h"
#include <CommCtrl.h>

namespace
{
	using namespace launcher;

	struct FoundHost
	{
		std::string site, siteId, build, address;
	};

	struct JoinState
	{
		HWND dlg = nullptr;
		JoinHooks* hooks = nullptr;
		JoinResult* out = nullptr;
		std::vector<FoundHost> found;
		bool connected = false;
	};

	JoinState* active = nullptr;
	constexpr size_t kMaxFound = 32;

	std::string Text(HWND dlg, int id)
	{
		wchar_t buf[256]{};
		GetDlgItemTextW(dlg, id, buf, 256);
		return Utf8(buf);
	}

	void Message(const std::string& text)
	{
		SetDlgItemTextW(active->dlg, IDC_JOIN_MESSAGE, Widen(text).c_str());
	}

	void FillHosts()
	{
		const HWND list = GetDlgItem(active->dlg, IDC_JOIN_HOSTS);
		ListView_DeleteAllItems(list);
		for (size_t i = 0; i < active->found.size(); i++)
		{
			const FoundHost& h = active->found[i];
			const std::wstring cells[] = { Widen(h.site), Widen(h.address), Widen(h.siteId.substr(0, 8)),
				h.siteId == active->hooks->pairedSiteId ? L"paired" : L"" };
			LVITEMW item{};
			item.mask = LVIF_TEXT;
			item.iItem = static_cast<int>(i);
			item.pszText = const_cast<wchar_t*>(cells[0].c_str());
			ListView_InsertItem(list, &item);
			for (int c = 1; c < 4; c++) ListView_SetItemText(list, static_cast<int>(i), c, const_cast<wchar_t*>(cells[c].c_str()));
		}
	}

	void FillRegions(const std::vector<std::string>& regions)
	{
		const HWND combo = GetDlgItem(active->dlg, IDC_JOIN_REGION);
		SendMessageW(combo, CB_RESETCONTENT, 0, 0);
		LRESULT pick = 0;
		for (const std::string& r : regions)
		{
			const LRESULT i = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Widen(r).c_str()));
			if (lan::EqualsNoCase(r, active->hooks->lastRegion)) pick = i;
		}
		SendMessageW(combo, CB_SETCURSEL, regions.empty() ? static_cast<WPARAM>(-1) : static_cast<WPARAM>(pick), 0);
		EnableWindow(GetDlgItem(active->dlg, IDOK), !regions.empty());
	}

	void OnSelectHost()
	{
		const int i = ListView_GetNextItem(GetDlgItem(active->dlg, IDC_JOIN_HOSTS), -1, LVNI_SELECTED);
		if (i < 0 || i >= static_cast<int>(active->found.size())) return;
		const FoundHost& h = active->found[static_cast<size_t>(i)];
		SetDlgItemTextW(active->dlg, IDC_JOIN_ADDRESS, Widen(h.address).c_str());
		const bool paired = h.siteId == active->hooks->pairedSiteId;
		EnableWindow(GetDlgItem(active->dlg, IDC_JOIN_CODE), !paired);
		if (paired) SetDlgItemTextW(active->dlg, IDC_JOIN_CODE, L"");
	}

	void OnConnect()
	{
		const std::string address = Text(active->dlg, IDC_JOIN_ADDRESS);
		const std::string code = IsWindowEnabled(GetDlgItem(active->dlg, IDC_JOIN_CODE)) ? Text(active->dlg, IDC_JOIN_CODE) : std::string();
		if (address.empty())
		{
			Message("Pick a host or type its address.");
			return;
		}
		if (!code.empty() && !lan::IsValidPairCode(code))
		{
			Message("The code is the 6 digits shown on the host.");
			return;
		}
		if (code.empty() && active->hooks->pairedSiteId.empty())
		{
			Message("This PC is not paired yet: type the 6-digit code shown on the host.");
			return;
		}
		SetDlgItemTextW(active->dlg, IDC_JOIN_CODE, L"");   // one attempt per code
		active->connected = false;
		FillRegions({});
		Message("Connecting to " + address + "...");
		active->hooks->connect(address, code);
	}

	INT_PTR CALLBACK JoinProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		try  // no exception may cross the dialog manager
		{
			switch (msg)
			{
			case WM_INITDIALOG:
			{
				active = reinterpret_cast<JoinState*>(lParam);
				active->dlg = dlg;
				const HWND list = GetDlgItem(dlg, IDC_JOIN_HOSTS);
				ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);
				const wchar_t* columns[] = { L"Name", L"Address", L"ID", L"Paired" };
				const int widths[] = { 110, 100, 70, 55 };
				for (int i = 0; i < 4; i++)
				{
					LVCOLUMNW col{};
					col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
					col.pszText = const_cast<wchar_t*>(columns[i]);
					col.cx = MulDiv(widths[i], static_cast<int>(GetDpiForWindow(dlg)), 96);
					col.iSubItem = i;
					ListView_InsertColumn(list, i, &col);
				}
				SendDlgItemMessageW(dlg, IDC_JOIN_CODE, EM_LIMITTEXT, 6, 0);
				SendDlgItemMessageW(dlg, IDC_JOIN_ADDRESS, EM_LIMITTEXT, 253, 0);
				EnableWindow(GetDlgItem(dlg, IDC_JOIN_LEAVE), active->hooks->joined);
				active->connected = !active->hooks->regions.empty();
				FillRegions(active->hooks->regions);
				active->hooks->discover(true);
				return TRUE;
			}
			case WM_NOTIFY:
			{
				const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
				if (hdr->idFrom == IDC_JOIN_HOSTS && hdr->code == LVN_ITEMCHANGED) OnSelectHost();
				return FALSE;
			}
			case WM_COMMAND:
				switch (LOWORD(wParam))
				{
				case IDC_JOIN_ADDRESS:
					if (HIWORD(wParam) == EN_KILLFOCUS)
					{
						const std::string a = Text(dlg, IDC_JOIN_ADDRESS);
						if (!a.empty()) active->hooks->probe(a);
						EnableWindow(GetDlgItem(dlg, IDC_JOIN_CODE), TRUE);
					}
					return TRUE;
				case IDC_JOIN_CONNECT:
					OnConnect();
					return TRUE;
				case IDOK:
				{
					wchar_t region[256]{};
					GetDlgItemTextW(dlg, IDC_JOIN_REGION, region, 256);
					const std::string r = Utf8(region);
					if (!active->connected || !IsSyncableRegion(r))
					{
						Message("Connect first, then pick a region.");
						return TRUE;
					}
					active->out->region = r;
					EndDialog(dlg, 1);
					return TRUE;
				}
				case IDC_JOIN_LEAVE:
					if (ConfirmYesNo(dlg, L"Leave the host? This PC stops syncing until you Join again. The pairing is kept."))
					{
						EndDialog(dlg, 2);
					}
					return TRUE;
				case IDCANCEL:
					EndDialog(dlg, 0);
					return TRUE;
				}
				return FALSE;
			}
		}
		catch (...)
		{
		}
		return FALSE;
	}
}

namespace launcher
{
	int RunJoinDialog(HWND owner, JoinHooks& hooks, JoinResult& out)
	{
		JoinState state;
		state.hooks = &hooks;
		state.out = &out;
		const INT_PTR r = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_JOIN), owner, JoinProc,
			reinterpret_cast<LPARAM>(&state));
		active = nullptr;
		return r == 1 || r == 2 ? static_cast<int>(r) : 0;
	}

	bool JoinDialogEvent(const LanEvent& e)
	{
		if (!active || !active->dlg) return false;
		switch (e.kind)
		{
		case LanEvent::Kind::Found:
		{
			// Keyed by ID and address: a spoofed reply adds a row, never moves a known one. Capped.
			auto it = std::find_if(active->found.begin(), active->found.end(),
				[&](const FoundHost& h) { return h.siteId == e.hello.siteId && h.address == e.address; });
			const FoundHost h{ e.hello.site, e.hello.siteId, e.hello.build, e.address };
			if (it == active->found.end())
			{
				if (active->found.size() >= kMaxFound) break;
				active->found.push_back(h);
			}
			else if (it->site == h.site && it->build == h.build) break;
			else *it = h;
			FillHosts();
			break;
		}
		case LanEvent::Kind::Status:
			Message(e.text);
			break;
		case LanEvent::Kind::Refused:
		case LanEvent::Kind::Closed:
			active->connected = false;
			FillRegions({});
			Message(e.text);
			break;
		case LanEvent::Kind::Session:
			active->connected = true;
			if (e.flag) active->hooks->pairedSiteId = e.hello.siteId;
			FillRegions(e.hello.regions);
			FillHosts();
			Message("Connected to " + e.hello.site + (e.flag ? " and paired" : "") + ". Pick the region to play, then OK.");
			break;
		default:
			break;
		}
		return true;
	}

	bool ConfirmYesNo(HWND owner, const std::wstring& text)
	{
		ModalScope modal;
		return MessageBoxW(owner, text.c_str(), L"SC4 Region Sync Launcher", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
	}
}
