/*
 * sc4-region-sync launcher
 *
 * Lan.cpp - the UI-thread side of two-PC sync (docs/PLAN-LAN.md sections 2 to 6):
 * launcher.ini [Lan], pairing codes, version checks, the guest's diff and swap,
 * the journal, mirroring of snapshots and status files, reaping and lan.log.
 */

#include "Lan.h"
#include "InstanceStatus.h"
#include "monocypher.h"
#include "version.h"
#include <objbase.h>
#include <shlobj.h>
#include <netlistmgr.h>
#include <tlhelp32.h>
#include <algorithm>
#include <set>

#ifndef BUILD_ID
#define BUILD_ID "0.0.0+0000000"
#endif

namespace
{
	using namespace launcher;
	using lan::Tile;
	using regionsync::SameHost;

	constexpr wchar_t kSection[] = L"Lan";
	constexpr ULONGLONG kCodeLifetimeMs = 10 * 60 * 1000;
	constexpr ULONGLONG kPeerKeepMs = 10 * 60 * 1000;
	constexpr ULONGLONG kRescanMs = 5000;
	constexpr size_t kMaxSnapshot = 256 * 1024;
	constexpr size_t kMaxStatus = 16 * 1024;
	constexpr size_t kMaxPeerSnapshots = 1024;   // per peer, per region

	enum class Sync { Idle, Hashing, Downloading, SwapPending, Synced, Failed };

	struct PendingWrite
	{
		std::string kind, name;
		std::vector<uint8_t> bytes;
	};

	struct State
	{
		HWND main = nullptr;
		Settings settings;
		LanWorker worker;
		bool workerOk = false;
		uint64_t version = 1;

		// [Lan]
		std::wstring ini;
		std::wstring regionsSetting, pluginsDir;
		int port = kDefaultLanPort;
		std::string siteId;
		LanRole role = LanRole::None;
		std::string peerSite, peerSiteId, region, hostAlias;
		uint32_t peerAddress = 0;
		bool hasKey = false;
		lan::Bytes32 key{};
		bool allowLoopback = false;

		std::string ownHost, build = BUILD_ID, pluginsHash;
		std::wstring regionsDir, lanDir, storeBase, statusDir;

		// Host.
		std::string code;
		ULONGLONG codeExpires = 0;
		std::string lastBurned;
		bool codeExpired = false, codeBurned = false;
		std::string syncedAt, lastMirrorAt;

		// Session.
		bool connected = false;
		lan::Hello peer;
		std::string peerAddrText, workerStatus;
		bool joinOpen = false;
		ULONGLONG disconnectedAt = 0;

		// Guest sync.
		Sync sync = Sync::Idle;
		bool guestSynced = false;
		bool resync = false;
		bool archiveFolder = false;
		lan::Manifest manifest;
		lan::DiffResult diff;
		std::wstring stagingDir;
		std::map<std::string, std::wstring> staged;
		size_t done = 0, total = 0;
		std::wstring localFolder;   // region folder name on this PC
		HANDLE saveMutex = nullptr;
		std::set<Tile> notSent;

		// Mirror.
		bool mirroring = false;
		std::string mirrorRegion;
		HANDLE chStore = INVALID_HANDLE_VALUE, chStatus = INVALID_HANDLE_VALUE, chSaves = INVALID_HANDLE_VALUE;
		ULONGLONG lastScan = 0;
		std::map<std::string, std::string> sentHash;   // "s:" or "t:" + name
		std::set<std::string> ownNames;
		std::map<std::string, PendingWrite> pending;
		std::set<Tile> openBoth;
		int32_t sizeX = 10000, sizeZ = 10000;
		bool sizeKnown = false;

		// Banners.
		std::string refused, failed, setupError, aliasWarning;
		std::string swapBroken;   // a rollback that could not finish: shown until a sync succeeds
		bool publicNetwork = false, pluginsDiffer = false, gameBlocksSwap = false;

		int modalDepth = 0;
		bool closeDeferred = false;
		std::deque<LanEvent> backlog;   // Manifest, Downloaded, SyncFailed held while a dialog is up
		ULONGLONG lastNotSent = 0;
		bool gameRunning = false;       // refreshed once per LAN timer tick
		uint64_t sessionSeq = 0;        // bumped per session; prompts compare it afterwards
		size_t peerSnapshots = 0;       // peer snapshots in the mirror region; recounted every 5 s
		ULONGLONG lastRecount = 0;
		std::wstring lastArchive;
		size_t reapFailures = 0;
	} S;

	void Bump() { S.version++; }

	// A message box or dialog other than Join is up: LAN handlers wait.
	bool Blocked() { return S.modalDepth > (S.joinOpen ? 1 : 0); }

	// ---- lan.log ----

