/*
 * sc4-region-sync launcher
 *
 * Net.cpp - the LAN worker thread (docs/PLAN-LAN.md sections 2 to 4): discovery,
 * TCP, handshake, frames, host staging and serving, guest hashing and downloads.
 * Blocks only in WSAWaitForMultipleEvents. Long jobs run one chunk per loop pass
 * so keepalives keep flowing. Never writes to Regions, the store or launcher.ini.
 */

#include <WinSock2.h>
#include <WS2tcpip.h>
#include "Lan.h"
#include "monocypher.h"
#include <algorithm>
#include <atomic>
#include <memory>

namespace
{
	using namespace lan;
	using launcher::LanCmd;
	using launcher::LanConfig;
	using launcher::LanEvent;
	using launcher::LanRole;
	using launcher::RandomBytes;
	using regionsync::SameHost;

	constexpr ULONGLONG kConnectTimeoutMs = 5000;
	constexpr ULONGLONG kHandshakeTimeoutMs = 10000;
	constexpr ULONGLONG kPingMs = 5000;
	constexpr ULONGLONG kDeadMs = 20000;
	constexpr ULONGLONG kPairIntervalMs = 2000;
	constexpr ULONGLONG kDiscoverMs = 1000;
	constexpr size_t kMaxHalfOpen = 4;
	constexpr size_t kChunk = 1024 * 1024;
	constexpr size_t kFileChunk = 256 * 1024;
	constexpr size_t kOutHighWater = 1024 * 1024;
	constexpr int kSelectLimit = 4;   // one plus 3 retries
	constexpr int kStageAttempts = 5;
	constexpr char kMagic[] = "SC4RS";
	constexpr ULONGLONG kBackoffMs[] = { 1000, 2000, 4000, 8000, 16000, 30000 };
	constexpr size_t kMaxQueuedEventBytes = 32u << 20;   // pause reading past this (UI behind)
	constexpr ULONGLONG kResolveTimeoutMs = 5000;

	using launcher::DeleteTree;
	using launcher::ListDir;
	using launcher::LowerAscii;
	using launcher::ParseIpv4;
	using launcher::Widen;

	// Empty when the system RNG fails; callers refuse to go on.
	std::string RandomHex(size_t bytes)
	{
		std::vector<uint8_t> b(bytes);
		return RandomBytes(b.data(), b.size()) ? ToHex(b) : std::string();
	}

	struct Hasher
	{
		crypto_blake2b_ctx ctx;
		Hasher() { crypto_blake2b_init(&ctx, 32); }
		void Update(const void* p, size_t n) { crypto_blake2b_update(&ctx, static_cast<const uint8_t*>(p), n); }
		std::string Hex()
		{
			Bytes32 h;
			crypto_blake2b_final(&ctx, h.data());
			return ToHex(h);
		}
	};

	struct FileId
	{
		DWORD volume = 0, indexHigh = 0, indexLow = 0;
		uint64_t size = 0, mtime = 0;
		bool operator==(const FileId&) const = default;
	};

	bool GetFileId(HANDLE h, FileId& id)
	{
		BY_HANDLE_FILE_INFORMATION info{};
		if (!GetFileInformationByHandle(h, &info)) return false;
		id.volume = info.dwVolumeSerialNumber;
		id.indexHigh = info.nFileIndexHigh;
		id.indexLow = info.nFileIndexLow;
		id.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
		id.mtime = (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
		return true;
	}

	struct Handle
	{
		HANDLE h = INVALID_HANDLE_VALUE;
		Handle() = default;
		explicit Handle(HANDLE v) : h(v) {}
		Handle(const Handle&) = delete;
		Handle& operator=(const Handle&) = delete;
		~Handle() { Reset(); }
		void Reset(HANDLE v = INVALID_HANDLE_VALUE) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = v; }
		bool Ok() const { return h != INVALID_HANDLE_VALUE; }
	};

	struct Conn
	{
		enum class Phase { Connecting, Hello, Pake, Confirm, Session, Dead };
		SOCKET s = INVALID_SOCKET;
		WSAEVENT ev = WSA_INVALID_EVENT;
		uint32_t addr = 0;
		Phase phase = Phase::Hello;
		ULONGLONG started = 0, lastRecv = 0, lastSend = 0, closeAt = 0;
		FrameDecoder dec;
		FrameEncoder enc;
		std::vector<uint8_t> out;
		size_t outPos = 0;
		bool closing = false;      // close once out is flushed
		bool readPending = false;  // FD_READ seen while reading was paused
		bool pairing = false;
		Hello peer;
		std::vector<uint8_t> gRaw, hRaw, transcript;
		std::string nonceG, nonceH;
		Bytes32 scalar{}, yH{}, k{};
		int selects = 0;
		Conn(char ownDir, char peerDir) : dec(peerDir), enc(ownDir) {}
		~Conn() { crypto_wipe(scalar.data(), scalar.size()); crypto_wipe(k.data(), k.size()); }
		std::string Addr() const { return launcher::FormatIpv4(addr); }
	};

	struct StagedFile
	{
		FileId id;
		std::wstring path;
		std::string hash;
	};

	// Host: copy Regions\<region> into staging, hash, then MANIFEST.
	struct StageJob
	{
		std::string region;
		std::wstring src, dir;
		std::map<std::string, Tile> tiles;   // lower-case name -> tile
		std::vector<std::pair<std::wstring, std::string>> files;   // wide, UTF-8
		size_t index = 0;
		int attempt = 0;
		Handle in, out;
		FileId before;
		std::unique_ptr<Hasher> hasher;
		std::wstring outPath;
		Manifest manifest;
	};

	// Guest: hash the local region folder.
	struct HashJob
	{
		std::wstring dir;
		std::vector<std::pair<std::wstring, std::string>> files;
		size_t index = 0;
		Handle in;
		std::unique_ptr<Hasher> hasher;
		std::map<std::string, std::string> hashes;
		Manifest manifest;
	};

	// Host: stream FILE frames for queued GETs.
	struct ServeJob
	{
		std::deque<std::string> names;
		Handle in;
		std::string current;
		uint64_t offset = 0, size = 0;
		void Reset() { names.clear(); in.Reset(); current.clear(); offset = size = 0; }
	};

	// Guest: one download in progress per name.
	struct Download
	{
		const ManifestEntry* entry = nullptr;
		std::wstring path;
		Handle out;
		uint64_t received = 0;
		std::unique_ptr<Hasher> hasher;
		bool done = false;
	};

	class Net
	{
	public:
		explicit Net(launcher::LanWorker& q) : q_(q) {}
		void Run();

	private:
		launcher::LanWorker& q_;
		LanConfig cfg_;
		SOCKET udp_ = INVALID_SOCKET, listen_ = INVALID_SOCKET;
		WSAEVENT udpEv_ = WSA_INVALID_EVENT, listenEv_ = WSA_INVALID_EVENT;
		std::vector<std::unique_ptr<Conn>> conns_;
		// Host.
		std::string code_;
		ULONGLONG lastPairTick_ = 0;
		std::unique_ptr<StageJob> stage_;
		std::map<std::string, StagedFile> stageCache_;   // lower(region/name)
		std::map<std::string, std::wstring> served_;     // lower(name) -> staged path
		Manifest servedManifest_;
		std::wstring hostSessionDir_;
		size_t partSeq_ = 0;   // staged names stay unique across sessions (reused files keep theirs)
		// Name resolution for Connect: GetAddrInfoExW, overlapped, 5 s timeout.
		struct Resolving
		{
			OVERLAPPED ov{};
			PADDRINFOEXW result = nullptr;
			HANDLE cancel = nullptr;
			std::wstring name;
			std::string text, code;
			bool active = false;
		} resolve_;
		ServeJob serve_;
		// Guest.
		bool discover_ = false;
		ULONGLONG lastDiscover_ = 0;
		std::string pairCode_;
		uint32_t target_ = 0;
		ULONGLONG reconnectAt_ = 0;
		size_t backoff_ = 0;
		ULONGLONG sessionSince_ = 0;
		int refusals_ = 0;
		bool reconnect_ = false;
		std::string selected_;
		std::unique_ptr<HashJob> hash_;
		Manifest manifest_;
		std::map<std::string, Download> downloads_;   // lower(name)
		std::deque<std::string> getQueue_;
		std::wstring guestSessionDir_;
		size_t downloadTotal_ = 0;

		void Emit(LanEvent::Kind kind, std::string text = {}, std::string reason = {})
		{
			LanEvent e;
			e.kind = kind;
			e.text = std::move(text);
			e.reason = std::move(reason);
			q_.Emit(std::move(e));
		}
		void Log(const std::string& text) { Emit(LanEvent::Kind::Log, text); }

		bool IsHost() const { return cfg_.role == LanRole::Host; }
		Conn* Session()
		{
			for (auto& c : conns_) if (c->phase == Conn::Phase::Session) return c.get();
			return nullptr;
		}

