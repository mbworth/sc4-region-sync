/*
 * sc4-region-sync launcher
 *
 * Lan.h - two PCs on one region, slice 1 (docs/PLAN-LAN.md). The worker thread
 * (Net.cpp) owns sockets, hashing and staging copies; the UI thread (Lan.cpp,
 * LanUi.cpp) owns Regions, the store, launcher.ini and every dialog. They talk
 * only through the two queues below.
 */

#pragma once
#include "Frame.h"
#include "Handshake.h"
#include "Launcher.h"
#include "Manifest.h"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>

namespace launcher
{
	inline constexpr UINT WM_APP_LAN = WM_APP + 1;
	inline constexpr UINT_PTR kLanTimer = 3;
	inline constexpr int kDefaultLanPort = 47641;

	enum class LanRole { None, Host, Join };

	// Everything the worker needs, copied in at Start.
	struct LanConfig
	{
		LanRole role = LanRole::None;
		int port = kDefaultLanPort;
		bool allowLoopback = false;
		std::string site;          // our host (HostAlias or computer name)
		std::string siteId;
		std::string build;
		std::string pluginsHash;
		std::wstring regionsDir;   // resolved
		std::wstring lanDir;       // <Regions parent>\RegionSyncLAN
		// Peer record; empty when unpaired.
		std::string peerSite, peerSiteId;
		bool hasKey = false;
		lan::Bytes32 peerKey{};
		uint32_t peerAddress = 0;  // host byte order; guest reconnect target
	};

	struct LanCmd
	{
		enum class Kind
		{
			Start, Stop, SetCode, Discover, Probe, Connect, Select, Stage, Download, Send, Close,
		};
		Kind kind = Kind::Stop;
		LanConfig config;                 // Start
		std::string text;                 // SetCode: code; Probe/Connect: address; Select/Stage: region
		bool on = false;                  // Discover
		std::map<std::string, lan::Tile> tiles;   // Stage: file -> tile from the host's _saves
		std::vector<std::string> names;   // Download
		lan::FrameType type = lan::FrameType::Ping;   // Send
		std::string frameText;
		std::vector<uint8_t> data;
	};

	struct LanEvent
	{
		enum class Kind
		{
			Log,          // text
			Found,        // site, siteId, build, address
			Status,       // text for the strip (connecting, reconnecting in N s)
			Session,      // hello (peer), paired (new K in key), address
			Closed,       // reason, text; stop: no reconnect follows
			Refused,      // reason, text (handshake refused or wrong code)
			PairAttempt,  // host: address, ok
			SelectAsked,  // host: region (UI answers with Stage)
			Manifest,     // guest: manifest, localHashes
			Progress,     // done, total
			Downloaded,   // guest: stagingDir, staged (name -> path)
			SyncFailed,   // text
			Frame,        // type, fields, data (SYNCED, INDEX, PUT, DELETE)
		};
		Kind kind = Kind::Log;
		std::string text, reason, address;
		bool flag = false;                // Closed: stop; PairAttempt: ok; Session: paired now
		lan::Hello hello;                 // Found (site, siteId, build) and Session
		lan::Bytes32 key{};
		lan::Manifest manifest;
		std::map<std::string, std::string> localHashes;
		std::wstring stagingDir;
		std::map<std::string, std::wstring> staged;
		size_t done = 0, total = 0;
		lan::FrameType type = lan::FrameType::Ping;
		lan::KeyValues fields;
		std::vector<uint8_t> data;
	};

	// Worker thread and its two queues (Net.cpp).
	class LanWorker
	{
	public:
		bool Start(HWND notify);   // creates the thread; idle until a Start command
		void Post(LanCmd cmd);     // UI thread
		std::deque<LanEvent> Drain();   // UI thread, everything queued
		// Sets stop and waits up to ms. False when the thread is still running.
		bool Stop(DWORD ms);