	std::string Stamp()
	{
		SYSTEMTIME st{};
		GetLocalTime(&st);
		char b[32];
		snprintf(b, sizeof(b), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		return b;
	}

	// Folder names: no colons.
	std::string FolderStamp()
	{
		SYSTEMTIME st{};
		GetLocalTime(&st);
		char b[32];
		snprintf(b, sizeof(b), "%04u-%02u-%02u %02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		return b;
	}

	std::string Clock()
	{
		SYSTEMTIME st{};
		GetLocalTime(&st);
		char b[16];
		snprintf(b, sizeof(b), "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
		return b;
	}

	// Never logs keys or codes.
	void Log(const std::string& text)
	{
		if (S.lanDir.empty()) return;
		const HANDLE f = CreateFileW((S.lanDir + L"\\lan.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f == INVALID_HANDLE_VALUE) return;
		const std::string line = Stamp() + " " + text + "\r\n";
		DWORD written = 0;
		WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
		CloseHandle(f);
	}

	// ---- Files ----

	std::wstring Parent(const std::wstring& path)
	{
		const size_t slash = path.find_last_of(L'\\');
		return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
	}

	bool IsDir(const std::wstring& path)
	{
		const DWORD a = GetFileAttributesW(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

	std::vector<std::wstring> ListFiles(const std::wstring& dir, const wchar_t* pattern)
	{
		std::vector<std::wstring> out;
		WIN32_FIND_DATAW fd{};
		const HANDLE find = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
		if (find == INVALID_HANDLE_VALUE) return out;
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
		} while (FindNextFileW(find, &fd));
		FindClose(find);
		return out;
	}

	std::vector<std::wstring> ListDirs(const std::wstring& dir)
	{
		std::vector<std::wstring> out;
		for (const auto& [name, isDir] : ListDir(dir)) if (isDir) out.push_back(name);
		return out;
	}

	// dir\name, only when the full path's parent is exactly dir (no traversal).
	bool Target(const std::wstring& dir, const std::string& name, std::wstring& out)
	{
		const std::wstring joined = dir + L"\\" + Widen(name);
		wchar_t full[MAX_PATH * 2]{};
		const DWORD n = GetFullPathNameW(joined.c_str(), MAX_PATH * 2, full, nullptr);
		if (n == 0 || n >= MAX_PATH * 2 || _wcsicmp(Parent(full).c_str(), dir.c_str()) != 0) return false;
		out = full;
		return true;
	}


	// Binary tmp plus rename (WriteFileAtomic rules): a failed write never replaces the file.
	// Creates every missing level; CreateDirectoryW alone fails when the parent is absent.
	bool EnsureDir(const std::wstring& dir)
	{
		const int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
		return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
	}

	void LogWriteFailure(const std::wstring& path, DWORD err)
	{
		static std::map<std::wstring, ULONGLONG> last;
		const ULONGLONG now = GetTickCount64();
		if (now - last[path] < 30000) return;
		last[path] = now;
		Log("Write failed: " + Utf8(path) + " error " + std::to_string(err));
	}

	bool WriteAtomic(const std::wstring& path, const void* data, size_t size)
	{
		const std::wstring tmp = path + L".tmp";
		const HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f == INVALID_HANDLE_VALUE)
		{
			LogWriteFailure(path, GetLastError());
			return false;
		}
		DWORD written = 0;
		const bool ok = (size == 0 || (WriteFile(f, data, static_cast<DWORD>(size), &written, nullptr) && written == size))
			&& FlushFileBuffers(f);
		const bool closed = CloseHandle(f) != FALSE;
		if (!ok || !closed || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			LogWriteFailure(path, GetLastError());
			DeleteFileW(tmp.c_str());
			return false;
		}
		return true;
	}

	std::string HashHex(const void* data, size_t size)
	{
		lan::Bytes32 h;
		crypto_blake2b(h.data(), 32, static_cast<const uint8_t*>(data), size);
		return lan::ToHex(h);
	}

	// ---- Settings ----

	std::wstring IniRead(const wchar_t* key)
	{
		wchar_t buffer[2048]{};
		GetPrivateProfileStringW(kSection, key, L"", buffer, 2048, S.ini.c_str());
		return buffer;
	}

	// Only locally generated or format-checked values reach here, so no CR/LF.
	void IniWrite(const wchar_t* key, const std::wstring& value)
	{
		WritePrivateProfileStringW(kSection, key, value.c_str(), S.ini.c_str());
	}

	void IniWrite(const wchar_t* key, const std::string& value) { IniWrite(key, Widen(value)); }

	const wchar_t* RoleName(LanRole r) { return r == LanRole::Host ? L"Host" : r == LanRole::Join ? L"Join" : L""; }

	void SavePeer()
	{
		IniWrite(L"PeerSite", S.peerSite);
		IniWrite(L"PeerSiteId", S.peerSiteId);
		IniWrite(L"PeerAddress", S.peerAddress ? FormatIpv4(S.peerAddress) : std::string());
		std::wstring protectedKey;
		if (S.hasKey && !ProtectKey(S.key, protectedKey)) Log("PeerKey: CryptProtectData failed; the pairing lasts until exit");
		IniWrite(L"PeerKey", protectedKey);
	}

	void ReadIni()
	{
		S.ini = IniPath();
		S.regionsSetting = IniRead(L"RegionsDirectory");
		S.pluginsDir = IniRead(L"PluginsDirectory");
		const std::wstring docs = DocumentsFolder();
		if (S.regionsSetting.empty()) S.regionsSetting = docs + L"\\SimCity 4\\Regions";
		if (S.pluginsDir.empty()) S.pluginsDir = docs + L"\\SimCity 4\\Plugins";
		if (IniRead(L"Port").empty())
		{
			// Visible defaults for the documented keys.
			IniWrite(L"RegionsDirectory", S.regionsSetting);
			IniWrite(L"PluginsDirectory", S.pluginsDir);
			IniWrite(L"Port", std::to_wstring(kDefaultLanPort));
		}
		const int port = GetPrivateProfileIntW(kSection, L"Port", kDefaultLanPort, S.ini.c_str());
		S.port = port >= 1024 && port <= 65534 ? port : kDefaultLanPort;
		S.siteId = Utf8(IniRead(L"SiteId"));
		if (!lan::IsValidSiteId(S.siteId))
		{
			uint8_t id[16];
			if (RandomBytes(id, sizeof(id)))
			{
				S.siteId = lan::ToHex(id);
				IniWrite(L"SiteId", S.siteId);
			}
			else
			{
				S.siteId.clear();   // Prepare refuses Host and Join
				Log("BCryptGenRandom failed: no SiteId");
			}
		}
		const std::wstring role = IniRead(L"Role");
		S.role = _wcsicmp(role.c_str(), L"Host") == 0 ? LanRole::Host : _wcsicmp(role.c_str(), L"Join") == 0 ? LanRole::Join : LanRole::None;
		S.peerSite = Utf8(IniRead(L"PeerSite"));
		S.peerSiteId = Utf8(IniRead(L"PeerSiteId"));
		S.hasKey = lan::IsValidSite(S.peerSite) && lan::IsValidSiteId(S.peerSiteId) && UnprotectKey(IniRead(L"PeerKey"), S.key);
		if (!S.hasKey)
		{
			S.peerSite.clear();
			S.peerSiteId.clear();
		}
		uint32_t addr = 0;
		S.peerAddress = ParseIpv4(Utf8(IniRead(L"PeerAddress")), addr) ? addr : 0;
		S.region = Utf8(IniRead(L"Region"));
		if (!lan::IsValidRegionName(S.region)) S.region.clear();
		S.allowLoopback = GetPrivateProfileIntW(kSection, L"AllowLoopback", 0, S.ini.c_str()) == 1;
		S.hostAlias = Utf8(IniRead(L"HostAlias"));
		if (!lan::IsValidSite(S.hostAlias)) S.hostAlias.clear();
	}

	// ---- Checks done once per Host or Join (UI thread) ----

	bool ResolveDir(const std::wstring& path, std::wstring& out, DWORD& volume)
	{
		const HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;
		wchar_t buf[MAX_PATH * 2]{};
		const DWORD n = GetFinalPathNameByHandleW(h, buf, MAX_PATH * 2, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		BY_HANDLE_FILE_INFORMATION info{};
		const bool ok = n > 0 && n < MAX_PATH * 2 && GetFileInformationByHandle(h, &info);
		CloseHandle(h);
		if (!ok) return false;
		out = buf;
		if (out.rfind(L"\\\\?\\UNC\\", 0) == 0) out = L"\\\\" + out.substr(8);
		else if (out.rfind(L"\\\\?\\", 0) == 0) out = out.substr(4);
		volume = info.dwVolumeSerialNumber;
		return true;
	}

	bool FileVersion(const std::wstring& path, uint64_t& version)
	{
		DWORD ignored = 0;
		const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
		if (size == 0) return false;
		std::vector<uint8_t> data(size);
		VS_FIXEDFILEINFO* info = nullptr;
		UINT len = 0;
		if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len)
			|| !info)
		{
			return false;
		}
		version = (static_cast<uint64_t>(info->dwFileVersionMS) << 32) | info->dwFileVersionLS;
		return true;
	}

	std::string VersionText(uint64_t v)
	{
		return std::to_string(v >> 48) + "." + std::to_string((v >> 32) & 0xFFFF) + "." + std::to_string((v >> 16) & 0xFFFF) + "."
			+ std::to_string(v & 0xFFFF);
	}

	bool Skipped(const std::wstring& rel, bool isDir)
	{
		const std::wstring name = rel.substr(rel.find_last_of(L'\\') + 1);
		if (isDir) return _wcsicmp(rel.c_str(), L"RegionSync") == 0;
		return _wcsnicmp(name.c_str(), L"SC4RegionSync.", 14) == 0
			|| (name.size() >= 4 && _wcsicmp(name.c_str() + name.size() - 4, L".log") == 0);
	}

	void CollectPlugins(const std::wstring& root, const std::wstring& rel, std::vector<std::string>& lines, int depth)
	{
		if (depth > 32) return;
		WIN32_FIND_DATAW fd{};
		const std::wstring dir = rel.empty() ? root : root + L"\\" + rel;
		const HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
		if (find == INVALID_HANDLE_VALUE) return;
		do
		{
			const std::wstring name = fd.cFileName;
			if (name == L"." || name == L"..") continue;
			const std::wstring r = rel.empty() ? name : rel + L"\\" + name;
			const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			if (Skipped(r, isDir)) continue;
			if (isDir)
			{
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) CollectPlugins(root, r, lines, depth + 1);
				continue;
			}
			std::wstring lower = r;
			for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
			const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
			lines.push_back(Utf8(lower) + "|" + std::to_string(size));
		} while (FindNextFileW(find, &fd));
		FindClose(find);
	}

	std::string PluginsHash()
	{
		std::vector<std::string> lines;
		CollectPlugins(S.pluginsDir, L"", lines, 0);
		std::sort(lines.begin(), lines.end());
		crypto_blake2b_ctx ctx;
		crypto_blake2b_init(&ctx, 32);
		for (const std::string& l : lines)
		{
			crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(l.data()), l.size());
			crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>("\n"), 1);
		}
		lan::Bytes32 h;
		crypto_blake2b_final(&ctx, h.data());
		return lan::ToHex(h);
	}

	bool PublicNetwork()
	{
		bool isPublic = false;
		INetworkListManager* nlm = nullptr;
		if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, __uuidof(INetworkListManager),
			reinterpret_cast<void**>(&nlm))) || !nlm)
		{
			return false;
		}
		IEnumNetworks* nets = nullptr;
		if (SUCCEEDED(nlm->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &nets)) && nets)
		{
			INetwork* net = nullptr;
			ULONG fetched = 0;
			while (nets->Next(1, &net, &fetched) == S_OK && net)
			{
				NLM_NETWORK_CATEGORY cat = NLM_NETWORK_CATEGORY_PRIVATE;
				if (SUCCEEDED(net->GetCategory(&cat)) && cat == NLM_NETWORK_CATEGORY_PUBLIC) isPublic = true;
				net->Release();
				net = nullptr;
			}
			nets->Release();
		}
		nlm->Release();
		return isPublic;
	}

	// Folders, volume, versions and the Plugins hash. False sets setupError.
	bool Prepare()
	{
		S.setupError.clear();
		S.statusDir = S.settings.statusDirectory;
		while (!S.statusDir.empty() && (S.statusDir.back() == L'\\' || S.statusDir.back() == L'/')) S.statusDir.pop_back();
		S.storeBase = Parent(S.statusDir);
		if (!lan::IsValidSite(S.ownHost))
		{
			S.setupError = "this PC's name '" + S.ownHost + "' is not usable on the LAN (1 to 15 letters, digits or '-')";
			return false;
		}
		DWORD regionsVolume = 0, lanVolume = 0;
		if (!ResolveDir(S.regionsSetting, S.regionsDir, regionsVolume))
		{
			S.setupError = "Regions folder not found: " + Utf8(S.regionsSetting);
			return false;
		}
		S.lanDir = Parent(S.regionsDir) + L"\\RegionSyncLAN";
		EnsureDir(S.lanDir);
		std::wstring resolvedLan;
		if (!ResolveDir(S.lanDir, resolvedLan, lanVolume) || lanVolume != regionsVolume)
		{
			S.setupError = "RegionSyncLAN is not on the same volume as Regions";
			return false;
		}
		for (const wchar_t* sub : { L"\\staging", L"\\archive", L"\\journal" }) EnsureDir(S.lanDir + sub);
		wchar_t exe[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		uint64_t own = 0, dll = 0;
		if (!FileVersion(exe, own) || !FileVersion(S.pluginsDir + L"\\SC4RegionSync.dll", dll) || own != dll)
		{
			S.setupError = "refused: version (plugin " + (dll ? VersionText(dll) : std::string("not found")) + ", launcher " + VersionText(own) + ")";
			return false;
		}
		S.pluginsHash = PluginsHash();
		return true;
	}

	LanConfig Config(LanRole role)
	{
		LanConfig c;
		c.role = role;
		c.port = S.port;
		c.allowLoopback = S.allowLoopback;
		c.site = S.ownHost;
		c.siteId = S.siteId;
		c.build = S.build;
		c.pluginsHash = S.pluginsHash;
		c.regionsDir = S.regionsDir;
		c.lanDir = S.lanDir;
		c.peerSite = S.peerSite;
		c.peerSiteId = S.peerSiteId;
		c.hasKey = S.hasKey;
		c.peerKey = S.key;
		c.peerAddress = S.peerAddress;
		return c;
	}

	void Post(LanCmd::Kind kind, std::string text = {})
	{
		LanCmd c;
		c.kind = kind;
		c.text = std::move(text);
		S.worker.Post(std::move(c));
	}

	void SendFrame(lan::FrameType type, std::string text, std::vector<uint8_t> data = {})
	{
		LanCmd c;
		c.kind = LanCmd::Kind::Send;
		c.type = type;
		c.frameText = std::move(text);
		c.data = std::move(data);
		S.worker.Post(std::move(c));
	}

	void StartWorker(LanRole role)
	{
		LanCmd c;
		c.kind = LanCmd::Kind::Start;
		c.config = Config(role);
		S.worker.Post(std::move(c));
		crypto_wipe(c.config.peerKey.data(), c.config.peerKey.size());   // moved-from copy
	}

	void NewCodeNow()
	{
		uint32_t r = 0;
		if (!RandomBytes(&r, sizeof(r)))
		{
			Log("BCryptGenRandom failed: no pairing code");
			S.code.clear();
			Bump();
			return;
		}
		char b[8];
		snprintf(b, sizeof(b), "%06u", r % 1000000u);
		S.code = b;
		S.codeExpires = GetTickCount64() + kCodeLifetimeMs;
		S.codeExpired = false;
		S.codeBurned = false;
		Post(LanCmd::Kind::SetCode, S.code);
		Bump();
	}

	void ClearCode()
	{
		S.code.clear();
		Post(LanCmd::Kind::SetCode, {});
		Bump();
	}

	// ---- Processes and status files ----

	bool AnyGameProcess()
	{
		const std::wstring& exe = S.settings.gameExe;
		const size_t slash = exe.find_last_of(L"\\/");
		const std::wstring name = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
		const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE) return true;   // cannot tell: treat as running
		PROCESSENTRY32W e{};
		e.dwSize = sizeof(e);
		bool found = false;
		for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
		{
			found = _wcsicmp(e.szExeFile, L"SimCity 4.exe") == 0 || (!name.empty() && _wcsicmp(e.szExeFile, name.c_str()) == 0);
		}
		CloseHandle(snap);
		return found;
	}

	struct LocalStatus
	{
		std::wstring name;
		regionsync::InstanceStatus s;
	};

	std::vector<LocalStatus> ReadStatusDir()
	{
		std::vector<LocalStatus> out;
		for (const std::wstring& name : ListFiles(S.statusDir, L"*.status"))
		{
			std::string text;
			if (!ReadSharedFile(S.statusDir + L"\\" + name, text, kMaxStatus * 4)) continue;
			LocalStatus l;
			l.name = name;
			regionsync::ParseInstanceStatus(text, l.s);
			out.push_back(std::move(l));
		}
		return out;
	}

	std::map<Tile, regionsync::SaveCounter> ReadSaves(const std::string& region)
	{
		std::map<Tile, regionsync::SaveCounter> out;
		const std::wstring dir = S.storeBase + L"\\_saves\\" + Widen(region);
		for (const std::wstring& name : ListFiles(dir, L"*.saves"))
		{
			Tile t;
			const std::string n = Utf8(name);
			std::string text;
			if (!lan::ParseTileKey(n.substr(0, n.size() - 6), t) || !ReadSharedFile(dir + L"\\" + name, text, 65536)) continue;
			regionsync::ParseSaveCounter(text, out[t]);
		}
		return out;
	}

	std::set<Tile> SaveCountFailed(const std::string& region)
	{
		std::set<Tile> out;
		for (const LocalStatus& l : ReadStatusDir())
		{
			if (!SameHost(l.s.writer.host, S.ownHost) || !lan::EqualsNoCase(l.s.region, region)) continue;
			for (const auto& [x, z] : l.s.saveCountFailedTiles) out.insert(Tile{ x, z });
		}
		return out;
	}

	std::wstring JournalPath(const std::string& region)
	{
		return S.lanDir + L"\\journal\\" + Widen(S.peerSiteId) + L"\\" + Widen(region) + L".txt";
	}

	lan::Journal LoadJournal(const std::string& region, bool& known)
	{
		lan::Journal j;
		std::string text, error;
		known = ReadSharedFile(JournalPath(region), text, 1 << 20);
		if (known && !lan::ParseJournalText(text, j, error))
		{
			Log("Journal for " + region + " unreadable (" + error + "); treated as empty");
			j = lan::Journal{};
		}
		return j;
	}

	void RefreshNotSent()
	{
		if (S.role != LanRole::Join || S.region.empty() || S.peerSiteId.empty()) return;
		bool known = false;
		const lan::Journal j = LoadJournal(S.region, known);
		std::set<Tile> ns = SaveCountFailed(S.region);
		for (const auto& [t, c] : ReadSaves(S.region))
		{
			const auto je = j.tiles.find(t);
			if (c.count > (je != j.tiles.end() ? je->second.baseCount : 0u)) ns.insert(t);
		}
		if (ns != S.notSent)
		{
			S.notSent = std::move(ns);
			Bump();
		}
	}

	// config.bmp pixel size is the region's tile grid.
	// False (and no snapshot PUTs accepted) when config.bmp is unreadable.
	void ReadRegionSize(const std::wstring& regionDir)
	{
		S.sizeKnown = false;
		S.sizeX = S.sizeZ = 10000;
		std::string head;
		const HANDLE f = CreateFileW((regionDir + L"\\config.bmp").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, 0, nullptr);
		if (f == INVALID_HANDLE_VALUE) return;
		uint8_t b[26]{};
		DWORD read = 0;
		const bool ok = ReadFile(f, b, sizeof(b), &read, nullptr) && read == sizeof(b) && b[0] == 'B' && b[1] == 'M';
		CloseHandle(f);
		if (!ok) return;
		const int32_t w = static_cast<int32_t>(b[18] | b[19] << 8 | b[20] << 16 | b[21] << 24);
		int32_t h = static_cast<int32_t>(b[22] | b[23] << 8 | b[24] << 16 | b[25] << 24);
		if (h < 0) h = -h;
		if (w > 0 && w <= 10000 && h > 0 && h <= 10000)
		{
			S.sizeX = w;
			S.sizeZ = h;
			S.sizeKnown = true;
		}
	}

	// ---- Mirroring (section 5) ----

	struct Writer
	{
		regionsync::WriterIdentity id;
		int32_t x = -1, z = -1;
	};

	Writer SnapshotWriter(const std::string& text)
	{
		regionsync::KeyValues kv;
		regionsync::ParseKeyValues(text, kv);
		Writer w;
		w.id.host = lan::Get(kv, "writerHost");
		regionsync::ReadKeyValue(kv, "writerPid", w.id.pid);
		regionsync::ReadKeyValue(kv, "writerStart", w.id.start);
		regionsync::ReadKeyValue(kv, "regionX", w.x);
		regionsync::ReadKeyValue(kv, "regionZ", w.z);
		return w;
	}

	std::wstring StoreDir() { return S.storeBase + L"\\" + Widen(S.mirrorRegion); }

	void CloseChange(HANDLE& h)
	{
		if (h != INVALID_HANDLE_VALUE) FindCloseChangeNotification(h);
		h = INVALID_HANDLE_VALUE;
	}

	void StopMirror()
	{
		S.mirroring = false;
		CloseChange(S.chStore);
		CloseChange(S.chStatus);
		CloseChange(S.chSaves);
		S.sentHash.clear();
		S.ownNames.clear();
		S.pending.clear();
		S.openBoth.clear();
		Bump();
	}

	// One pass: PUT what changed, DELETE what vanished. initial sends INDEX first.
	void Scan(bool initial)
	{
		struct Out { std::string kind, name; std::vector<uint8_t> bytes; };
		std::vector<Out> puts, deletes;
		std::map<std::string, std::vector<std::string>> index;   // kind -> names
		std::set<std::string> present;
		std::string warn;

		for (const std::wstring& wname : ListFiles(S.statusDir, L"*.status"))
		{
			const std::string name = Utf8(wname);
			uint32_t pid = 0;
			uint64_t start = 0;
			std::string text;
			if (!lan::ParseStatusName(name, pid, start) || !ReadSharedFile(S.statusDir + L"\\" + wname, text, kMaxStatus)) continue;
			regionsync::InstanceStatus st;
			regionsync::ParseInstanceStatus(text, st);
			if (!SameHost(st.writer.host, S.ownHost))
			{
				if (S.allowLoopback && !S.hostAlias.empty() && st.writer.Known() && !SameHost(st.writer.host, S.peerSite))
				{
					warn = "status file from " + st.writer.host + " does not match HostAlias " + S.hostAlias;
				}
				continue;
			}
			if (st.writer.pid != pid || st.writer.start != start) continue;
			const std::string key = "t:" + name;
			present.insert(key);
			index["status"].push_back(name);
			const std::string h = HashHex(text.data(), text.size());
			if (S.sentHash[key] != h) puts.push_back({ "status", name, std::vector<uint8_t>(text.begin(), text.end()) }), S.sentHash[key] = h;
		}
		for (const std::wstring& wname : ListFiles(StoreDir(), L"*.snapshot"))
		{
			const std::string name = Utf8(wname);
			Tile t;
			std::string text;
			if (!lan::ParseSnapshotName(name, S.sizeX, S.sizeZ, t) || !ReadSharedFile(StoreDir() + L"\\" + wname, text, kMaxSnapshot)) continue;
			const Writer w = SnapshotWriter(text);
			if (!SameHost(w.id.host, S.ownHost) || w.x != t.x || w.z != t.z) continue;
			const std::string key = "s:" + name;
			present.insert(key);
			index["snapshot"].push_back(name);
			const std::string h = HashHex(text.data(), text.size());
			if (S.sentHash[key] != h) puts.push_back({ "snapshot", name, std::vector<uint8_t>(text.begin(), text.end()) }), S.sentHash[key] = h;
		}
		for (const std::string& key : S.ownNames)
		{
			if (!present.count(key)) deletes.push_back({ key[0] == 's' ? "snapshot" : "status", key.substr(2), {} });
		}
		for (const Out& d : deletes) S.sentHash.erase((d.kind == "snapshot" ? "s:" : "t:") + d.name);
		S.ownNames = present;
		if (warn != S.aliasWarning)
		{
			S.aliasWarning = warn;
			Bump();
		}

		if (initial)
		{
			for (const char* kind : { "status", "snapshot" })
			{
				lan::Fields f;
				f.Add("kind", kind);
				if (std::string(kind) == "snapshot") f.Add("region", S.mirrorRegion);
				const auto& names = index[kind];
				f.Add("count", names.size());
				for (size_t i = 0; i < names.size(); i++) f.Add("name." + std::to_string(i), names[i]);
				if (f.Ok()) SendFrame(lan::FrameType::Index, f.Text());
			}
		}
		// Status before snapshots on PUT; snapshot DELETE before status DELETE.
		std::stable_sort(puts.begin(), puts.end(), [](const Out& a, const Out& b) { return a.kind == "status" && b.kind != "status"; });
		std::stable_sort(deletes.begin(), deletes.end(), [](const Out& a, const Out& b) { return a.kind == "snapshot" && b.kind != "snapshot"; });
		for (Out& p : puts)
		{
			lan::Fields f;
			f.Add("kind", p.kind).Add("name", p.name);
			if (p.kind == "snapshot") f.Add("region", S.mirrorRegion);
			SendFrame(lan::FrameType::Put, f.Text(), std::move(p.bytes));
		}
		for (const Out& d : deletes)
		{
			lan::Fields f;
			f.Add("kind", d.kind).Add("name", d.name);
			if (d.kind == "snapshot") f.Add("region", S.mirrorRegion);
			SendFrame(lan::FrameType::Delete, f.Text());
		}
		if (!puts.empty() || !deletes.empty())
		{
			S.lastMirrorAt = Clock();
			Bump();
		}
	}

	HANDLE Watch(const std::wstring& dir)
	{
		if (!EnsureDir(dir)) Log("Cannot create " + Utf8(dir));
		return FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
	}

	void OpenWatches()
	{
		if (S.chStore == INVALID_HANDLE_VALUE) S.chStore = Watch(StoreDir());
		if (S.chStatus == INVALID_HANDLE_VALUE) S.chStatus = Watch(S.statusDir);
		EnsureDir(S.storeBase + L"\\_saves");
		if (S.chSaves == INVALID_HANDLE_VALUE) S.chSaves = Watch(S.storeBase + L"\\_saves\\" + Widen(S.mirrorRegion));
	}

	size_t PeerSnapshotCount();

	void StartMirror(const std::string& region)
	{
		if (S.mirroring && lan::EqualsNoCase(S.mirrorRegion, region)) return;
		StopMirror();
		if (!IsSyncableRegion(region))
		{
			Log("Mirror refused for region '" + region + "' (reserved or invalid name)");
			return;
		}
		S.mirrorRegion = region;
		S.mirroring = true;
		ReadRegionSize(S.regionsDir + L"\\" + Widen(region));
		OpenWatches();
		Log("Mirroring " + region + " with " + S.peerSite);
		S.peerSnapshots = PeerSnapshotCount();
		S.lastRecount = GetTickCount64();
		Scan(true);
		S.lastScan = GetTickCount64();
	}

	// A handle that cannot be re-armed (folder removed or renamed) is closed; OpenWatches reopens it.
	bool Signalled(HANDLE& h)
	{
		if (h == INVALID_HANDLE_VALUE || WaitForSingleObject(h, 0) != WAIT_OBJECT_0) return false;
		if (!FindNextChangeNotification(h)) CloseChange(h);
		return true;
	}

	// The tile is open here: a live local snapshot writer, or a local status in city state on it.
	bool TileOpenHere(Tile t)
	{
		std::string text;
		if (ReadSharedFile(StoreDir() + L"\\" + Widen(regionsync::SnapshotFileName(t.x, t.z)), text, kMaxSnapshot))
		{
			const Writer w = SnapshotWriter(text);
			if (SameHost(w.id.host, S.ownHost) && regionsync::writer::IsAlive(w.id) == regionsync::writer::Liveness::Alive) return true;
		}
		for (const LocalStatus& l : ReadStatusDir())
		{
			if (SameHost(l.s.writer.host, S.ownHost) && l.s.state == "city" && lan::EqualsNoCase(l.s.region, S.mirrorRegion)
				&& l.s.regionX == t.x && l.s.regionZ == t.z)
			{
				return true;
			}
		}
		return false;
	}

	bool PeerWrote(const std::wstring& path, bool snapshot)
	{
		std::string text;
		if (!ReadSharedFile(path, text, snapshot ? kMaxSnapshot : kMaxStatus)) return false;
		if (snapshot) return SameHost(SnapshotWriter(text).id.host, S.peerSite);
		regionsync::InstanceStatus st;
		regionsync::ParseInstanceStatus(text, st);
		return SameHost(st.writer.host, S.peerSite);
	}

	size_t PeerStatusCount(const std::string& except)
	{
		size_t n = 0;
		for (const LocalStatus& l : ReadStatusDir())
		{
			if (SameHost(l.s.writer.host, S.peerSite) && Utf8(l.name) != except) n++;
		}
		return n;
	}

	size_t PeerSnapshotCount()
	{
		size_t n = 0;
		for (const std::wstring& name : ListFiles(StoreDir(), L"*.snapshot"))
		{
			if (PeerWrote(StoreDir() + L"\\" + name, true)) n++;
		}
		return n;
	}

	void Cap(const std::string& why)
	{
		Log("Cap: " + why + "; BYE reason=cap");
		Post(LanCmd::Kind::Close, "cap");
	}

	// True when done (written, skipped or rejected); false keeps it pending.
	bool ApplyPut(const PendingWrite& p)
	{
		const std::string text(p.bytes.begin(), p.bytes.end());
		if (p.kind == "status")
		{
			uint32_t pid = 0;
			uint64_t start = 0;
			if (!lan::ParseStatusName(p.name, pid, start)) return true;
			regionsync::InstanceStatus st;
			regionsync::ParseInstanceStatus(text, st);
			if (!SameHost(st.writer.host, S.peerSite) || SameHost(st.writer.host, S.ownHost) || st.writer.pid != pid || st.writer.start != start)
			{
				Log("PUT status " + p.name + " rejected: writer does not match the peer or the name");
				return true;
			}
			std::wstring target;
			if (!Target(S.statusDir, p.name, target)) return true;
			std::string local;
			if (ReadSharedFile(target, local, kMaxStatus) && local == text) return true;
			if (Exists(target) && !PeerWrote(target, false))
			{
				Log("PUT status " + p.name + " rejected: a local writer owns that file");
				return true;
			}
			if (!Exists(target) && PeerStatusCount(p.name) + 1 > static_cast<size_t>(S.settings.maxInstances))
			{
				Cap("more than MaxInstances status files from " + S.peerSite);
				return true;
			}
			return WriteAtomic(target, p.bytes.data(), p.bytes.size());
		}
		Tile t;
		if (!S.sizeKnown)
		{
			Log("PUT snapshot " + p.name + " refused: config.bmp of " + S.mirrorRegion + " is unreadable");
			return true;
		}
		if (!lan::ParseSnapshotName(p.name, S.sizeX, S.sizeZ, t)) return true;
		const Writer w = SnapshotWriter(text);
		if (!SameHost(w.id.host, S.peerSite) || SameHost(w.id.host, S.ownHost) || w.x != t.x || w.z != t.z)
		{
			Log("PUT snapshot " + p.name + " rejected: writer or tile does not match");
			return true;
		}
		std::wstring target;
		if (!Target(StoreDir(), p.name, target)) return true;
		// Open here first: the held bytes wait until the local city closes. The live writer's
		// next publish wins any race with a write below (no restore step).
		if (TileOpenHere(t) || (Exists(target) && !PeerWrote(target, true)))
		{
			if (S.openBoth.insert(t).second)
			{
				Log("Tile (" + std::to_string(t.x) + "," + std::to_string(t.z) + ") has a local snapshot or is open here; peer snapshot held");
				Bump();
			}
			return false;
		}
		std::string local;
		if (!(ReadSharedFile(target, local, kMaxSnapshot) && local == text))
		{
			const bool isNew = !Exists(target);
			if (isNew && S.peerSnapshots + 1 > kMaxPeerSnapshots)
			{
				Cap("more than " + std::to_string(kMaxPeerSnapshots) + " snapshots from " + S.peerSite + " in " + S.mirrorRegion);
				return true;
			}
			if (!WriteAtomic(target, p.bytes.data(), p.bytes.size())) return false;
			if (isNew) S.peerSnapshots++;
		}
		if (S.openBoth.erase(t)) Bump();
		return true;
	}

	void OnPut(const lan::KeyValues& kv, std::vector<uint8_t>& data)
	{
		PendingWrite p{ lan::Get(kv, "kind"), lan::Get(kv, "name"), std::move(data) };
		if (p.kind == "status" && p.bytes.size() > kMaxStatus) return Cap("status file over 16 KiB");
		if (p.kind == "snapshot" && p.bytes.size() > kMaxSnapshot) return Cap("snapshot over 256 KiB");
		if (p.kind != "status" && p.kind != "snapshot") return;
		if (p.kind == "snapshot" && (!S.mirroring || !lan::EqualsNoCase(lan::Get(kv, "region"), S.mirrorRegion))) return;
		const std::string key = (p.kind == "snapshot" ? "s:" : "t:") + p.name;
		if (ApplyPut(p)) S.pending.erase(key);
		else S.pending[key] = std::move(p);   // one slot per name: the latest bytes
		S.lastMirrorAt = Clock();
		Bump();
	}

	void OnDelete(const lan::KeyValues& kv)
	{
		const std::string kind = lan::Get(kv, "kind"), name = lan::Get(kv, "name");
		const bool snapshot = kind == "snapshot";
		Tile t;
		uint32_t pid = 0;
		uint64_t start = 0;
		if (snapshot ? !S.mirroring || !lan::ParseSnapshotName(name, S.sizeX, S.sizeZ, t) : !lan::ParseStatusName(name, pid, start)) return;
		std::wstring target;
		if (!Target(snapshot ? StoreDir() : S.statusDir, name, target)) return;
		S.pending.erase((snapshot ? "s:" : "t:") + name);
		if (PeerWrote(target, snapshot) && !DeleteFileW(target.c_str()))
		{
			Log("DELETE " + name + " failed, error " + std::to_string(GetLastError()));
		}
	}

	void OnIndex(const lan::KeyValues& kv)
	{
		const std::string kind = lan::Get(kv, "kind");
		const bool snapshot = kind == "snapshot";
		if (kind != "snapshot" && kind != "status") return;
		if (snapshot && (!S.mirroring || !lan::EqualsNoCase(lan::Get(kv, "region"), S.mirrorRegion))) return;
		std::set<std::string> listed;
		for (const auto& [k, v] : kv) if (k.rfind("name.", 0) == 0) listed.insert(v);
		const std::wstring dir = snapshot ? StoreDir() : S.statusDir;
		for (const std::wstring& wname : ListFiles(dir, snapshot ? L"*.snapshot" : L"*.status"))
		{
			if (!listed.count(Utf8(wname)) && PeerWrote(dir + L"\\" + wname, snapshot) && !DeleteFileW((dir + L"\\" + wname).c_str()))
			{
				Log("INDEX: deleting " + Utf8(wname) + " failed, error " + std::to_string(GetLastError()));
			}
		}
	}

	// After 10 minutes without the peer: its status files, then its snapshots. False when a delete
	// failed (the plugin may hold a snapshot open without FILE_SHARE_DELETE); the caller retries.
	bool ReapPeer()
	{
		if (S.peerSite.empty()) return true;
		size_t failures = 0;
		const auto remove = [&](const std::wstring& path)
		{
			if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) failures++;
		};
		for (const std::wstring& n : ListFiles(S.statusDir, L"*.status"))
		{
			if (PeerWrote(S.statusDir + L"\\" + n, false)) remove(S.statusDir + L"\\" + n);
		}
		for (const std::wstring& d : ListDirs(S.storeBase))
		{
			if (d[0] == L'_') continue;
			const std::wstring dir = S.storeBase + L"\\" + d;
			for (const std::wstring& n : ListFiles(dir, L"*.snapshot"))
			{
				if (PeerWrote(dir + L"\\" + n, true)) remove(dir + L"\\" + n);
			}
		}
		if (failures && S.reapFailures == 0) Log("Reap of " + S.peerSite + ": " + std::to_string(failures) + " file(s) busy; retrying");
		S.reapFailures = failures;
		if (!failures) Log("Peer " + S.peerSite + " gone for 10 minutes: its status files and snapshots are removed");
		return failures == 0;
	}

	// ---- Guest sync (section 4) ----

	void FailSync(const std::string& text)
	{
		S.failed = text;
		S.sync = Sync::Failed;
		Log("Sync failed: " + text);
		Bump();
	}

	std::wstring ExistingFolderName(const std::string& region)
	{
		for (const std::wstring& d : ListDirs(S.regionsDir))
		{
			if (lan::EqualsNoCase(Utf8(d), region)) return d;
		}
		return {};
	}

	void OnManifest(LanEvent& e)
	{
		S.manifest = std::move(e.manifest);
		const std::string& region = S.manifest.region;
		S.localFolder = ExistingFolderName(region);
		const bool exists = !S.localFolder.empty();
		if (exists && (GetFileAttributesW((S.regionsDir + L"\\" + S.localFolder).c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
		{
			return FailSync("the region folder " + region + " is a link; not synced");
		}
		if (!exists) S.localFolder = Widen(region);
		bool known = false;
		lan::Journal j = LoadJournal(region, known);
		S.archiveFolder = false;
		std::map<std::string, std::string> local = std::move(e.localHashes);
		if (exists && !known && !local.empty() && !S.resync)
		{
			const uint64_t session = S.sessionSeq;
			const bool yes = ConfirmYesNo(S.main, L"This PC already has a region folder \"" + S.localFolder + L"\" that was never synced with "
				+ Widen(S.peerSite) + L".\n\nMove it to RegionSyncLAN\\archive and download the host's copy?");
			if (!yes)
			{
				Post(LanCmd::Kind::Close, "declined");
				return FailSync("sync declined; the local region was left as it is");
			}
			if (S.sessionSeq != session || !S.connected) return;   // a new session came up during the prompt; it diffs again
			S.archiveFolder = true;
			local.clear();
		}
		const bool full = S.archiveFolder || S.resync;
		const auto saves = full ? std::map<Tile, regionsync::SaveCounter>{} : ReadSaves(region);
		const auto failed = full ? std::set<Tile>{} : SaveCountFailed(region);
		S.diff = lan::DiffRegion(S.manifest, local, saves, full ? lan::Journal{} : j, failed);
		ULARGE_INTEGER freeBytes{};
		if (!GetDiskFreeSpaceExW(S.lanDir.c_str(), &freeBytes, nullptr, nullptr) || freeBytes.QuadPart < S.diff.downloadBytes + (64ull << 20))
		{
			Post(LanCmd::Kind::Close, "space");
			return FailSync("not enough free space for " + std::to_string(S.diff.downloadBytes >> 20) + " MiB of downloads");
		}
		LanCmd c;
		c.kind = LanCmd::Kind::Download;
		for (const auto& item : S.diff.items) if (item.action == lan::DiffAction::Download) c.names.push_back(item.name);
		S.total = c.names.size();
		S.done = 0;
		S.sync = Sync::Downloading;
		Log("Diff " + region + ": " + std::to_string(S.total) + " to download, " + std::to_string(S.diff.keptTiles.size()) + " tiles kept (not sent)");
		S.worker.Post(std::move(c));
		Bump();
	}

	struct Move
	{
		std::wstring from, to;
		bool createdDir = false;
		bool fromOwned = false;   // from is our staging file: moving back may replace
	};

	bool DoMove(const std::wstring& from, const std::wstring& to, DWORD flags, std::vector<Move>& done, std::string& error, bool fromOwned = false)
	{
		if (MoveFileExW(from.c_str(), to.c_str(), flags))
		{
			done.push_back({ from, to, false, fromOwned });
			return true;
		}
		error = "moving " + Utf8(from) + " failed, error " + std::to_string(GetLastError());
		return false;
	}

	// False when anything could not be put back.
	bool Rollback(std::vector<Move>& done)
	{
		bool ok = true;
		for (auto it = done.rbegin(); it != done.rend(); ++it)
		{
			const DWORD flags = MOVEFILE_WRITE_THROUGH | (it->fromOwned ? MOVEFILE_REPLACE_EXISTING : 0);
			if (it->createdDir) RemoveDirectoryW(it->to.c_str());
			else if (!MoveFileExW(it->to.c_str(), it->from.c_str(), flags))
			{
				Log("Rollback: could not move " + Utf8(it->to) + " back, error " + std::to_string(GetLastError()));
				ok = false;
			}
		}
		done.clear();
		return ok;
	}

	bool CreateDir(const std::wstring& dir, std::vector<Move>& done)
	{
		if (IsDir(dir)) return true;
		if (!CreateDirectoryW(dir.c_str(), nullptr)) return false;
		done.push_back({ {}, dir, true });
		return true;
	}

	enum class SwapResult { Ok, RolledBack, RollbackFailed };

	// The swap proper, under the save mutex.
	SwapResult SwapFiles(std::string& error)
	{
		const std::wstring targetDir = S.regionsDir + L"\\" + S.localFolder;
		if (Exists(targetDir) && (GetFileAttributesW(targetDir.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
		{
			error = "the region folder is a link";
			return SwapResult::RolledBack;   // nothing was moved
		}
		const std::wstring archiveBase = S.lanDir + L"\\archive\\" + Widen(FolderStamp());
		const std::wstring archiveRegion = archiveBase + L"\\" + S.localFolder;
		S.lastArchive = archiveRegion;
		Log("Swap archive folder: " + Utf8(archiveRegion));
		std::vector<Move> done;
		const auto ensureArchive = [&]() { return CreateDir(archiveBase, done) && CreateDir(archiveRegion, done); };
		const DWORD replace = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
		const auto fail = [&](const std::string& fallback)
		{
			if (error.empty()) error = fallback;
			return Rollback(done) ? SwapResult::RolledBack : SwapResult::RollbackFailed;
		};

		if (S.archiveFolder && (!CreateDir(archiveBase, done) || !DoMove(targetDir, archiveRegion, MOVEFILE_WRITE_THROUGH, done, error)))
		{
			return fail("cannot create the archive folder");
		}
		if (!CreateDir(targetDir, done)) return fail("cannot create " + Utf8(targetDir));
		for (const auto& item : S.diff.items)
		{
			if (item.action != lan::DiffAction::Download && item.action != lan::DiffAction::Archive) continue;
			std::wstring original, target;
			if (!item.localName.empty() && !S.archiveFolder)
			{
				std::wstring dest;
				if (!Target(targetDir, item.localName, original) || !ensureArchive() || !Target(archiveRegion, item.localName, dest)
					|| !DoMove(original, dest, MOVEFILE_WRITE_THROUGH, done, error))
				{
					return fail("archiving " + item.localName + " failed");
				}
			}
			if (item.action == lan::DiffAction::Archive) continue;
			const auto st = S.staged.find(item.name);
			if (st == S.staged.end() || !Target(targetDir, item.name, target) || !DoMove(st->second, target, replace, done, error, true))
			{
				return fail(item.name + " is missing from staging");
			}
		}
		return SwapResult::Ok;
	}

	void FinishSync()
	{
		const std::string& region = S.manifest.region;
		bool known = false;
		lan::Journal j = (S.archiveFolder || S.resync) ? lan::Journal{} : LoadJournal(region, known);
		lan::UpdateJournalAfterSwap(j, S.manifest, ReadSaves(region), S.diff.keptTiles);
		EnsureDir(S.lanDir + L"\\journal\\" + Widen(S.peerSiteId));
		const std::string text = lan::BuildJournalText(j);
		if (!WriteAtomic(JournalPath(region), text.data(), text.size()))
		{
			// No SYNCED: the next connect diffs again.
			DeleteTree(S.stagingDir);
			S.stagingDir.clear();
			S.staged.clear();
			return FailSync("journal write failed for " + region);
		}
		size_t downloaded = 0, same = 0;
		for (const auto& item : S.diff.items)
		{
			if (item.action == lan::DiffAction::Download) downloaded++;
			if (item.action == lan::DiffAction::Same) same++;
		}
		SendFrame(lan::FrameType::Synced, lan::Fields().Add("region", region).Add("downloaded", downloaded).Add("kept", same)
			.Add("notSent", S.diff.keptTiles.size()).Text());
		DeleteTree(S.stagingDir);
		S.stagingDir.clear();
		S.staged.clear();
		S.sync = Sync::Synced;
		S.guestSynced = true;
		if (S.mirroring) ReadRegionSize(S.regionsDir + L"\\" + Widen(S.mirrorRegion));
		S.resync = false;
		S.failed.clear();
		S.swapBroken.clear();
		S.gameBlocksSwap = false;
		S.syncedAt = Clock();
		Log("Synced " + region + ": " + std::to_string(downloaded) + " downloaded, " + std::to_string(same) + " unchanged, "
			+ std::to_string(S.diff.keptTiles.size()) + " tiles not sent");
		RefreshNotSent();
		if (S.connected) StartMirror(Utf8(S.localFolder));
		Bump();
	}

	void ReleaseSaveMutex()
	{
		if (!S.saveMutex) return;
		ReleaseMutex(S.saveMutex);
		CloseHandle(S.saveMutex);
		S.saveMutex = nullptr;
	}

	void DropStaging()
	{
		DeleteTree(S.stagingDir);
		S.stagingDir.clear();
		S.staged.clear();
	}

	// Retried on every LAN timer tick (250 ms) until it holds the save mutex or a game starts.
	void TrySwap()
	{
		if (S.sync != Sync::SwapPending || Blocked()) return;
		if (S.gameRunning)
		{
			if (!S.gameBlocksSwap)
			{
				S.gameBlocksSwap = true;
				Log("Swap postponed: a SimCity 4 process runs on this PC");
				Bump();
			}
			if (S.connected && !S.mirroring) StartMirror(Utf8(S.localFolder));
			return;
		}
		const bool needsLock = S.archiveFolder || std::any_of(S.diff.items.begin(), S.diff.items.end(), [](const lan::DiffItem& i)
		{
			return i.action == lan::DiffAction::Download || i.action == lan::DiffAction::Archive;
		}) || !IsDir(S.regionsDir + L"\\" + S.localFolder);
		if (needsLock)
		{
			// The plugin's name; the game passes its region folder name through the ANSI code page.
			const std::wstring name = L"Local\\SC4RegionSync.SaveLock." + S.localFolder;
			HANDLE m = CreateMutexW(nullptr, FALSE, name.c_str());
			if (!m)
			{
				DropStaging();
				return FailSync("CreateMutexW failed, error " + std::to_string(GetLastError()));
			}
			const DWORD w = WaitForSingleObject(m, 0);
			if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED)
			{
				CloseHandle(m);
				return;
			}
			S.saveMutex = m;
			Log("Save mutex " + Utf8(name) + " opened and held for the swap");
			if (AnyGameProcess())   // fresh check under the mutex
			{
				ReleaseSaveMutex();
				S.gameRunning = true;
				return;
			}
			std::string error;
			const SwapResult result = SwapFiles(error);
			ReleaseSaveMutex();
			if (result != SwapResult::Ok)
			{
				DropStaging();
				if (result == SwapResult::RollbackFailed)
				{
					// State first: handlers run while the box is up.
					S.swapBroken = "the last sync was not fully undone: compare the region with " + Utf8(S.lastArchive);
					FailSync(S.swapBroken);
					ShowMessage(S.main, L"The sync failed and some originals could not be put back. Check the region folder against "
						+ S.lastArchive + L" before playing.\n\n" + Widen(error), MB_ICONERROR);
					return;
				}
				FailSync("swap rolled back: " + error);
				ShowMessage(S.main, L"The sync could not replace the region files, so every original was put back.\n\n" + Widen(error));
				return;
			}
		}
		FinishSync();
	}

	void OnDownloaded(LanEvent& e)
	{
		S.stagingDir = e.stagingDir;
		S.staged = std::move(e.staged);
		S.sync = Sync::SwapPending;
		Log("Downloads verified: " + std::to_string(S.staged.size()) + " files staged");
		Bump();
		TrySwap();
	}

	void SelectRegion()
	{
		std::string match;
		for (const std::string& r : S.peer.regions) if (IsSyncableRegion(r) && lan::EqualsNoCase(r, S.region)) match = r;
		if (match.empty())
		{
			S.failed = S.region.empty() ? "no region selected; use Join" : "region " + S.region + " is not on " + S.peerSite + "; use Join";
			Bump();
			return;
		}
		StopMirror();   // the host stops on SELECT; INDEX and PUTs start over after the sync
		S.sync = Sync::Hashing;
		S.failed.clear();
		Post(LanCmd::Kind::Select, match);
		Bump();
	}

	// ---- Events ----

	void OnSession(LanEvent& e)
	{
		S.sessionSeq++;
		S.connected = true;
		S.peer = e.hello;
		S.peerAddrText = e.address;
		S.disconnectedAt = 0;
		S.refused.clear();
		S.workerStatus.clear();
		if (e.flag)
		{
			S.peerSite = e.hello.site;
			S.peerSiteId = e.hello.siteId;
			S.key = e.key;
			crypto_wipe(e.key.data(), e.key.size());
			S.hasKey = true;
			if (S.role == LanRole::Join || S.joinOpen)
			{
				uint32_t addr = 0;
				if (ParseIpv4(e.address, addr)) S.peerAddress = addr;
			}
			SavePeer();
			if (S.role == LanRole::Host) ClearCode();
		}
		S.pluginsDiffer = e.hello.pluginsHash != S.pluginsHash;
		if (S.pluginsDiffer) Log("Plugins differ from " + S.peerSite + " (banner only)");
		if (S.role == LanRole::Join && !S.joinOpen)
		{
			// Staged files still wait for the game to close: keep them and just mirror.
			if (S.sync == Sync::SwapPending) StartMirror(Utf8(S.localFolder));
			else SelectRegion();
		}
		Bump();
	}

	void OnClosed(LanEvent& e)
	{
		const bool wasConnected = S.connected;
		S.connected = false;
		if (!e.reason.empty()) S.refused = e.text;
		if (e.reason == "ports") S.role = LanRole::None;   // the worker gave up; launcher.ini keeps Role for the next start
		if (wasConnected && !S.disconnectedAt) S.disconnectedAt = GetTickCount64();
		StopMirror();
		if (S.sync == Sync::Hashing || S.sync == Sync::Downloading) S.sync = Sync::Idle;
		Log(e.text);
		Bump();
	}

	void OnFrame(LanEvent& e)
	{
		switch (e.type)
		{
		case lan::FrameType::Synced:
		{
			S.syncedAt = Clock();
			const auto field = [&](const char* key, bool region)
			{
				const std::string v = lan::Get(e.fields, key);
				uint64_t n = 0;
				return (region ? lan::IsValidRegionName(v) : lan::ParseU64(v, n)) ? v : std::string("unknown");
			};
			Log("Guest synced " + field("region", true) + ": downloaded " + field("downloaded", false) + ", not sent " + field("notSent", false));
		}
			if (S.role == LanRole::Host) StartMirror(S.region);
			break;
		case lan::FrameType::Index:
			if (S.role == LanRole::Host && !S.mirroring && !S.region.empty()) StartMirror(S.region);
			OnIndex(e.fields);
			break;
		case lan::FrameType::Put:
			OnPut(e.fields, e.data);
			break;
		case lan::FrameType::Delete:
			OnDelete(e.fields);
			break;
		default:
			break;
		}
		Bump();
	}

	void OnSelectAsked(const std::string& region)
	{
		S.region = region;
		IniWrite(L"Region", region);
		LanCmd c;
		c.kind = LanCmd::Kind::Stage;
		c.text = region;
		for (const auto& [t, counter] : ReadSaves(region))
		{
			if (lan::IsValidRegionFileName(counter.file)) c.tiles[counter.file] = t;
		}
		StopMirror();
		S.worker.Post(std::move(c));
		Log("Guest selected " + region + "; staging");
		Bump();
	}

	void Handle(LanEvent& e)
	{
		if (S.joinOpen) JoinDialogEvent(e);
		switch (e.kind)
		{
		case LanEvent::Kind::Log:
			return Log(e.text);
		case LanEvent::Kind::Found:
			return;
		case LanEvent::Kind::Status:
			S.workerStatus = e.text;
			return Bump();
		case LanEvent::Kind::Session:
			return OnSession(e);
		case LanEvent::Kind::Closed:
			return OnClosed(e);
		case LanEvent::Kind::Refused:
			S.refused = e.text;
			if (e.reason == "unpaired" && e.text.find("pairing lost") != std::string::npos) Log("Pairing lost: Join again");
			Log("Refused: " + e.text);
			return Bump();
		case LanEvent::Kind::PairAttempt:
			// Never regenerated here: only New code or a fresh Host issues one.
			S.lastBurned = e.address;
			S.code.clear();
			S.codeBurned = true;
			Log("Pairing attempt from " + e.address + " failed; code burned");
			return Bump();
		case LanEvent::Kind::SelectAsked:
			return OnSelectAsked(e.text);
		case LanEvent::Kind::Manifest:
			return OnManifest(e);
		case LanEvent::Kind::Progress:
			S.done = e.done;
			S.total = e.total;
			return Bump();
		case LanEvent::Kind::Downloaded:
			return OnDownloaded(e);
		case LanEvent::Kind::SyncFailed:
			return FailSync(e.text);
		case LanEvent::Kind::Frame:
			return OnFrame(e);
		}
	}

	// Only these can prompt; they wait while a dialog is up. Everything else (mirror frames
	// included) is handled at once, so the queue cannot grow behind a forgotten dialog.
	bool Deferrable(const LanEvent& e)
	{
		return e.kind == LanEvent::Kind::Manifest || e.kind == LanEvent::Kind::Downloaded || e.kind == LanEvent::Kind::SyncFailed;
	}

	void HandleSafely(LanEvent& e)
	{
		try
		{
			Handle(e);
		}
		catch (...)
		{
			Log("LAN event handler failed");
		}
	}

	void DrainAll()
	{
		if (!S.workerOk) return;
		std::deque<LanEvent> batch = S.worker.Drain();
		// Coalesce PUTs: only the latest bytes per kind and name matter.
		std::set<std::string> seen;
		std::vector<bool> skip(batch.size(), false);
		for (size_t i = batch.size(); i-- > 0;)
		{
			const LanEvent& e = batch[i];
			if (e.kind != LanEvent::Kind::Frame || e.type != lan::FrameType::Put) continue;
			skip[i] = !seen.insert(lan::Get(e.fields, "kind") + "/" + lan::Get(e.fields, "name")).second;
		}
		for (size_t i = 0; i < batch.size(); i++)
		{
			if (skip[i]) continue;
			if (Blocked() && Deferrable(batch[i])) S.backlog.push_back(std::move(batch[i]));
			else if (!S.backlog.empty() && Deferrable(batch[i])) S.backlog.push_back(std::move(batch[i]));   // keep their order
			else HandleSafely(batch[i]);
		}
		while (!S.backlog.empty() && !Blocked())
		{
			LanEvent e = std::move(S.backlog.front());
			S.backlog.pop_front();
			HandleSafely(e);
		}
	}

	void Stop(bool clearRole)
	{
		Post(LanCmd::Kind::Stop);
		if (S.connected && !S.disconnectedAt) S.disconnectedAt = GetTickCount64();
		S.connected = false;
		StopMirror();
		S.code.clear();
		// A pending swap from a peer we left must never run.
		S.sync = Sync::Idle;
		DropStaging();
		S.backlog.clear();
		if (clearRole)
		{
			S.role = LanRole::None;
			IniWrite(L"Role", std::wstring());
		}
		Bump();
	}

	bool StartRole(LanRole role, bool interactive)
	{
		if (!Prepare())
		{
			Log("Setup refused: " + S.setupError);
			if (interactive) ShowMessage(S.main, Widen(S.setupError));
			Bump();
			return false;
		}
		Log(std::string(role == LanRole::Host ? "Host" : "Join") + " started, build " + S.build + ", site " + S.ownHost);
		S.role = role;
		IniWrite(L"Role", RoleName(role));
		S.refused.clear();
		StartWorker(role);
		if (role == LanRole::Host)
		{
			S.publicNetwork = PublicNetwork();
			if (S.publicNetwork) Log("Network profile is Public: Windows Firewall blocks the guest");
			if (!S.hasKey) NewCodeNow();
		}
		Bump();
		return true;
	}
}

namespace launcher
{
	ModalScope::ModalScope() { S.modalDepth++; }

	ModalScope::~ModalScope()
	{
		if (--S.modalDepth == 0 && S.closeDeferred && S.main)
		{
			S.closeDeferred = false;
			PostMessageW(S.main, WM_CLOSE, 0, 0);
		}
	}

	bool InModal() { return S.modalDepth > 0; }

	void DeferClose() { S.closeDeferred = true; }

	void LanController::Init(HWND main, const Settings& settings)
	{
		S.main = main;
		S.settings = settings;
		ReadIni();
		if (!S.hostAlias.empty()) regionsync::writer::SetHostAlias(S.hostAlias);
		S.ownHost = regionsync::writer::Current().host;
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		S.workerOk = S.worker.Start(main);
		// Staging left by an earlier run belongs to nobody now.
		DWORD volume = 0;
		std::wstring regions;
		if (ResolveDir(S.regionsSetting, regions, volume))
		{
			S.regionsDir = regions;
			S.lanDir = Parent(regions) + L"\\RegionSyncLAN";
			DeleteTree(S.lanDir + L"\\staging");
		}
		if (!S.peerSite.empty()) S.disconnectedAt = GetTickCount64();   // peer files from an earlier run
		const LanRole resume = S.role;
		S.role = LanRole::None;
		if (resume != LanRole::None && S.workerOk && !(resume == LanRole::Join && !S.hasKey)) StartRole(resume, false);
		SetTimer(main, kLanTimer, 250, nullptr);
	}

	void LanController::SettingsChanged(const Settings& settings)
	{
		S.settings = settings;
	}

	bool LanController::Shutdown()
	{
		KillTimer(S.main, kLanTimer);
		ReleaseSaveMutex();
		crypto_wipe(S.key.data(), S.key.size());
		StopMirror();
		Log("Launcher closing");
		return S.worker.Stop(3000);
	}

	void LanController::OnWorkerMessage()
	{
		DrainAll();   // events that can prompt wait in the backlog while a dialog is up
	}

	void LanController::OnTimer()
	{
		DrainAll();
		if (Blocked()) return;
		const ULONGLONG now = GetTickCount64();
		S.gameRunning = AnyGameProcess();   // once per tick; CanResync and TrySwap read it
		if (!S.code.empty() && now > S.codeExpires)
		{
			ClearCode();
			S.codeExpired = true;
			Log("Pairing code expired");
		}
		if (S.disconnectedAt && !S.connected && now - S.disconnectedAt > kPeerKeepMs)
		{
			// A busy file keeps the clock expired; retried every 5 s.
			S.disconnectedAt = ReapPeer() ? 0 : now - kPeerKeepMs + 5000;
		}
		if (S.sync == Sync::SwapPending) TrySwap();
		const bool savesChanged = Signalled(S.chSaves);
		if (S.mirroring)
		{
			const bool storeChanged = Signalled(S.chStore);
			const bool statusChanged = Signalled(S.chStatus);
			const bool changed = storeChanged || statusChanged;
			if (changed || now - S.lastScan > kRescanMs)
			{
				S.lastScan = now;
				OpenWatches();
				if (!S.sizeKnown) ReadRegionSize(S.regionsDir + L"\\" + Widen(S.mirrorRegion));   // absent until the first swap
				Scan(false);
			}
			if (now - S.lastRecount > kRescanMs)
			{
				S.lastRecount = now;
				S.peerSnapshots = PeerSnapshotCount();   // deletes and reaps are not tracked one by one
			}
			for (auto it = S.pending.begin(); it != S.pending.end();)
			{
				it = ApplyPut(it->second) ? S.pending.erase(it) : std::next(it);
			}
			if (S.pending.empty() && !S.openBoth.empty())
			{
				S.openBoth.clear();
				Bump();
			}
		}
		if (savesChanged || now - S.lastNotSent > 10000)
		{
			S.lastNotSent = now;
			RefreshNotSent();
		}
	}

	void LanController::ToggleHost()
	{
		if (S.role == LanRole::Host)
		{
			Log("Host stopped");
			return Stop(true);
		}
		if (S.role == LanRole::Join)
		{
			ShowMessage(S.main, L"This PC is joined to " + Widen(S.peerSite) + L". Use Join, then Leave, before hosting.");
			return;
		}
		StartRole(LanRole::Host, true);
	}

	void LanController::NewCode()
	{
		if (S.role != LanRole::Host) return;
		Log("New code requested");
		NewCodeNow();
	}

	void LanController::ShowJoin()
	{
		if (S.role == LanRole::Host)
		{
			ShowMessage(S.main, L"This PC is hosting. Uncheck Host before joining another PC.");
			return;
		}
		const LanRole before = S.role;
		if (before != LanRole::Join && !StartRole(LanRole::Join, true)) return;
		JoinHooks hooks;
		hooks.discover = [](bool on)
		{
			LanCmd c;
			c.kind = LanCmd::Kind::Discover;
			c.on = on;
			S.worker.Post(std::move(c));
		};
		hooks.probe = [](const std::string& address) { Post(LanCmd::Kind::Probe, address); };
		hooks.connect = [](const std::string& address, const std::string& code)
		{
			LanCmd c;
			c.kind = LanCmd::Kind::Connect;
			c.text = address;
			c.frameText = code;
			S.worker.Post(std::move(c));
		};
		hooks.pairedSiteId = S.peerSiteId;
		hooks.lastRegion = S.region;
		hooks.joined = before == LanRole::Join;
		if (S.connected) hooks.regions = S.peer.regions;
		JoinResult r;
		int result = 0;
		{
			ModalScope modal;
			S.joinOpen = true;
			result = RunJoinDialog(S.main, hooks, r);
			S.joinOpen = false;
		}
		hooks.discover(false);
		if (result == 2)   // Leave
		{
			Log("Left " + S.peerSite);
			Post(LanCmd::Kind::Close, "left");
			return Stop(true);
		}
		if (result != 1)
		{
			if (before != LanRole::Join) Stop(true);
			// A session that came up while the dialog was open still needs its sync.
			else if (S.connected && (S.sync == Sync::Idle || S.sync == Sync::Failed)) SelectRegion();
			return;
		}
		S.region = r.region;
		IniWrite(L"Region", r.region);
		IniWrite(L"Role", L"Join");
		S.role = LanRole::Join;
		if (S.connected) SelectRegion();
		Bump();
	}

	void LanController::Resync()
	{
		if (!CanResync()) return;
		const uint64_t session = S.sessionSeq;
		std::wstring tiles;
		for (const Tile& t : S.notSent) tiles += L" (" + std::to_wstring(t.x) + L"," + std::to_wstring(t.z) + L")";
		if (!ConfirmYesNo(S.main, L"Re-sync replaces this PC's copy of " + Widen(S.region) + L" with the host's."
			+ (tiles.empty() ? std::wstring() : L"\n\nTiles not sent, which go to RegionSyncLAN\\archive:" + tiles) + L"\n\nContinue?"))
		{
			return;
		}
		S.gameRunning = AnyGameProcess();
		if (S.sessionSeq != session || !CanResync()) return;   // handlers ran during the prompt
		Log("Re-sync requested");
		S.resync = true;   // FinishSync writes a fresh journal
		SelectRegion();
	}

	LanRole LanController::Role() const { return S.role; }
	bool LanController::Connected() const { return S.connected; }
	const std::string& LanController::OwnHost() const { return S.ownHost; }
	uint64_t LanController::StatusVersion() const { return S.version; }
	bool LanController::IsOwnHost(const std::string& host) const { return SameHost(host, S.ownHost); }

	bool LanController::LaunchAllowed() const
	{
		return S.role != LanRole::Join || (S.guestSynced && S.sync != Sync::SwapPending && S.sync != Sync::Downloading
			&& S.sync != Sync::Hashing);
	}

	bool LanController::CanResync() const
	{
		return S.role == LanRole::Join && S.connected && S.sync != Sync::Downloading && S.sync != Sync::Hashing && !S.gameRunning;
	}

	LanStatusPart LanController::Status() const
	{
		LanStatusPart p;
		const std::wstring peer = Widen(S.peerSite);
		if (S.role == LanRole::Host)
		{
			if (!S.code.empty())
			{
				p.strip = L"Hosting, code " + Widen(S.code.substr(0, 3)) + L" " + Widen(S.code.substr(3)) + L" (New code)";
			}
			else if (S.connected)
			{
				p.strip = L"Hosting, paired with " + peer + (S.syncedAt.empty() ? L"" : L", synced " + Widen(S.syncedAt))
					+ (S.lastMirrorAt.empty() ? L"" : L", last mirror " + Widen(S.lastMirrorAt));
			}
			else
			{
				p.strip = S.codeBurned ? L"Hosting, code burned (New code)" : S.codeExpired ? L"Hosting, code expired (New code)"
					: S.hasKey ? L"Hosting, waiting for " + peer : L"Hosting";
			}
			if (!S.lastBurned.empty()) p.strip += L", last attempt from " + Widen(S.lastBurned);
		}
		else if (S.role == LanRole::Join)
		{
			const std::wstring name = peer.empty() ? L"host" : peer;
			if (!S.connected) p.strip = name + L": " + (S.workerStatus.empty() ? L"not connected" : Widen(S.workerStatus));
			else if (S.sync == Sync::Hashing) p.strip = name + L": checking local files";
			else if (S.sync == Sync::Downloading) p.strip = name + L": syncing " + std::to_wstring(S.done) + L"/" + std::to_wstring(S.total);
			else if (S.sync == Sync::SwapPending) p.strip = name + L": close SimCity 4 on this PC to finish the sync";
			else if (S.sync == Sync::Failed) p.strip = name + L": sync failed";
			else p.strip = name + L": synced";
			if (!S.notSent.empty()) p.strip += L", " + std::to_wstring(S.notSent.size()) + (S.notSent.size() == 1 ? L" tile not sent" : L" tiles not sent");
			std::wstring tip = L"Not sent:";
			for (const Tile& t : S.notSent) tip += L" (" + std::to_wstring(t.x) + L"," + std::to_wstring(t.z) + L")";
			p.stripTip = (S.notSent.empty() ? L"All tiles sent." : tip) + L"\r\nPeer build: " + (S.peer.build.empty() ? L"unknown" : Widen(S.peer.build))
				+ L"\r\nThis build: " + Widen(S.build);
		}
		if (S.role == LanRole::Host) p.stripTip = L"Peer build: " + (S.peer.build.empty() ? L"unknown" : Widen(S.peer.build)) + L"\r\nThis build: " + Widen(S.build);

		std::vector<std::wstring> banners;
		if (!S.swapBroken.empty()) banners.push_back(Widen(S.swapBroken));
		if (!S.setupError.empty()) banners.push_back(Widen(S.setupError));
		if (!S.refused.empty() && !S.connected) banners.push_back(Widen(S.refused));
		if (!S.failed.empty()) banners.push_back(Widen(S.failed));
		if (S.publicNetwork && S.role == LanRole::Host) banners.push_back(L"This PC's network is Public: set it to Private so the other PC can connect.");
		if (S.pluginsDiffer && S.connected) banners.push_back(L"Plugins differ from " + peer + L".");
		if (S.sync == Sync::SwapPending && S.gameBlocksSwap) banners.push_back(L"Sync pending: close SimCity 4 on this PC to finish it.");
		for (const Tile& t : S.openBoth)
		{
			banners.push_back(L"Tile (" + std::to_wstring(t.x) + L"," + std::to_wstring(t.z) + L") is open on both PCs.");
		}
		if (!S.notSent.empty() && S.role == LanRole::Join)
		{
			banners.push_back(std::to_wstring(S.notSent.size()) + L" tile(s) saved here are not sent to " + peer + L" (slice 1).");
		}
		if (!S.aliasWarning.empty()) banners.push_back(Widen(S.aliasWarning));
		if (!banners.empty())
		{
			p.banner = banners.front();
			for (const std::wstring& b : banners) p.bannerTip += (p.bannerTip.empty() ? L"" : L"\r\n") + b;
		}
		return p;
	}
}