		void Reset();
		void Start(const LanConfig& cfg);
		void Command(LanCmd& cmd);
		bool OpenSockets();
		void CloseSocket(SOCKET& s, WSAEVENT& ev);
		void Drop(Conn& c, const std::string& why, bool log = true);
		void CloseConn(Conn& c);
		void Sweep();
		void Tick();
		bool HasJob() const;
		void StepJobs();

		void OnUdp();
		void OnAccept();
		void OnConn(Conn& c, const WSANETWORKEVENTS& ne);
		void Flush(Conn& c);
		void Queue(Conn& c, const std::vector<uint8_t>& bytes) { c.out.insert(c.out.end(), bytes.begin(), bytes.end()); }
		bool SendFrame(Conn& c, FrameType type, const std::string& text, std::span<const uint8_t> data = {});
		void SendHandshake(Conn& c, FrameType type, const std::string& text, char dir, const Bytes32* key,
			std::span<const uint8_t> context, std::vector<uint8_t>* raw = nullptr);
		void SendDiscover(uint32_t addr);
		void BeginResolve(const std::string& text, const std::string& code);
		void EndResolve(INT syncResult = WSA_IO_PENDING);
		void CancelResolve();
		void ConnectTo(uint32_t addr, const std::string& text, const std::string& code);
		void Read(Conn& c);
		bool ReadPaused() const { return q_.QueuedBytes() > kMaxQueuedEventBytes; }

		void Connect(uint32_t addr);
		void ScheduleReconnect();
		void OnFrame(Conn& c, Frame& f);
		void HostHello(Conn& c, Frame& f);
		void HostRefuse(Conn& c, const std::string& reason, const Bytes32* key);
		void HostPake(Conn& c, Frame& f);
		void HostConfirm(Conn& c, Frame& f);
		void GuestHello(Conn& c, Frame& f);
		void GuestConfirm(Conn& c, Frame& f);
		void GuestRefused(Conn& c, const Hello& h, bool authenticated);
		void StartSession(Conn& c, bool pairedNow);
		void SessionFrame(Conn& c, Frame& f);
		Hello OwnHello(const std::string& role, const std::string& nonce) const;
		bool BuildHostHello(const Hello& h, std::string& text);
		std::string RegionFields() const;
		std::vector<std::string> HostRegions() const;

		void StartStage(const std::string& region, const std::map<std::string, Tile>& tiles);
		void StepStage();
		void FailStage(const std::string& why);
		void StepServe();
		void StartHash(const Manifest& m);
		void StepHash();
		void StartDownloads(const std::vector<std::string>& names);
		void OnFile(Conn& c, const KeyValues& kv, const std::vector<uint8_t>& data);
		void AbandonDownloads();
		void ClearHostStaging();
	};

	void Net::CloseSocket(SOCKET& s, WSAEVENT& ev)
	{
		if (s != INVALID_SOCKET) closesocket(s);
		if (ev != WSA_INVALID_EVENT) WSACloseEvent(ev);
		s = INVALID_SOCKET;
		ev = WSA_INVALID_EVENT;
	}

	void Net::Reset()
	{
		for (auto& c : conns_) if (c->phase != Conn::Phase::Dead) CloseConn(*c);
		Sweep();
		CloseSocket(udp_, udpEv_);
		CloseSocket(listen_, listenEv_);
		stage_.reset();
		serve_.Reset();
		hash_.reset();
		AbandonDownloads();
		ClearHostStaging();
		CancelResolve();
		crypto_wipe(code_.data(), code_.size());
		code_.clear();
		crypto_wipe(cfg_.peerKey.data(), cfg_.peerKey.size());
		cfg_.hasKey = false;
		discover_ = false;
		target_ = 0;
		reconnect_ = false;
		refusals_ = 0;
		backoff_ = 0;
	}

	void Net::Start(const LanConfig& cfg)
	{
		Reset();
		cfg_ = cfg;
		if (cfg_.role == LanRole::None) return;
		if (!OpenSockets())
		{
			CloseSocket(udp_, udpEv_);
			CloseSocket(listen_, listenEv_);
			Emit(LanEvent::Kind::Closed, "Could not open the LAN ports (" + std::to_string(cfg_.port) + "). Is another launcher hosting?", "ports");
			cfg_.role = LanRole::None;
			return;
		}
		if (cfg_.role == LanRole::Join && cfg_.hasKey && cfg_.peerAddress)
		{
			target_ = cfg_.peerAddress;
			reconnect_ = true;
			reconnectAt_ = GetTickCount64();
		}
	}

