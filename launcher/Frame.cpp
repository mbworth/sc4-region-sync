/*
 * sc4-region-sync launcher
 *
 * Frame.cpp - see Frame.h.
 */

#include "Frame.h"
#include "monocypher.h"
#include <algorithm>
#include <charconv>
#include <set>

namespace
{
	void PutU32(std::vector<uint8_t>& out, uint32_t v)
	{
		for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}

	void PutU64(std::vector<uint8_t>& out, uint64_t v)
	{
		for (int i = 0; i < 8; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}

	uint64_t GetLE(const uint8_t* p, int n)
	{
		uint64_t v = 0;
		for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
		return v;
	}

	// version..data
	std::vector<uint8_t> Body(const lan::Frame& f)
	{
		std::vector<uint8_t> out;
		out.reserve(lan::kFrameFixed + f.text.size() + f.data.size());
		out.push_back(lan::kFrameVersion);
		out.push_back(static_cast<uint8_t>(f.type));
		PutU64(out, f.counter);
		PutU32(out, static_cast<uint32_t>(f.text.size()));
		out.insert(out.end(), f.text.begin(), f.text.end());
		out.insert(out.end(), f.data.begin(), f.data.end());
		return out;
	}

	lan::FrameStatus Encode(lan::Frame& f, char dir, const lan::Bytes32* key, std::span<const uint8_t> context, size_t cap)
	{
		if (f.text.size() > lan::kMaxText) return lan::FrameStatus::TextTooLong;
		const size_t len = lan::kFrameFixed + f.text.size() + f.data.size() + lan::kMacSize;
		if (len > cap) return lan::FrameStatus::BadLength;
		if (key) f.mac = lan::ComputeFrameMac(f, dir, *key, context);
		else f.mac.fill(0);
		f.raw.clear();
		f.raw.reserve(4 + len);
		PutU32(f.raw, static_cast<uint32_t>(len));
		const std::vector<uint8_t> body = Body(f);
		f.raw.insert(f.raw.end(), body.begin(), body.end());
		f.raw.insert(f.raw.end(), f.mac.begin(), f.mac.end());
		return lan::FrameStatus::Ok;
	}

	bool IsControl(char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; }
}

namespace lan
{
	const char* FrameStatusName(FrameStatus s)
	{
		switch (s)
		{
		case FrameStatus::Ok: return "ok";
		case FrameStatus::NeedMore: return "need-more";
		case FrameStatus::BadLength: return "bad-length";
		case FrameStatus::BadVersion: return "bad-version";
		case FrameStatus::UnknownType: return "unknown-type";
		case FrameStatus::TextTooLong: return "text-too-long";
		case FrameStatus::BadCounter: return "bad-counter";
		case FrameStatus::BadMac: return "bad-mac";
		}
		return "?";
	}

	bool IsKnownFrameType(uint8_t t)
	{
		return t >= static_cast<uint8_t>(FrameType::Hello) && t <= static_cast<uint8_t>(FrameType::Bye);
	}

	Bytes32 ComputeFrameMac(const Frame& f, char dir, const Bytes32& key, std::span<const uint8_t> context)
	{
		const std::vector<uint8_t> body = Body(f);
		const uint8_t d = static_cast<uint8_t>(dir);
		crypto_blake2b_ctx ctx;
		crypto_blake2b_keyed_init(&ctx, 32, key.data(), key.size());
		crypto_blake2b_update(&ctx, &d, 1);
		crypto_blake2b_update(&ctx, context.data(), context.size());
		crypto_blake2b_update(&ctx, body.data(), body.size());
		Bytes32 mac;
		crypto_blake2b_final(&ctx, mac.data());
		return mac;
	}

	bool VerifyFrameMac(const Frame& f, char dir, const Bytes32& key, std::span<const uint8_t> context)
	{
		const Bytes32 mac = ComputeFrameMac(f, dir, key, context);
		return crypto_verify32(mac.data(), f.mac.data()) == 0;
	}

	FrameStatus EncodeHandshakeFrame(Frame& f, char dir, const Bytes32* key, std::span<const uint8_t> context)
	{
		f.counter = 0;
		return Encode(f, dir, key, context, kMaxLenHandshake);
	}

	FrameStatus FrameEncoder::Encode(FrameType type, std::string_view text, std::span<const uint8_t> data, std::vector<uint8_t>& out)
	{
		Frame f;
		f.type = type;
		f.counter = counter_ + 1;
		f.text.assign(text);
		f.data.assign(data.begin(), data.end());
		const FrameStatus s = ::Encode(f, dir_, &key_, {}, kMaxLenSession);
		if (s == FrameStatus::Ok)
		{
			counter_++;
			out = std::move(f.raw);
		}
		return s;
	}