		// Worker side.
		void Emit(LanEvent e);
		bool TakeCommands(std::deque<LanCmd>& out);
		HANDLE WakeEvent() const { return wake_; }
		HANDLE StopEvent() const { return stop_; }
		bool Stopping() const { return WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0; }
		size_t QueuedBytes() const { return queuedBytes_; }   // events not yet drained by the UI

	private:
		static DWORD WINAPI ThreadProc(void* self);
		HWND notify_ = nullptr;
		HANDLE thread_ = nullptr, wake_ = nullptr, stop_ = nullptr;
		std::mutex inLock_, outLock_;
		std::deque<LanCmd> in_;
		std::deque<LanEvent> out_;
		std::atomic<size_t> queuedBytes_{ 0 };
	};

	// LanCrypto.cpp
	bool RandomBytes(void* out, size_t size);
	bool ProtectKey(const lan::Bytes32& key, std::wstring& base64);
	bool UnprotectKey(const std::wstring& base64, lan::Bytes32& key);

	// Region names the launcher syncs: a leading '_' would collide with the store's _saves and _instances.
	inline bool IsSyncableRegion(std::string_view s) { return !s.empty() && s[0] != '_' && lan::IsValidRegionName(s); }

	// Net.cpp helpers shared with Lan.cpp.
	std::vector<std::pair<std::wstring, bool>> ListDir(const std::wstring& dir);   // (name, is directory)
	void DeleteTree(const std::wstring& dir);
	std::string LowerAscii(std::string s);
	bool IsPrivateAddress(uint32_t hostOrder, bool allowLoopback);
	bool ParseIpv4(const std::string& text, uint32_t& hostOrder);
	std::string FormatIpv4(uint32_t hostOrder);
	std::string Utf8(const std::wstring& wide);

	struct LanStatusPart
	{
		std::wstring strip, stripTip, banner, bannerTip;
	};

	// UI-thread controller (Lan.cpp). One instance, owned by main.cpp.
	class LanController
	{
	public:
		// Reads [Lan], applies HostAlias, starts the worker and resumes Role.
		void Init(HWND main, const Settings& settings);
		void SettingsChanged(const Settings& settings);
		// Stop flag, signal, join up to 3 s. False when the worker did not stop.
		bool Shutdown();

		void OnTimer();       // 250 ms
		void OnWorkerMessage();

		void ToggleHost();
		void ShowJoin();
		void Resync();
		void NewCode();

		LanRole Role() const;
		bool Connected() const;
		bool LaunchAllowed() const;
		bool CanResync() const;
		const std::string& OwnHost() const;
		LanStatusPart Status() const;
		// Bumped whenever Status() may have changed.
		uint64_t StatusVersion() const;
		bool IsOwnHost(const std::string& host) const;
	};

	// LanUi.cpp
	struct JoinResult
	{
		std::string region;
	};
	struct JoinHooks
	{
		std::function<void(bool)> discover;
		std::function<void(const std::string&)> probe;
		std::function<void(const std::string& address, const std::string& code)> connect;
		std::string pairedSiteId;   // marks the paired host in the list
		std::string lastRegion;
		std::vector<std::string> regions;   // when already connected
		bool joined = false;                // enables Leave
	};
	// 1 OK (out filled), 2 Leave, 0 Cancel.
	int RunJoinDialog(HWND owner, JoinHooks& hooks, JoinResult& out);
	// Forwards worker events to an open Join dialog; false when none is open.
	bool JoinDialogEvent(const LanEvent& e);
	bool ConfirmYesNo(HWND owner, const std::wstring& text);

	// Message boxes and dialogs. While one is up, LAN handlers return early
	// and WM_CLOSE is deferred until the last one closes.
	struct ModalScope
	{
		ModalScope();
		~ModalScope();
		ModalScope(const ModalScope&) = delete;
		ModalScope& operator=(const ModalScope&) = delete;
	};
	bool InModal();
	void DeferClose();
}