	bool Net::OpenSockets()
	{
		udp_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (udp_ == INVALID_SOCKET) return false;
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		a.sin_port = htons(static_cast<u_short>(IsHost() ? cfg_.port + 1 : 0));
		BOOL yes = TRUE;
		setsockopt(udp_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes));
		setsockopt(udp_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
		if (bind(udp_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
		{
			Log("UDP bind failed, error " + std::to_string(WSAGetLastError()));
			return false;
		}
		udpEv_ = WSACreateEvent();
		WSAEventSelect(udp_, udpEv_, FD_READ);
		if (!IsHost()) return true;

		listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listen_ == INVALID_SOCKET) return false;
		setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes));
		a.sin_port = htons(static_cast<u_short>(cfg_.port));
		if (bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(listen_, 8) != 0)
		{
			Log("TCP listen failed, error " + std::to_string(WSAGetLastError()));
			return false;
		}
		listenEv_ = WSACreateEvent();
		WSAEventSelect(listen_, listenEv_, FD_ACCEPT);
		Log("Hosting: TCP " + std::to_string(cfg_.port) + ", UDP " + std::to_string(cfg_.port + 1) + " on all addresses");
		return true;
	}

	void Net::CloseConn(Conn& c)
	{
		if (c.phase == Conn::Phase::Dead) return;
		const bool wasSession = c.phase == Conn::Phase::Session;
		const bool wasPairing = IsHost() && c.pairing && (c.phase == Conn::Phase::Pake);
		c.phase = Conn::Phase::Dead;
		if (wasPairing)
		{
			LanEvent e;
			e.kind = LanEvent::Kind::PairAttempt;
			e.address = c.Addr();
			e.flag = false;
			q_.Emit(std::move(e));
		}
		if (wasSession)
		{
			if (IsHost())
			{
				serve_.Reset();
				stage_.reset();
			}
			else
			{
				hash_.reset();
				AbandonDownloads();
			}
			Emit(LanEvent::Kind::Closed, "Connection to " + c.Addr() + " closed");
			if (!IsHost()) ScheduleReconnect();
		}
		else if (!IsHost() && reconnect_ && c.addr == target_ && reconnectAt_ == 0)
		{
			ScheduleReconnect();
		}
	}

	void Net::Drop(Conn& c, const std::string& why, bool log)
	{
		if (log) Log("Dropped " + c.Addr() + ": " + why);
		CloseConn(c);
	}

	void Net::Sweep()
	{
		for (auto it = conns_.begin(); it != conns_.end();)
		{
			if ((*it)->phase == Conn::Phase::Dead)
			{
				CloseSocket((*it)->s, (*it)->ev);
				it = conns_.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	void Net::ScheduleReconnect()
	{
		if (!reconnect_ || IsHost() || cfg_.role != LanRole::Join || !cfg_.hasKey || !target_) return;
		if (sessionSince_ && GetTickCount64() - sessionSince_ >= 60000) backoff_ = 0;
		sessionSince_ = 0;
		const ULONGLONG base = kBackoffMs[std::min(backoff_, std::size(kBackoffMs) - 1)];
		uint8_t r = 0;
		launcher::RandomBytes(&r, 1);
		const ULONGLONG delay = base * (80 + (r % 41)) / 100;   // +-20 %
		backoff_++;
		reconnectAt_ = GetTickCount64() + delay;
		Emit(LanEvent::Kind::Status, "reconnecting in " + std::to_string((delay + 500) / 1000) + " s");
	}

	// Dotted IPv4 connects at once; a name resolves without blocking the loop (resolve_.ov.hEvent is waited on).
	void Net::BeginResolve(const std::string& text, const std::string& code)
	{
		CancelResolve();
		uint32_t addr = 0;
		if (ParseIpv4(text, addr)) return ConnectTo(addr, text, code);
		if (text.empty() || text.size() > 253)
		{
			Emit(LanEvent::Kind::Refused, "cannot resolve " + text, "address");
			return;
		}
		resolve_.name = Widen(text);
		resolve_.text = text;
		resolve_.code = code;
		resolve_.ov = OVERLAPPED{};
		resolve_.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		ADDRINFOEXW hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		timeval timeout{ static_cast<long>(kResolveTimeoutMs / 1000), 0 };
		const INT r = GetAddrInfoExW(resolve_.name.c_str(), nullptr, NS_ALL, nullptr, &hints, &resolve_.result, &timeout,
			&resolve_.ov, nullptr, &resolve_.cancel);
		if (r == WSA_IO_PENDING)
		{
			resolve_.active = true;
			Emit(LanEvent::Kind::Status, "resolving " + text);
			return;
		}
		resolve_.active = true;   // completed at once: finish now with its own result
		EndResolve(r);
	}

	void Net::EndResolve(INT syncResult)
	{
		if (!resolve_.active) return;
		const INT r = syncResult == WSA_IO_PENDING ? GetAddrInfoExOverlappedResult(&resolve_.ov) : syncResult;
		uint32_t addr = 0;
		if (r == NO_ERROR && resolve_.result && resolve_.result->ai_addr)
		{
			addr = ntohl(reinterpret_cast<sockaddr_in*>(resolve_.result->ai_addr)->sin_addr.s_addr);
		}
		if (resolve_.result) FreeAddrInfoExW(resolve_.result);
		CloseHandle(resolve_.ov.hEvent);
		const std::string text = resolve_.text, code = resolve_.code;
		crypto_wipe(resolve_.code.data(), resolve_.code.size());
		resolve_ = Resolving{};
		if (!addr)
		{
			Emit(LanEvent::Kind::Refused, "cannot resolve " + text + " (error " + std::to_string(r) + ")", "address");
			return;
		}
		ConnectTo(addr, text, code);
	}

	void Net::CancelResolve()
	{
		if (!resolve_.active) return;
		GetAddrInfoExCancel(&resolve_.cancel);
		WaitForSingleObject(resolve_.ov.hEvent, INFINITE);   // the cancel completes promptly; the OVERLAPPED must outlive the call
		if (resolve_.result) FreeAddrInfoExW(resolve_.result);
		CloseHandle(resolve_.ov.hEvent);
		resolve_ = Resolving{};
	}

	void Net::ConnectTo(uint32_t addr, const std::string& text, const std::string& code)
	{
		if (!launcher::IsPrivateAddress(addr, cfg_.allowLoopback))
		{
			Emit(LanEvent::Kind::Refused, text + " is not a private address", "address");
			return;
		}
		pairCode_ = code;
		target_ = addr;
		reconnect_ = pairCode_.empty() && cfg_.hasKey;
		refusals_ = 0;
		backoff_ = 0;
		for (auto& c : conns_) CloseConn(*c);
		Connect(addr);
	}

	void Net::SendDiscover(uint32_t addr)
	{
		if (udp_ == INVALID_SOCKET) return;
		const std::string text = Fields().Add("magic", kMagic).Add("proto", kProto).Text();
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = htonl(addr);
		to.sin_port = htons(static_cast<u_short>(cfg_.port + 1));
		sendto(udp_, text.data(), static_cast<int>(text.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
	}

	void Net::OnUdp()
	{
		char buf[1024];
		for (;;)
		{
			sockaddr_in from{};
			int fromLen = sizeof(from);
			const int n = recvfrom(udp_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
			if (n < 0) break;
			const uint32_t addr = ntohl(from.sin_addr.s_addr);
			if (!launcher::IsPrivateAddress(addr, cfg_.allowLoopback) || n > 512) continue;
			KeyValues kv;
			if (!ParseFields(std::string_view(buf, static_cast<size_t>(n)), kv) || Get(kv, "magic") != kMagic
				|| Get(kv, "proto") != std::to_string(kProto))
			{
				continue;
			}
			if (IsHost())
			{
				if (!kv.count("site"))   // a DISCOVER, not another host's reply
				{
					const std::string reply = Fields().Add("magic", kMagic).Add("proto", kProto).Add("site", cfg_.site)
						.Add("siteId", cfg_.siteId).Add("build", cfg_.build).Add("port", static_cast<uint64_t>(cfg_.port)).Text();
					sendto(udp_, reply.data(), static_cast<int>(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), fromLen);
				}
				continue;
			}
			LanEvent e;
			e.kind = LanEvent::Kind::Found;
			e.hello.site = Get(kv, "site");
			e.hello.siteId = Get(kv, "siteId");
			e.hello.build = Get(kv, "build");
			e.address = launcher::FormatIpv4(addr);
			if (IsValidSite(e.hello.site) && IsValidSiteId(e.hello.siteId) && IsValidBuild(e.hello.build)) q_.Emit(std::move(e));
		}
	}

	void Net::OnAccept()
	{
		for (;;)
		{
			sockaddr_in from{};
			int fromLen = sizeof(from);
			const SOCKET s = accept(listen_, reinterpret_cast<sockaddr*>(&from), &fromLen);
			if (s == INVALID_SOCKET) break;
			const uint32_t addr = ntohl(from.sin_addr.s_addr);
			const std::string text = launcher::FormatIpv4(addr);
			size_t halfOpen = 0, fromSame = 0;
			for (const auto& c : conns_)
			{
				if (c->phase == Conn::Phase::Session || c->phase == Conn::Phase::Dead) continue;
				halfOpen++;
				if (c->addr == addr) fromSame++;
			}
			if (!launcher::IsPrivateAddress(addr, cfg_.allowLoopback))
			{
				Log("Refused at accept: " + text + " is not a private address");
				closesocket(s);
				continue;
			}
			if (halfOpen >= kMaxHalfOpen || fromSame >= 1)
			{
				Log("Refused at accept: half-open limit (" + text + ")");
				closesocket(s);
				continue;
			}
			auto c = std::make_unique<Conn>('H', 'G');
			c->s = s;
			c->addr = addr;
			c->ev = WSACreateEvent();
			WSAEventSelect(s, c->ev, FD_READ | FD_WRITE | FD_CLOSE);
			c->started = c->lastRecv = c->lastSend = GetTickCount64();
			c->phase = Conn::Phase::Hello;
			conns_.push_back(std::move(c));
		}
	}

	void Net::Connect(uint32_t addr)
	{
		reconnectAt_ = 0;
		for (auto& c : conns_) if (c->phase != Conn::Phase::Session) CloseConn(*c);
		auto c = std::make_unique<Conn>('G', 'H');
		c->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (c->s == INVALID_SOCKET)
		{
			ScheduleReconnect();
			return;
		}
		c->addr = addr;
		c->ev = WSACreateEvent();
		WSAEventSelect(c->s, c->ev, FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE);
		c->started = c->lastRecv = c->lastSend = GetTickCount64();
		c->phase = Conn::Phase::Connecting;
		c->pairing = !pairCode_.empty();
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = htonl(addr);
		to.sin_port = htons(static_cast<u_short>(cfg_.port));
		Emit(LanEvent::Kind::Status, "connecting to " + c->Addr());
		if (connect(c->s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0 && WSAGetLastError() != WSAEWOULDBLOCK)
		{
			Log("connect to " + c->Addr() + " failed, error " + std::to_string(WSAGetLastError()));
			c->phase = Conn::Phase::Dead;
			conns_.push_back(std::move(c));
			ScheduleReconnect();
			return;
		}
		conns_.push_back(std::move(c));
	}

	Hello Net::OwnHello(const std::string& role, const std::string& nonce) const
	{
		Hello h;
		h.role = role;
		h.site = cfg_.site;
		h.siteId = cfg_.siteId;
		h.nonce = nonce;
		h.build = cfg_.build;
		h.pluginsHash = cfg_.pluginsHash;
		return h;
	}

	// BuildHelloText drops regions past kMaxHelloRegionBytes; log what the guest will not see.
	bool Net::BuildHostHello(const Hello& h, std::string& text)
	{
		if (!BuildHelloText(h, text)) return false;
		Hello back;
		std::string error;
		if (ParseHello(text, back, error) && back.regions.size() < h.regions.size())
		{
			Log("HELLO lists " + std::to_string(back.regions.size()) + " of " + std::to_string(h.regions.size()) + " regions (size cap)");
		}
		return true;
	}

	// region.N lines for the pairing CONFIRM, within the same budget as a HELLO.
	std::string Net::RegionFields() const
	{
		std::string text;
		const std::vector<std::string> regions = HostRegions();
		for (size_t i = 0; i < regions.size(); i++)
		{
			const std::string line = Fields().Add("region." + std::to_string(i), regions[i]).Text();
			if (text.size() + line.size() > kMaxHelloRegionBytes) break;
			text += line;
		}
		return text;
	}

	std::vector<std::string> Net::HostRegions() const
	{
		std::vector<std::string> out;
		for (const auto& [name, isDir] : ListDir(cfg_.regionsDir))
		{
			const std::string n = launcher::Utf8(name);
			if (isDir && launcher::IsSyncableRegion(n)) out.push_back(n);
		}
		return out;
	}

	void Net::SendHandshake(Conn& c, FrameType type, const std::string& text, char dir, const Bytes32* key,
		std::span<const uint8_t> context, std::vector<uint8_t>* raw)
	{
		Frame f;
		f.type = type;
		f.text = text;
		if (EncodeHandshakeFrame(f, dir, key, context) != FrameStatus::Ok)
		{
			Drop(c, "handshake frame too large");
			return;
		}
		Queue(c, f.raw);
		if (raw) *raw = f.raw;
	}

	bool Net::SendFrame(Conn& c, FrameType type, const std::string& text, std::span<const uint8_t> data)
	{
		std::vector<uint8_t> bytes;
		const FrameStatus s = c.enc.Encode(type, text, data, bytes);
		if (s != FrameStatus::Ok)
		{
			Log(std::string("Frame not sent: ") + FrameStatusName(s));
			return false;
		}
		Queue(c, bytes);
		c.lastSend = GetTickCount64();
		return true;
	}

	void Net::Flush(Conn& c)
	{
		while (c.phase != Conn::Phase::Dead && c.outPos < c.out.size())
		{
			const int chunk = static_cast<int>(std::min<size_t>(c.out.size() - c.outPos, 256 * 1024));
			const int n = send(c.s, reinterpret_cast<const char*>(c.out.data() + c.outPos), chunk, 0);
			if (n < 0)
			{
				if (WSAGetLastError() != WSAEWOULDBLOCK) Drop(c, "send failed, error " + std::to_string(WSAGetLastError()));
				return;
			}
			c.outPos += static_cast<size_t>(n);
		}
		if (c.outPos == c.out.size())
		{
			c.out.clear();
			c.outPos = 0;
			if (c.closing) CloseConn(c);
		}
	}

	void Net::OnConn(Conn& c, const WSANETWORKEVENTS& ne)
	{
		if (ne.lNetworkEvents & FD_CONNECT)
		{
			if (ne.iErrorCode[FD_CONNECT_BIT] != 0)
			{
				Log("connect to " + c.Addr() + " failed, error " + std::to_string(ne.iErrorCode[FD_CONNECT_BIT]));
				CloseConn(c);
				return;
			}
			c.phase = Conn::Phase::Hello;
			c.started = c.lastRecv = GetTickCount64();
			c.nonceG = RandomHex(16);
			if (c.nonceG.empty())
			{
				reconnect_ = false;
				Emit(LanEvent::Kind::Refused, "the system random generator failed; not connecting", "random");
				return Drop(c, "BCryptGenRandom failed");
			}
			Hello h = OwnHello("guest", c.nonceG);
			h.pair = c.pairing;
			std::string text;
			if (!BuildHelloText(h, text))
			{
				Drop(c, "own HELLO invalid");
				return;
			}
			SendHandshake(c, FrameType::Hello, text, 'G', c.pairing ? nullptr : &cfg_.peerKey, {}, &c.gRaw);
		}
		if (ne.lNetworkEvents & (FD_READ | FD_CLOSE)) Read(c);
	}

	// Each chunk is decoded before the next recv, so an oversized frame is refused after one
	// chunk. Paused while the UI is more than kMaxQueuedEventBytes behind.
	void Net::Read(Conn& c)
	{
		uint8_t buf[65536];
		c.readPending = false;
		while (c.phase != Conn::Phase::Dead)
		{
			if (ReadPaused())
			{
				c.readPending = true;
				return;
			}
			const int n = recv(c.s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
			if (n <= 0)
			{
				if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) c.closing = true;   // peer closed
				break;
			}
			c.dec.Feed(std::span<const uint8_t>(buf, static_cast<size_t>(n)));
			Frame f;
			for (;;)
			{
				if (c.phase == Conn::Phase::Dead) return;
				const FrameStatus st = c.dec.Next(f);
				if (st == FrameStatus::NeedMore) break;
				if (st != FrameStatus::Ok)
				{
					Drop(c, std::string("bad frame: ") + FrameStatusName(st));
					return;
				}
				c.lastRecv = GetTickCount64();
				OnFrame(c, f);
			}
		}
		if (c.closing && c.phase != Conn::Phase::Dead && c.out.empty()) CloseConn(c);
	}

	void Net::OnFrame(Conn& c, Frame& f)
	{
		if (f.type == FrameType::Bye && c.phase != Conn::Phase::Session)
		{
			// Unauthenticated BYE before the session: only a refusal message, reconnect rules unchanged
			// except the pairing "wrong code" case.
			KeyValues kv;
			const std::string reason = ParseFields(f.text, kv) && IsValidReason(Get(kv, "reason")) ? Get(kv, "reason") : "unknown";
			if (!IsHost())
			{
				if (c.pairing) pairCode_.clear();
				Emit(LanEvent::Kind::Refused, reason == "code" ? "wrong code" : "refused: " + reason, reason);
			}
			Drop(c, "BYE reason=" + reason);
			return;
		}
		switch (c.phase)
		{
		case Conn::Phase::Hello:
			if (f.type != FrameType::Hello) return Drop(c, "expected HELLO");
			return IsHost() ? HostHello(c, f) : GuestHello(c, f);
		case Conn::Phase::Pake:
			if (f.type != FrameType::Pake) return Drop(c, "expected PAKE");
			return HostPake(c, f);
		case Conn::Phase::Confirm:
			if (f.type != FrameType::Confirm) return Drop(c, "expected CONFIRM");
			return IsHost() ? HostConfirm(c, f) : GuestConfirm(c, f);
		case Conn::Phase::Session:
			return SessionFrame(c, f);
		default:
			return;
		}
	}

	void Net::HostRefuse(Conn& c, const std::string& reason, const Bytes32* key)
	{
		Hello h = OwnHello("host", RandomHex(16));
		h.result = "refused";
		h.reason = reason;
		std::string text;
		if (BuildHelloText(h, text)) SendHandshake(c, FrameType::Hello, text, 'H', key, c.gRaw);
		Log("Refused " + c.Addr() + ": " + reason);
		c.closing = true;
		c.closeAt = GetTickCount64() + 2000;
	}

	void Net::HostHello(Conn& c, Frame& f)
	{
		std::string error;
		Hello g;
		if (!ParseHello(f.text, g, error) || g.role != "guest") return Drop(c, "bad HELLO: " + error);
		c.gRaw = f.raw;
		c.peer = g;
		c.pairing = g.pair;
		if (g.proto != kProto) return HostRefuse(c, "proto", nullptr);
		if (SameHost(g.site, cfg_.site)) return HostRefuse(c, "site", nullptr);
		if (g.pair)
		{
			// The only action on an unauthenticated pairing HELLO is this reply.
			const ULONGLONG now = GetTickCount64();
			if (g.build != cfg_.build) return HostRefuse(c, "version", nullptr);
			if (code_.empty()) return HostRefuse(c, Session() ? "busy" : "nocode", nullptr);
			if (lastPairTick_ && now - lastPairTick_ < kPairIntervalMs) return HostRefuse(c, "rate", nullptr);
			lastPairTick_ = now;
			std::string code = code_;
			crypto_wipe(code_.data(), code_.size());
			code_.clear();   // burned: one attempt per code
			c.nonceH = RandomHex(16);
			Bytes32 p;
			const bool random = !c.nonceH.empty() && RandomBytes(c.scalar.data(), c.scalar.size());
			const bool ok = random && CpaceGenerator(code, cfg_.siteId, g.siteId, g.nonce, c.nonceH, p) && CpacePublic(c.scalar, p, c.yH);
			crypto_wipe(code.data(), code.size());
			crypto_wipe(p.data(), p.size());
			if (!ok) return Drop(c, random ? "CPace setup failed" : "BCryptGenRandom failed; pairing refused");
			// No region list before the code is proven: it comes with CONFIRM.
			Hello h = OwnHello("host", c.nonceH);
			h.result = "ok";
			h.y = ToHex(c.yH);
			std::string text;
			if (!BuildHostHello(h, text)) return Drop(c, "own HELLO invalid");
			SendHandshake(c, FrameType::Hello, text, 'H', nullptr, {}, &c.hRaw);
			c.phase = Conn::Phase::Pake;
			Log("Pairing attempt from " + c.Addr() + " (" + g.site + ")");
			return;
		}
		if (!cfg_.hasKey || g.siteId != cfg_.peerSiteId || !VerifyFrameMac(f, 'G', cfg_.peerKey))
		{
			return HostRefuse(c, "unpaired", nullptr);
		}
		if (!SameHost(g.site, cfg_.peerSite)) return HostRefuse(c, "site", &cfg_.peerKey);
		if (g.build != cfg_.build) return HostRefuse(c, "version", &cfg_.peerKey);
		c.nonceH = RandomHex(16);
		if (c.nonceH.empty()) return Drop(c, "BCryptGenRandom failed");
		Hello h = OwnHello("host", c.nonceH);
		h.result = "ok";
		h.regions = HostRegions();
		std::string text;
		if (!BuildHostHello(h, text)) return Drop(c, "own HELLO invalid");
		SendHandshake(c, FrameType::Hello, text, 'H', &cfg_.peerKey, c.gRaw, &c.hRaw);
		c.k = cfg_.peerKey;
		c.phase = Conn::Phase::Confirm;
	}

	void Net::HostPake(Conn& c, Frame& f)
	{
		Bytes32 yG, confG;
		if (!ParsePakeText(f.text, yG, confG)) return Drop(c, "bad PAKE");
		c.transcript = CpaceTranscript(c.gRaw, c.hRaw, yG, c.yH);
		if (!CpaceKey(c.scalar, yG, c.transcript, c.k) || !VerifyCpaceConfirmation(c.k, 'G', c.transcript, confG))
		{
			SendHandshake(c, FrameType::Bye, Fields().Add("reason", "code").Text(), 'H', nullptr, {});
			Log("Pairing from " + c.Addr() + " failed: wrong code");
			LanEvent e;
			e.kind = LanEvent::Kind::PairAttempt;
			e.address = c.Addr();
			q_.Emit(std::move(e));
			c.pairing = false;   // already reported
			c.closing = true;
			c.closeAt = GetTickCount64() + 2000;
			c.phase = Conn::Phase::Confirm;   // nothing more is accepted
			return;
		}
		SendHandshake(c, FrameType::Confirm, BuildConfirmText(CpaceConfirmation(c.k, 'H', c.transcript)) + RegionFields(), 'H', nullptr, {});
		cfg_.peerSite = c.peer.site;
		cfg_.peerSiteId = c.peer.siteId;
		cfg_.peerKey = c.k;
		cfg_.hasKey = true;
		StartSession(c, true);
	}

	void Net::HostConfirm(Conn& c, Frame& f)
	{
		if (c.pairing || c.closing) return Drop(c, "unexpected CONFIRM");
		if (!VerifyFrameMac(f, 'G', c.k, PairedConfirmContext(c.gRaw, c.hRaw, c.nonceH))) return Drop(c, "CONFIRM MAC failed");
		StartSession(c, false);
	}

	void Net::GuestRefused(Conn& c, const Hello& h, bool authenticated)
	{
		std::string text = "refused: " + h.reason;
		if (h.reason == "version") text = "version mismatch: host " + h.build + ", this PC " + cfg_.build;
		const bool stop = c.pairing || h.reason == "version" || h.reason == "unpaired";
		if (!authenticated && !c.pairing)
		{
			// Unauthenticated: only a re-pair prompt, after 3 in a row.
			if (++refusals_ >= 3)
			{
				reconnect_ = false;
				Emit(LanEvent::Kind::Refused, "pairing lost, Join again", "unpaired");
			}
			else
			{
				Log("Unauthenticated refusal from " + c.Addr() + ": " + h.reason);
			}
			return Drop(c, "refused " + h.reason, false);
		}
		if (stop) reconnect_ = false;
		Emit(LanEvent::Kind::Refused, text, h.reason);
		Drop(c, text, false);
	}

	void Net::GuestHello(Conn& c, Frame& f)
	{
		std::string error;
		Hello h;
		if (!ParseHello(f.text, h, error) || h.role != "host") return Drop(c, "bad HELLO: " + error);
		const bool authentic = !c.pairing && cfg_.hasKey && VerifyFrameMac(f, 'H', cfg_.peerKey, c.gRaw);
		if (h.result == "refused") return GuestRefused(c, h, authentic);
		if (h.result != "ok") return Drop(c, "HELLO without result");
		if (SameHost(h.site, cfg_.site)) return Drop(c, "host has our name");
		if (c.pairing)
		{
			if (h.build != cfg_.build)
			{
				h.reason = "version";
				return GuestRefused(c, h, true);
			}
			Bytes32 yH, p, yG;
			if (!FromHex(h.y, yH)) return Drop(c, "HELLO without Y");
			c.hRaw = f.raw;
			c.nonceH = h.nonce;
			const bool random = RandomBytes(c.scalar.data(), c.scalar.size());
			const bool ok = random && CpaceGenerator(pairCode_, h.siteId, cfg_.siteId, c.nonceG, c.nonceH, p) && CpacePublic(c.scalar, p, yG);
			crypto_wipe(pairCode_.data(), pairCode_.size());
			pairCode_.clear();
			crypto_wipe(p.data(), p.size());
			if (!ok) return Drop(c, random ? "CPace setup failed" : "BCryptGenRandom failed; pairing refused");
			c.transcript = CpaceTranscript(c.gRaw, c.hRaw, yG, yH);
			if (!CpaceKey(c.scalar, yH, c.transcript, c.k)) return Drop(c, "CPace key failed");
			SendHandshake(c, FrameType::Pake, BuildPakeText(yG, CpaceConfirmation(c.k, 'G', c.transcript)), 'G', nullptr, {});
			c.peer = h;
			c.phase = Conn::Phase::Confirm;
			return;
		}
		if (!authentic)
		{
			if (++refusals_ >= 3)
			{
				reconnect_ = false;
				Emit(LanEvent::Kind::Refused, "pairing lost, Join again", "unpaired");
			}
			return Drop(c, "host HELLO MAC failed");
		}
		if (h.siteId != cfg_.peerSiteId || !SameHost(h.site, cfg_.peerSite)) return Drop(c, "host identity changed");
		if (h.build != cfg_.build)
		{
			h.reason = "version";
			return GuestRefused(c, h, true);
		}
		c.hRaw = f.raw;
		c.nonceH = h.nonce;
		c.k = cfg_.peerKey;
		c.peer = h;
		SendHandshake(c, FrameType::Confirm, "", 'G', &c.k, PairedConfirmContext(c.gRaw, c.hRaw, c.nonceH));
		StartSession(c, false);
	}

	void Net::GuestConfirm(Conn& c, Frame& f)
	{
		Bytes32 confH;
		if (!ParseConfirmText(f.text, confH) || !VerifyCpaceConfirmation(c.k, 'H', c.transcript, confH))
		{
			Emit(LanEvent::Kind::Refused, "host confirmation failed", "code");
			return Drop(c, "host confirmation failed");
		}
		// The region list arrives here, after the code is proven.
		KeyValues kv;
		ParseFields(f.text, kv);
		c.peer.regions.clear();
		for (size_t i = 0;; i++)
		{
			const auto it = kv.find("region." + std::to_string(i));
			if (it == kv.end()) break;
			if (launcher::IsSyncableRegion(it->second)) c.peer.regions.push_back(it->second);
		}
		cfg_.peerSite = c.peer.site;
		cfg_.peerSiteId = c.peer.siteId;
		cfg_.peerKey = c.k;
		cfg_.hasKey = true;
		cfg_.peerAddress = c.addr;
		target_ = c.addr;
		reconnect_ = true;
		StartSession(c, true);
	}

	void Net::StartSession(Conn& c, bool pairedNow)
	{
		const Bytes32 ks = SessionKey(c.k, IsHost() ? c.peer.nonce : c.nonceG, c.nonceH);
		c.enc.StartSession(ks);
		c.dec.StartSession(ks);
		// An older connection from the same peer is replaced only now.
		for (auto& o : conns_)
		{
			if (o.get() != &c && o->phase != Conn::Phase::Dead) Drop(*o, "replaced by a new session");
		}
		c.phase = Conn::Phase::Session;
		c.selects = 0;
		c.lastRecv = c.lastSend = GetTickCount64();
		refusals_ = 0;
		sessionSince_ = GetTickCount64();
		Log(std::string(pairedNow ? "Paired with " : "Session with ") + c.peer.site + " at " + c.Addr());
		LanEvent e;
		e.kind = LanEvent::Kind::Session;
		e.hello = c.peer;
		e.flag = pairedNow;
		e.address = c.Addr();
		if (pairedNow) e.key = c.k;
		q_.Emit(std::move(e));
	}

	void Net::SessionFrame(Conn& c, Frame& f)
	{
		KeyValues kv;
		if (!ParseFields(f.text, kv)) return Drop(c, "malformed frame text");
		const auto forward = [&]()
		{
			LanEvent e;
			e.kind = LanEvent::Kind::Frame;
			e.type = f.type;
			e.fields = std::move(kv);
			e.data = std::move(f.data);
			q_.Emit(std::move(e));
		};
		switch (f.type)
		{
		case FrameType::Ping:
			return;
		case FrameType::Bye:
		{
			const std::string reason = IsValidReason(Get(kv, "reason")) ? Get(kv, "reason") : "unknown";
			if (!IsHost() && (reason == "unpaired" || reason == "version" || reason == "cap")) reconnect_ = false;
			Emit(LanEvent::Kind::Refused, "peer closed: " + reason, reason);
			return Drop(c, "BYE reason=" + reason);
		}
		case FrameType::Index:
		case FrameType::Put:
		case FrameType::Delete:
			return forward();
		default:
			break;
		}
		if (IsHost())
		{
			if (f.type == FrameType::Select)
			{
				const std::string region = Get(kv, "region");
				if (++c.selects > kSelectLimit) return Drop(c, "too many SELECT");
				std::string match;
				for (const std::string& r : HostRegions()) if (EqualsNoCase(r, region)) match = r;
				if (match.empty())
				{
					SendFrame(c, FrameType::Bye, Fields().Add("reason", "region").Text());
					c.closing = true;
					c.closeAt = GetTickCount64() + 2000;
					return;
				}
				serve_.Reset();
				stage_.reset();
				LanEvent e;
				e.kind = LanEvent::Kind::SelectAsked;
				e.text = match;
				q_.Emit(std::move(e));
				return;
			}
			if (f.type == FrameType::Get)
			{
				const std::string name = Get(kv, "name");
				if (!served_.count(LowerAscii(name)) || !EqualsNoCase(Get(kv, "region"), servedManifest_.region)) return Drop(c, "GET outside the manifest");
				// A repeated GET for a queued or streaming name is ignored.
				const auto same = [&](const std::string& n) { return EqualsNoCase(n, name); };
				if ((serve_.in.Ok() && same(serve_.current)) || std::any_of(serve_.names.begin(), serve_.names.end(), same)) return;
				serve_.names.push_back(name);
				return;
			}
			if (f.type == FrameType::Synced)
			{
				ClearHostStaging();
				return forward();
			}
			return Drop(c, "unexpected frame type");
		}
		if (f.type == FrameType::Manifest)
		{
			Manifest m;
			std::string error;
			if (!ParseManifest(f.text, std::string_view(reinterpret_cast<const char*>(f.data.data()), f.data.size()), m, error))
			{
				Emit(LanEvent::Kind::SyncFailed, "Host manifest refused: " + error);
				return Drop(c, "bad MANIFEST: " + error);
			}
			if (!EqualsNoCase(m.region, selected_)) return Drop(c, "MANIFEST for another region");
			AbandonDownloads();
			StartHash(m);
			return;
		}
		if (f.type == FrameType::File) return OnFile(c, kv, f.data);
		Drop(c, "unexpected frame type");
	}

	// ---- Host staging ----

	void Net::ClearHostStaging()
	{
		if (!hostSessionDir_.empty()) DeleteTree(hostSessionDir_);
		hostSessionDir_.clear();
		stageCache_.clear();
		served_.clear();
	}

	void Net::StartStage(const std::string& region, const std::map<std::string, Tile>& tiles)
	{
		auto job = std::make_unique<StageJob>();
		job->region = region;
		job->src = cfg_.regionsDir + L"\\" + Widen(region);
		job->tiles = tiles;
		job->manifest.region = region;
		uint64_t total = 0;
		for (const auto& [name, isDir] : ListDir(job->src))
		{
			const std::string n = launcher::Utf8(name);
			if (isDir)
			{
				Log("Stage: subfolder skipped: " + n);
				continue;
			}
			if (!IsValidRegionFileName(n))
			{
				Log("Stage: not sent (name rules): " + n);
				continue;
			}
			WIN32_FILE_ATTRIBUTE_DATA a{};
			if (GetFileAttributesExW((job->src + L"\\" + name).c_str(), GetFileExInfoStandard, &a))
			{
				total += (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
			}
			job->files.emplace_back(name, n);
		}
		if (job->files.empty()) return FailStage("region " + region + " has no files to send");
		ULARGE_INTEGER freeBytes{};
		const uint64_t need = total + total / 10 + 256ull * 1024 * 1024;
		if (!GetDiskFreeSpaceExW(cfg_.lanDir.c_str(), &freeBytes, nullptr, nullptr) || freeBytes.QuadPart < need)
		{
			return FailStage("not enough free space for staging (" + std::to_string(need >> 20) + " MiB needed)");
		}
		// Staged files from an earlier session move into this one when reused.
		const std::wstring oldDir = hostSessionDir_;
		hostSessionDir_ = cfg_.lanDir + L"\\staging\\h" + Widen(RandomHex(4));
		CreateDirectoryW((cfg_.lanDir + L"\\staging").c_str(), nullptr);
		if (!CreateDirectoryW(hostSessionDir_.c_str(), nullptr)) return FailStage("cannot create the staging folder");
		std::map<std::string, StagedFile> moved;
		for (auto& [key, sf] : stageCache_)
		{
			const std::wstring to = hostSessionDir_ + sf.path.substr(sf.path.find_last_of(L'\\'));
			if (MoveFileExW(sf.path.c_str(), to.c_str(), 0))
			{
				sf.path = to;
				moved[key] = sf;
			}
		}
		stageCache_ = std::move(moved);
		if (!oldDir.empty()) DeleteTree(oldDir);
		served_.clear();
		Log("Stage: " + region + ", " + std::to_string(job->files.size()) + " files, " + std::to_string(total >> 20) + " MiB");
		stage_ = std::move(job);
	}

	void Net::FailStage(const std::string& why)
	{
		Log("Stage failed: " + why);
		Emit(LanEvent::Kind::SyncFailed, "Host staging failed: " + why);
		stage_.reset();
		if (Conn* c = Session())
		{
			SendFrame(*c, FrameType::Bye, Fields().Add("reason", "stage").Text());
			c->closing = true;
			c->closeAt = GetTickCount64() + 2000;
		}
	}

	void Net::StepStage()
	{
		StageJob& j = *stage_;
		if (j.index >= j.files.size())
		{
			std::string text, data, error;
			if (!BuildManifestText(j.manifest, text, data, error)) return FailStage(error);
			servedManifest_ = j.manifest;
			Conn* c = Session();
			if (c)
			{
				SendFrame(*c, FrameType::Manifest, text, std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
				Log("Stage done: MANIFEST sent, " + std::to_string(j.manifest.files.size()) + " files");
			}
			stage_.reset();
			return;
		}
		const auto& [wname, name] = j.files[j.index];
		const std::wstring src = j.src + L"\\" + wname;
		const std::string lower = LowerAscii(name);
		const std::string cacheKey = j.region + "/" + lower;
		const auto retry = [&](const std::string& why)
		{
			j.in.Reset();
			j.out.Reset();
			if (!j.outPath.empty()) DeleteFileW(j.outPath.c_str());
			j.hasher.reset();
			if (++j.attempt >= kStageAttempts) return FailStage(name + ": " + why);
			Log("Stage: retry " + name + " (" + why + ")");
			WaitForSingleObject(q_.StopEvent(), 200);
		};
		const auto finishFile = [&](const StagedFile& sf)
		{
			ManifestEntry e;
			e.name = name;
			e.size = sf.id.size;
			e.hash = sf.hash;
			for (const auto& [file, tile] : j.tiles) if (EqualsNoCase(file, name)) e.tile = tile;
			j.manifest.files.push_back(e);
			served_[lower] = sf.path;
			stageCache_[cacheKey] = sf;
			j.index++;
			j.attempt = 0;
		};

		if (!j.in.Ok())
		{
			j.in.Reset(CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
			if (!j.in.Ok()) return retry("open failed, error " + std::to_string(GetLastError()));
			if (!GetFileId(j.in.h, j.before)) return retry("file id failed");
			if (const auto it = stageCache_.find(cacheKey); it != stageCache_.end() && it->second.id == j.before
				&& GetFileAttributesW(it->second.path.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				j.in.Reset();
				return finishFile(it->second);   // unchanged since the earlier session
			}
			wchar_t part[32];
			swprintf_s(part, L"\\f%zu.part", partSeq_++);
			j.outPath = hostSessionDir_ + part;
			j.out.Reset(CreateFileW(j.outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
			if (!j.out.Ok()) return FailStage("cannot write staging, error " + std::to_string(GetLastError()));
			j.hasher = std::make_unique<Hasher>();
		}
		// A save renaming this file over (City.sc4_) waits on our handle: let go and retry.
		FILE_STANDARD_INFO standard{};
		if (GetFileInformationByHandleEx(j.in.h, FileStandardInfo, &standard, sizeof(standard)) && standard.DeletePending)
		{
			return retry("delete pending (a save is replacing it)");
		}
		std::vector<uint8_t> buf(kChunk);
		DWORD read = 0;
		if (!ReadFile(j.in.h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)) return retry("read failed");
		if (read > 0)
		{
			DWORD written = 0;
			if (!WriteFile(j.out.h, buf.data(), read, &written, nullptr) || written != read) return FailStage("staging write failed");
			j.hasher->Update(buf.data(), read);
			return;
		}
		// End of file: the source must be the same file, size and time as when opened.
		j.in.Reset();
		const bool flushed = FlushFileBuffers(j.out.h) != FALSE;
		j.out.Reset();
		Handle again(CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, 0, nullptr));
		FileId after;
		if (!flushed || !again.Ok() || !GetFileId(again.h, after) || !(after == j.before)) return retry("changed while copying");
		StagedFile sf{ j.before, j.outPath, j.hasher->Hex() };
		j.hasher.reset();
		j.outPath.clear();
		finishFile(sf);
	}

	void Net::StepServe()
	{
		Conn* c = Session();
		if (!c)
		{
			serve_.Reset();
			return;
		}
		if (c->out.size() - c->outPos > kOutHighWater) return;
		if (!serve_.in.Ok())
		{
			const std::string name = serve_.names.front();
			serve_.names.pop_front();
			const auto it = served_.find(LowerAscii(name));
			if (it == served_.end()) return;
			serve_.in.Reset(CreateFileW(it->second.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
			LARGE_INTEGER size{};
			if (!serve_.in.Ok() || !GetFileSizeEx(serve_.in.h, &size))
			{
				Log("Serve: cannot open staged " + name);
				serve_.in.Reset();
				return;
			}
			serve_.current = name;
			serve_.offset = 0;
			serve_.size = static_cast<uint64_t>(size.QuadPart);
		}
		std::vector<uint8_t> buf(kFileChunk);
		DWORD read = 0;
		if (!ReadFile(serve_.in.h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr))
		{
			Log("Serve: read failed for " + serve_.current);
			serve_.in.Reset();
			return;
		}
		buf.resize(read);
		const bool last = serve_.offset + read >= serve_.size;
		SendFrame(*c, FrameType::File, Fields().Add("region", servedManifest_.region).Add("name", serve_.current)
			.Add("offset", serve_.offset).Add("last", last ? 1 : 0).Text(), buf);
		serve_.offset += read;
		if (last) serve_.in.Reset();
	}

	// ---- Guest hashing and downloads ----

	void Net::StartHash(const Manifest& m)
	{
		auto job = std::make_unique<HashJob>();
		job->dir = cfg_.regionsDir + L"\\" + Widen(m.region);
		const DWORD attrs = GetFileAttributesW(job->dir.c_str());
		if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
		{
			// Case-insensitive match: the folder may differ in case from the host's spelling.
			for (const auto& [name, isDir] : ListDir(job->dir))
			{
				if (!isDir) job->files.emplace_back(name, launcher::Utf8(name));
			}
		}
		job->manifest = m;
		hash_ = std::move(job);
	}

	void Net::StepHash()
	{
		HashJob& j = *hash_;
		if (j.index >= j.files.size())
		{
			LanEvent e;
			e.kind = LanEvent::Kind::Manifest;
			e.manifest = std::move(j.manifest);
			e.localHashes = std::move(j.hashes);
			manifest_ = e.manifest;
			hash_.reset();
			q_.Emit(std::move(e));
			return;
		}
		const auto& [wname, name] = j.files[j.index];
		if (!j.in.Ok())
		{
			j.in.Reset(CreateFileW((j.dir + L"\\" + wname).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
			if (!j.in.Ok())
			{
				j.hashes[name] = std::string(64, '0');   // unreadable: never "same", so a download replaces it
				j.index++;
				return;
			}
			j.hasher = std::make_unique<Hasher>();
		}
		std::vector<uint8_t> buf(kChunk);
		DWORD read = 0;
		if (ReadFile(j.in.h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) && read > 0)
		{
			j.hasher->Update(buf.data(), read);
			return;
		}
		j.in.Reset();
		j.hashes[name] = j.hasher->Hex();
		j.hasher.reset();
		j.index++;
	}

	void Net::AbandonDownloads()
	{
		downloads_.clear();
		getQueue_.clear();
		if (!guestSessionDir_.empty()) DeleteTree(guestSessionDir_);
		guestSessionDir_.clear();
	}

	void Net::StartDownloads(const std::vector<std::string>& names)
	{
		AbandonDownloads();
		Conn* c = Session();
		if (!c) return;
		guestSessionDir_ = cfg_.lanDir + L"\\staging\\g" + Widen(RandomHex(4));
		CreateDirectoryW((cfg_.lanDir + L"\\staging").c_str(), nullptr);
		if (!CreateDirectoryW(guestSessionDir_.c_str(), nullptr))
		{
			Emit(LanEvent::Kind::SyncFailed, "cannot create the staging folder");
			return;
		}
		size_t n = 0;
		for (const std::string& name : names)
		{
			const ManifestEntry* entry = nullptr;
			for (const auto& e : manifest_.files) if (e.name == name) entry = &e;
			if (!entry) continue;
			Download& d = downloads_[LowerAscii(name)];
			d.entry = entry;
			wchar_t part[32];
			swprintf_s(part, L"\\f%zu.part", n++);
			d.path = guestSessionDir_ + part;
			SendFrame(*c, FrameType::Get, Fields().Add("region", manifest_.region).Add("name", name).Text());
		}
		downloadTotal_ = downloads_.size();
		if (downloads_.empty())
		{
			LanEvent e;
			e.kind = LanEvent::Kind::Downloaded;
			e.stagingDir = guestSessionDir_;
			guestSessionDir_.clear();
			q_.Emit(std::move(e));
		}
	}

	void Net::OnFile(Conn& c, const KeyValues& kv, const std::vector<uint8_t>& data)
	{
		const auto it = downloads_.find(LowerAscii(Get(kv, "name")));
		uint64_t offset = 0;
		const std::string last = Get(kv, "last");
		if (it == downloads_.end() || it->second.done || !ParseU64(Get(kv, "offset"), offset) || (last != "0" && last != "1"))
		{
			return Drop(c, "FILE outside the download list");
		}
		Download& d = it->second;
		if (offset != d.received || offset + data.size() > d.entry->size) return Drop(c, "FILE offset or size out of range");
		if (!d.out.Ok())
		{
			d.out.Reset(CreateFileW(d.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
			if (!d.out.Ok())
			{
				Emit(LanEvent::Kind::SyncFailed, "cannot write staging, error " + std::to_string(GetLastError()));
				return AbandonDownloads();
			}
			d.hasher = std::make_unique<Hasher>();
		}
		DWORD written = 0;
		if (!data.empty() && (!WriteFile(d.out.h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) || written != data.size()))
		{
			Emit(LanEvent::Kind::SyncFailed, "staging write failed (disk full?)");
			return AbandonDownloads();
		}
		d.hasher->Update(data.data(), data.size());
		d.received += data.size();
		if (last != "1") return;
		const bool flushed = FlushFileBuffers(d.out.h) != FALSE;
		d.out.Reset();
		if (!flushed || d.received != d.entry->size || d.hasher->Hex() != d.entry->hash)
		{
			Log("Download of " + d.entry->name + " failed verification; SELECT again");
			AbandonDownloads();
			if (++c.selects > kSelectLimit) return Drop(c, "too many retries");
			SendFrame(c, FrameType::Select, Fields().Add("region", selected_).Text());
			return;
		}
		d.done = true;
		const size_t done = static_cast<size_t>(std::count_if(downloads_.begin(), downloads_.end(), [](const auto& p) { return p.second.done; }));
		LanEvent p;
		p.kind = LanEvent::Kind::Progress;
		p.done = done;
		p.total = downloadTotal_;
		q_.Emit(std::move(p));
		if (done < downloads_.size()) return;
		LanEvent e;
		e.kind = LanEvent::Kind::Downloaded;
		e.stagingDir = guestSessionDir_;
		for (const auto& [key, dl] : downloads_) e.staged[dl.entry->name] = dl.path;
		guestSessionDir_.clear();   // the UI owns it now
		downloads_.clear();
		q_.Emit(std::move(e));
	}

	// ---- Loop ----

	void Net::Command(LanCmd& cmd)
	{
		switch (cmd.kind)
		{
		case LanCmd::Kind::Start:
			Start(cmd.config);
			crypto_wipe(cmd.config.peerKey.data(), cmd.config.peerKey.size());
			return;
		case LanCmd::Kind::Stop:
			Reset();
			cfg_.role = LanRole::None;
			return;
		case LanCmd::Kind::SetCode:
			code_ = cmd.text;
			return;
		case LanCmd::Kind::Discover:
			discover_ = cmd.on && !IsHost();
			lastDiscover_ = 0;
			return;
		case LanCmd::Kind::Probe:
		{
			// Literal addresses only: names are resolved on Connect.
			uint32_t addr = 0;
			if (!ParseIpv4(cmd.text, addr)) return;
			if (!launcher::IsPrivateAddress(addr, cfg_.allowLoopback)) Emit(LanEvent::Kind::Status, cmd.text + " is not a private address");
			else SendDiscover(addr);
			return;
		}
		case LanCmd::Kind::Connect:
			BeginResolve(cmd.text, cmd.frameText);
			crypto_wipe(cmd.frameText.data(), cmd.frameText.size());
			return;
		case LanCmd::Kind::Select:
			if (Conn* c = Session())
			{
				selected_ = cmd.text;
				c->selects++;
				SendFrame(*c, FrameType::Select, Fields().Add("region", cmd.text).Text());
			}
			return;
		case LanCmd::Kind::Stage:
			if (Session()) StartStage(cmd.text, cmd.tiles);
			return;
		case LanCmd::Kind::Download:
			return StartDownloads(cmd.names);
		case LanCmd::Kind::Send:
			if (Conn* c = Session()) SendFrame(*c, cmd.type, cmd.frameText, cmd.data);
			return;
		case LanCmd::Kind::Close:
			reconnect_ = false;
			if (Conn* c = Session())
			{
				SendFrame(*c, FrameType::Bye, Fields().Add("reason", cmd.text).Text());
				c->closing = true;
				c->closeAt = GetTickCount64() + 2000;
			}
			return;
		}
	}

	bool Net::HasJob() const
	{
		bool serving = !serve_.names.empty() || serve_.in.Ok();
		if (serving)
		{
			// Over the high-water mark FD_WRITE wakes the loop; no busy spin.
			for (const auto& c : conns_)
			{
				if (c->phase == Conn::Phase::Session && c->out.size() - c->outPos > kOutHighWater) serving = false;
			}
		}
		return stage_ || hash_ || serving;
	}

	void Net::StepJobs()
	{
		if (stage_) StepStage();
		if (hash_) StepHash();
		if (!serve_.names.empty() || serve_.in.Ok()) StepServe();
	}

	void Net::Tick()
	{
		const ULONGLONG now = GetTickCount64();
		for (auto& c : conns_)
		{
			Conn& x = *c;
			if (x.phase == Conn::Phase::Dead) continue;
			if (x.closing && x.closeAt && now > x.closeAt) CloseConn(x);
			else if (x.phase == Conn::Phase::Connecting && now - x.started > kConnectTimeoutMs)
			{
				Log("connect to " + x.Addr() + " timed out");
				CloseConn(x);
			}
			else if (x.phase != Conn::Phase::Session && x.phase != Conn::Phase::Connecting && now - x.started > kHandshakeTimeoutMs)
			{
				Drop(x, "handshake timed out");
			}
			else if (x.phase == Conn::Phase::Session)
			{
				if (now - x.lastRecv > kDeadMs) Drop(x, "no frame for 20 s");
				else if (now - x.lastSend > kPingMs && x.out.empty()) SendFrame(x, FrameType::Ping, "");
			}
		}
		if (!IsHost() && cfg_.role == LanRole::Join && reconnectAt_ && now >= reconnectAt_ && !Session()
			&& std::none_of(conns_.begin(), conns_.end(), [](const auto& c) { return c->phase != Conn::Phase::Dead; }))
		{
			if (reconnect_ && target_) Connect(target_);
			else reconnectAt_ = 0;
		}
		if (discover_ && now - lastDiscover_ >= kDiscoverMs)
		{
			lastDiscover_ = now;
			SendDiscover(INADDR_BROADCAST);
			if (cfg_.allowLoopback) SendDiscover(INADDR_LOOPBACK);
		}
	}

	void Net::Run()
	{
		WSADATA wsa{};
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		{
			Log("WSAStartup failed");
			return;
		}
		std::deque<LanCmd> cmds;
		for (;;)
		{
			std::vector<HANDLE> handles = { q_.StopEvent(), q_.WakeEvent() };
			if (udpEv_ != WSA_INVALID_EVENT) handles.push_back(udpEv_);
			if (listenEv_ != WSA_INVALID_EVENT) handles.push_back(listenEv_);
			if (resolve_.active) handles.push_back(resolve_.ov.hEvent);
			for (const auto& c : conns_) if (c->ev != WSA_INVALID_EVENT && handles.size() < WSA_MAXIMUM_WAIT_EVENTS) handles.push_back(c->ev);
			WSAWaitForMultipleEvents(static_cast<DWORD>(handles.size()), handles.data(), FALSE, HasJob() ? 0 : 250, FALSE);
			if (q_.Stopping()) break;
			if (q_.TakeCommands(cmds))
			{
				for (LanCmd& cmd : cmds) Command(cmd);
				cmds.clear();
			}
			if (resolve_.active && WaitForSingleObject(resolve_.ov.hEvent, 0) == WAIT_OBJECT_0) EndResolve();
			WSANETWORKEVENTS ne{};
			if (udp_ != INVALID_SOCKET && WSAEnumNetworkEvents(udp_, udpEv_, &ne) == 0 && (ne.lNetworkEvents & FD_READ)) OnUdp();
			if (listen_ != INVALID_SOCKET && WSAEnumNetworkEvents(listen_, listenEv_, &ne) == 0 && (ne.lNetworkEvents & FD_ACCEPT)) OnAccept();
			for (size_t i = 0; i < conns_.size(); i++)
			{
				Conn& c = *conns_[i];
				if (c.phase == Conn::Phase::Dead) continue;
				ne = WSANETWORKEVENTS{};
				if (WSAEnumNetworkEvents(c.s, c.ev, &ne) == 0 && ne.lNetworkEvents) OnConn(c, ne);
				else if (c.readPending && !ReadPaused()) Read(c);   // no new FD_READ comes until recv runs
			}
			Tick();
			if (q_.Stopping()) break;
			StepJobs();
			for (auto& c : conns_) Flush(*c);
			Sweep();
		}
		Reset();
		WSACleanup();
	}
}

namespace launcher
{
	std::vector<std::pair<std::wstring, bool>> ListDir(const std::wstring& dir)
	{
		std::vector<std::pair<std::wstring, bool>> out;
		WIN32_FIND_DATAW fd{};
		const HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
		if (find == INVALID_HANDLE_VALUE) return out;
		do
		{
			const std::wstring name = fd.cFileName;
			if (name == L"." || name == L"..") continue;
			out.emplace_back(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
		} while (FindNextFileW(find, &fd));
		FindClose(find);
		return out;
	}

	// Never follows a reparse point.
	void DeleteTree(const std::wstring& dir)
	{
		if (dir.empty()) return;
		for (const auto& [name, isDir] : ListDir(dir))
		{
			const std::wstring path = dir + L"\\" + name;
			const DWORD attrs = GetFileAttributesW(path.c_str());
			if (isDir && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) DeleteTree(path);
			else if (isDir) RemoveDirectoryW(path.c_str());
			else DeleteFileW(path.c_str());
		}
		RemoveDirectoryW(dir.c_str());
	}

	std::string LowerAscii(std::string s)
	{
		for (char& c : s) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
		return s;
	}

	std::string Utf8(const std::wstring& wide)
	{
		if (wide.empty()) return {};
		const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
		if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
		return out;
	}

	bool IsPrivateAddress(uint32_t a, bool allowLoopback)
	{
		return (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8 || (allowLoopback && a == INADDR_LOOPBACK);
	}

	bool ParseIpv4(const std::string& text, uint32_t& hostOrder)
	{
		in_addr a{};
		if (text.empty() || inet_pton(AF_INET, text.c_str(), &a) != 1) return false;
		hostOrder = ntohl(a.s_addr);
		return true;
	}

	std::string FormatIpv4(uint32_t a)
	{
		return std::to_string(a >> 24) + "." + std::to_string((a >> 16) & 255) + "." + std::to_string((a >> 8) & 255) + "." + std::to_string(a & 255);
	}

	bool LanWorker::Start(HWND notify)
	{
		notify_ = notify;
		wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!wake_ || !stop_) return false;
		thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
		return thread_ != nullptr;
	}

	DWORD WINAPI LanWorker::ThreadProc(void* self)
	{
		auto* w = static_cast<LanWorker*>(self);
		try
		{
			Net net(*w);
			net.Run();
		}
		catch (...)
		{
			try
			{
				LanEvent e;
				e.kind = LanEvent::Kind::Closed;
				e.text = "LAN worker stopped after an internal error";
				e.reason = "error";
				w->Emit(std::move(e));
			}
			catch (...)
			{
			}
		}
		return 0;
	}

	void LanWorker::Post(LanCmd cmd)
	{
		{
			std::lock_guard<std::mutex> lock(inLock_);
			in_.push_back(std::move(cmd));
		}
		SetEvent(wake_);
	}

	bool LanWorker::TakeCommands(std::deque<LanCmd>& out)
	{
		std::lock_guard<std::mutex> lock(inLock_);
		if (in_.empty()) return false;
		out.swap(in_);
		return true;
	}

	void LanWorker::Emit(LanEvent e)
	{
		bool wasEmpty = false;
		{
			std::lock_guard<std::mutex> lock(outLock_);
			wasEmpty = out_.empty();
			queuedBytes_ += e.data.size() + e.text.size();
			out_.push_back(std::move(e));
		}
		if (wasEmpty) PostMessageW(notify_, WM_APP_LAN, 0, 0);
	}

	std::deque<LanEvent> LanWorker::Drain()
	{
		std::deque<LanEvent> out;
		std::lock_guard<std::mutex> lock(outLock_);
		out.swap(out_);
		queuedBytes_ = 0;
		return out;
	}

	bool LanWorker::Stop(DWORD ms)
	{
		if (!thread_) return true;
		SetEvent(stop_);
		if (WaitForSingleObject(thread_, ms) != WAIT_OBJECT_0) return false;
		CloseHandle(thread_);
		thread_ = nullptr;
		return true;
	}
}