	void FrameDecoder::Feed(std::span<const uint8_t> bytes)
	{
		if (pos_ > 0)
		{
			buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
			pos_ = 0;
		}
		buf_.insert(buf_.end(), bytes.begin(), bytes.end());
	}

	FrameStatus FrameDecoder::Next(Frame& out)
	{
		if (error_ != FrameStatus::Ok) return error_;
		auto fail = [&](FrameStatus s) { error_ = s; return s; };
		const size_t avail = buf_.size() - pos_;
		if (avail < 4) return FrameStatus::NeedMore;
		const uint8_t* p = buf_.data() + pos_;
		const uint64_t len = GetLE(p, 4);
		if (len < kFrameFixed + kMacSize || len > (session_ ? kMaxLenSession : kMaxLenHandshake)) return fail(FrameStatus::BadLength);
		// Header fields are checked as soon as they arrive, before the body is buffered.
		if (avail >= 5 && p[4] != kFrameVersion) return fail(FrameStatus::BadVersion);
		if (avail >= 6 && !IsKnownFrameType(p[5])) return fail(FrameStatus::UnknownType);
		if (avail >= 4 + kFrameFixed)
		{
			const uint64_t textLen = GetLE(p + 14, 4);
			if (textLen > kMaxText) return fail(FrameStatus::TextTooLong);
			if (textLen > len - kFrameFixed - kMacSize) return fail(FrameStatus::BadLength);
		}
		if (avail < 4 + len) return FrameStatus::NeedMore;

		Frame f;
		f.type = static_cast<FrameType>(p[5]);
		f.counter = GetLE(p + 6, 8);
		const size_t textLen = static_cast<size_t>(GetLE(p + 14, 4));
		const uint8_t* text = p + 4 + kFrameFixed;
		const size_t dataLen = static_cast<size_t>(len) - kFrameFixed - textLen - kMacSize;
		f.text.assign(reinterpret_cast<const char*>(text), textLen);
		f.data.assign(text + textLen, text + textLen + dataLen);
		std::copy(text + textLen + dataLen, text + textLen + dataLen + kMacSize, f.mac.begin());
		f.raw.assign(p, p + 4 + len);
		pos_ += 4 + static_cast<size_t>(len);

		if (session_)
		{
			if (f.counter != counter_ + 1) return fail(FrameStatus::BadCounter);
			if (!VerifyFrameMac(f, peerDir_, key_)) return fail(FrameStatus::BadMac);
			counter_++;
		}
		else if (f.counter != 0)
		{
			return fail(FrameStatus::BadCounter);
		}
		out = std::move(f);
		return FrameStatus::Ok;
	}

	Fields& Fields::Add(std::string_view key, std::string_view value)
	{
		if (key.empty() || key.find('=') != std::string_view::npos || key[0] == ';' || key[0] == '#')
		{
			ok_ = false;
		}
		for (char c : key) if (IsControl(c)) ok_ = false;
		for (char c : value) if (IsControl(c)) ok_ = false;
		if (ok_)
		{
			text_.append(key).append("=").append(value).append("\n");
		}
		return *this;
	}

	bool ParseFields(std::string_view text, KeyValues& out)
	{
		out.clear();
		std::set<std::string_view> keys;
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t end = text.find('\n', pos);
			if (end == std::string_view::npos) end = text.size();
			const std::string_view line = text.substr(pos, end - pos);
			pos = end + 1;
			for (char c : line) if (IsControl(c)) return false;
			const size_t eq = line.find('=');
			if (eq == std::string_view::npos || eq == 0 || line[0] == ';' || line[0] == '#') return false;
			if (!keys.insert(line.substr(0, eq)).second) return false;
		}
		regionsync::ParseKeyValues(text, out);
		return true;
	}

	std::string Get(const KeyValues& kv, const std::string& key)
	{
		const auto it = kv.find(key);
		return it != kv.end() ? it->second : std::string();
	}

	std::string ToHex(std::span<const uint8_t> bytes)
	{
		static const char digits[] = "0123456789abcdef";
		std::string out;
		out.reserve(bytes.size() * 2);
		for (uint8_t b : bytes)
		{
			out.push_back(digits[b >> 4]);
			out.push_back(digits[b & 15]);
		}
		return out;
	}

	bool IsLowerHex(std::string_view s, size_t length)
	{
		if (s.size() != length) return false;
		for (char c : s)
		{
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
		}
		return true;
	}

	bool FromHex(std::string_view hex, std::span<uint8_t> out)
	{
		if (!IsLowerHex(hex, out.size() * 2)) return false;
		auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
		for (size_t i = 0; i < out.size(); i++)
		{
			out[i] = static_cast<uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
		}
		return true;
	}

	bool ParseU64(std::string_view s, uint64_t& out)
	{
		if (s.empty() || (s.size() > 1 && s[0] == '0')) return false;
		const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
		return r.ec == std::errc() && r.ptr == s.data() + s.size();
	}
}
