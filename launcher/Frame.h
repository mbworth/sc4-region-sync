/*
 * sc4-region-sync launcher
 *
 * Frame.h - LAN wire format (docs/PLAN-LAN.md section 3). Game-free and Windows-free.
 *   u32 len | u8 version | u8 type | u64 counter | u32 textLen | text | data | mac[32]
 * Integers are little-endian. mac = keyed BLAKE2b-256(key, dir || context || version..data).
 */

#pragma once
#include "NeighborSnapshot.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lan
{
	using Bytes32 = std::array<uint8_t, 32>;
	using regionsync::KeyValues;

	enum class FrameType : uint8_t
	{
		Hello = 1, Pake, Confirm, Select, Manifest, Get, File, Synced, Index, Put, Delete, Ping, Bye,
	};

	constexpr uint8_t kFrameVersion = 1;
	constexpr size_t kFrameFixed = 14;   // version, type, counter, textLen
	constexpr size_t kMacSize = 32;
	constexpr size_t kMaxLenHandshake = 4 * 1024;
	constexpr size_t kMaxLenSession = 1024 * 1024 + 64;
	constexpr size_t kMaxText = 64 * 1024;

	enum class FrameStatus
	{
		Ok, NeedMore, BadLength, BadVersion, UnknownType, TextTooLong, BadCounter, BadMac,
	};
	const char* FrameStatusName(FrameStatus s);

	struct Frame
	{
		FrameType type = FrameType::Ping;
		uint64_t counter = 0;
		std::string text;
		std::vector<uint8_t> data;
		Bytes32 mac{};
		std::vector<uint8_t> raw;   // whole frame as on the wire (transcripts); filled by encode and decode
	};

	// dir is the sender: 'G' or 'H'.
	Bytes32 ComputeFrameMac(const Frame& f, char dir, const Bytes32& key, std::span<const uint8_t> context = {});
	bool VerifyFrameMac(const Frame& f, char dir, const Bytes32& key, std::span<const uint8_t> context = {});

	// Handshake frames: counter 0, 4 KiB cap. Fills f.mac and f.raw. A null key writes a zero MAC
	// (the pairing frames before K; CPace confirmations authenticate them afterwards).
	FrameStatus EncodeHandshakeFrame(Frame& f, char dir, const Bytes32* key, std::span<const uint8_t> context = {});

	// Session frames: counter 1, 2, ... MAC with Ks.
	class FrameEncoder
	{
	public:
		explicit FrameEncoder(char dir) : dir_(dir) {}
		void StartSession(const Bytes32& ks) { key_ = ks; counter_ = 0; }
		FrameStatus Encode(FrameType type, std::string_view text, std::span<const uint8_t> data, std::vector<uint8_t>& out);
	private:
		char dir_;
		Bytes32 key_{};
		uint64_t counter_ = 0;
	};

	// Incremental. Before StartSession: 4 KiB cap, counter must be 0, MAC not checked (the
	// caller uses VerifyFrameMac once it knows the key). After: caps, exact counter and MAC.
	// Any error is sticky: drop the connection.
	class FrameDecoder
	{
	public:
		explicit FrameDecoder(char peerDir) : peerDir_(peerDir) {}
		void Feed(std::span<const uint8_t> bytes);
		FrameStatus Next(Frame& out);
		void StartSession(const Bytes32& ks) { key_ = ks; session_ = true; counter_ = 0; }
	private:
		char peerDir_;
		bool session_ = false;
		Bytes32 key_{};
		uint64_t counter_ = 0;
		std::vector<uint8_t> buf_;
		size_t pos_ = 0;
		FrameStatus error_ = FrameStatus::Ok;
	};

	bool IsKnownFrameType(uint8_t t);

	// Key=value text, snapshot syntax. Add refuses keys or values with CR, LF or other control
	// characters and keys with '='; a refused field poisons the builder (Ok() false).
	class Fields
	{
	public:
		Fields& Add(std::string_view key, std::string_view value);
		Fields& Add(std::string_view key, uint64_t value) { return Add(key, std::string_view(std::to_string(value))); }
		bool Ok() const { return ok_; }
		const std::string& Text() const { return text_; }
	private:
		std::string text_;
		bool ok_ = true;
	};

	// Strict read of a frame's text: every line is key=value, no CR, no control characters,
	// no empty or duplicate keys, no comments.
	bool ParseFields(std::string_view text, KeyValues& out);
	std::string Get(const KeyValues& kv, const std::string& key);

	std::string ToHex(std::span<const uint8_t> bytes);
	bool FromHex(std::string_view hex, std::span<uint8_t> out);   // exact length, lowercase only
	bool IsLowerHex(std::string_view s, size_t length);
	bool ParseU64(std::string_view s, uint64_t& out);   // canonical decimal: no sign, no leading zero
}
