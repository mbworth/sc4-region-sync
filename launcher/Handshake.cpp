/*
 * sc4-region-sync launcher
 *
 * Handshake.cpp - see Handshake.h.
 */

#include "Handshake.h"
#include "Manifest.h"
#include "monocypher.h"

namespace
{
	bool IsZero(const lan::Bytes32& b)
	{
		const lan::Bytes32 zero{};
		return crypto_verify32(b.data(), zero.data()) == 0;
	}

	bool AllOf(std::string_view s, size_t minLen, size_t maxLen, bool (*ok)(char))
	{
		if (s.size() < minLen || s.size() > maxLen) return false;
		for (char c : s) if (!ok(c)) return false;
		return true;
	}

	bool IsDigit(char c) { return c >= '0' && c <= '9'; }
	bool IsLowerAlpha(char c) { return c >= 'a' && c <= 'z'; }
	bool IsSiteChar(char c) { return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-'; }

	void Update(crypto_blake2b_ctx& ctx, std::string_view s)
	{
		crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(s.data()), s.size());
	}

	bool ReadHex32(const lan::KeyValues& kv, const std::string& key, lan::Bytes32& out)
	{
		return lan::FromHex(lan::Get(kv, key), out);
	}
}

namespace lan
{
	bool IsValidSiteId(std::string_view s) { return IsLowerHex(s, 32); }
	bool IsValidSite(std::string_view s) { return AllOf(s, 1, 15, IsSiteChar); }
	bool IsValidPluginsHash(std::string_view s) { return IsLowerHex(s, 64); }
	bool IsValidNonce(std::string_view s) { return IsLowerHex(s, 32); }
	bool IsValidY(std::string_view s) { return IsLowerHex(s, 64); }
	bool IsValidPairCode(std::string_view s) { return AllOf(s, 6, 6, IsDigit); }
	bool IsValidReason(std::string_view s) { return AllOf(s, 1, 16, IsLowerAlpha); }

	bool IsValidBuild(std::string_view s)
	{
		const size_t plus = s.find('+');
		if (plus == std::string_view::npos || !IsLowerHex(s.substr(plus + 1), 7)) return false;
		const std::string_view version = s.substr(0, plus);
		if (version.empty() || version.size() > 32 || version.front() == '.' || version.back() == '.') return false;
		for (size_t i = 0; i < version.size(); i++)
		{
			if (version[i] == '.' ? version[i + 1] == '.' : !IsDigit(version[i])) return false;
		}
		return true;
	}

	bool BuildHelloText(const Hello& h, std::string& text)
	{
		if ((h.role != "host" && h.role != "guest") || !IsValidSite(h.site) || !IsValidSiteId(h.siteId)
			|| !IsValidNonce(h.nonce) || !IsValidBuild(h.build) || !IsValidPluginsHash(h.pluginsHash)
			|| (!h.result.empty() && h.result != "ok" && h.result != "refused")
			|| (!h.reason.empty() && !IsValidReason(h.reason)) || (!h.y.empty() && !IsValidY(h.y)))
		{
			return false;
		}
		Fields f;
		f.Add("proto", h.proto).Add("role", h.role).Add("site", h.site).Add("siteId", h.siteId)
			.Add("nonce", h.nonce).Add("pair", h.pair ? 1 : 0).Add("build", h.build).Add("pluginsHash", h.pluginsHash);
		if (!h.result.empty()) f.Add("result", h.result);
		if (!h.reason.empty()) f.Add("reason", h.reason);
		for (size_t i = 0; i < h.regions.size(); i++)
		{
			if (!IsValidRegionName(h.regions[i])) return false;
			if (f.Text().size() + h.regions[i].size() + 16 > kMaxHelloRegionBytes) break;
			f.Add("region." + std::to_string(i), h.regions[i]);
		}
		if (!h.y.empty()) f.Add("Y", h.y);
		if (!f.Ok()) return false;
		text = f.Text();
		return true;
	}

	bool ParseHello(std::string_view text, Hello& h, std::string& error)
	{
		h = Hello{};
		KeyValues kv;
		if (!ParseFields(text, kv))
		{
			error = "malformed text";
			return false;
		}
		auto bad = [&](const char* field) { error = std::string("bad ") + field; return false; };
		uint64_t proto = 0;
		if (!ParseU64(Get(kv, "proto"), proto) || proto == 0 || proto > 65535) return bad("proto");
		h.proto = static_cast<uint32_t>(proto);
		h.role = Get(kv, "role");
		if (h.role != "host" && h.role != "guest") return bad("role");
		h.site = Get(kv, "site");
		if (!IsValidSite(h.site)) return bad("site");
		h.siteId = Get(kv, "siteId");
		if (!IsValidSiteId(h.siteId)) return bad("siteId");
		h.nonce = Get(kv, "nonce");
		if (!IsValidNonce(h.nonce)) return bad("nonce");
		const std::string pair = Get(kv, "pair");
		if (pair != "0" && pair != "1") return bad("pair");
		h.pair = pair == "1";
		h.build = Get(kv, "build");
		if (!IsValidBuild(h.build)) return bad("build");
		h.pluginsHash = Get(kv, "pluginsHash");
		if (!IsValidPluginsHash(h.pluginsHash)) return bad("pluginsHash");
		if (kv.count("result"))
		{
			h.result = Get(kv, "result");
			if (h.role != "host" || (h.result != "ok" && h.result != "refused")) return bad("result");
		}
		if (kv.count("reason"))
		{
			h.reason = Get(kv, "reason");
			if (h.role != "host" || !IsValidReason(h.reason)) return bad("reason");
		}
		if (kv.count("Y"))
		{
			h.y = Get(kv, "Y");
			if (h.role != "host" || !IsValidY(h.y)) return bad("Y");
		}
		size_t regionKeys = 0;
		for (const auto& [key, value] : kv)
		{
			if (key.rfind("region.", 0) == 0) regionKeys++;
		}
		for (size_t i = 0; i < regionKeys; i++)
		{
			const auto it = kv.find("region." + std::to_string(i));
			if (it == kv.end() || h.role != "host" || !IsValidRegionName(it->second)) return bad("region");
			h.regions.push_back(it->second);
		}
		return true;
	}

	std::string BuildPakeText(const Bytes32& y, const Bytes32& conf)
	{
		return Fields().Add("Y", ToHex(y)).Add("conf", ToHex(conf)).Text();
	}

	bool ParsePakeText(std::string_view text, Bytes32& y, Bytes32& conf)
	{
		KeyValues kv;
		return ParseFields(text, kv) && ReadHex32(kv, "Y", y) && ReadHex32(kv, "conf", conf);
	}

	std::string BuildConfirmText(const Bytes32& conf)
	{
		return Fields().Add("conf", ToHex(conf)).Text();
	}

	bool ParseConfirmText(std::string_view text, Bytes32& conf)
	{
		KeyValues kv;
		return ParseFields(text, kv) && ReadHex32(kv, "conf", conf);
	}

	bool CpaceGenerator(std::string_view code, std::string_view hostSiteId, std::string_view guestSiteId,
		std::string_view nonceG, std::string_view nonceH, Bytes32& p)
	{
		if (!IsValidPairCode(code) || !IsValidSiteId(hostSiteId) || !IsValidSiteId(guestSiteId)
			|| !IsValidNonce(nonceG) || !IsValidNonce(nonceH))
		{
			return false;
		}
		// Fixed-length fields, so the concatenation is unambiguous.
		crypto_blake2b_ctx ctx;
		crypto_blake2b_init(&ctx, 32);
		for (std::string_view s : { std::string_view("sc4rs-cpace"), code, hostSiteId, guestSiteId, nonceG, nonceH })
		{
			Update(ctx, s);
		}
		Bytes32 hidden;
		crypto_blake2b_final(&ctx, hidden.data());
		// The map ignores the two padding bits of the representative.
		crypto_elligator_map(p.data(), hidden.data());
		crypto_wipe(hidden.data(), hidden.size());
		return !IsZero(p);
	}

	bool CpacePublic(const Bytes32& scalar, const Bytes32& p, Bytes32& y)
	{
		crypto_x25519(y.data(), scalar.data(), p.data());
		return !IsZero(y);
	}

	std::vector<uint8_t> CpaceTranscript(std::span<const uint8_t> guestHello, std::span<const uint8_t> hostHello,
		const Bytes32& yG, const Bytes32& yH)
	{
		std::vector<uint8_t> t(guestHello.begin(), guestHello.end());
		t.insert(t.end(), hostHello.begin(), hostHello.end());
		t.insert(t.end(), yG.begin(), yG.end());
		t.insert(t.end(), yH.begin(), yH.end());
		return t;
	}

	bool CpaceKey(const Bytes32& scalar, const Bytes32& yPeer, std::span<const uint8_t> transcript, Bytes32& k)
	{
		Bytes32 z;
		crypto_x25519(z.data(), scalar.data(), yPeer.data());
		const bool ok = !IsZero(z);
		if (ok)
		{
			crypto_blake2b_ctx ctx;
			crypto_blake2b_init(&ctx, 32);
			crypto_blake2b_update(&ctx, z.data(), z.size());
			crypto_blake2b_update(&ctx, transcript.data(), transcript.size());
			crypto_blake2b_final(&ctx, k.data());
		}
		crypto_wipe(z.data(), z.size());
		return ok;
	}

	Bytes32 CpaceConfirmation(const Bytes32& k, char who, std::span<const uint8_t> transcript)
	{
		const uint8_t w = static_cast<uint8_t>(who);
		crypto_blake2b_ctx ctx;
		crypto_blake2b_keyed_init(&ctx, 32, k.data(), k.size());
		Update(ctx, "conf");
		crypto_blake2b_update(&ctx, &w, 1);
		crypto_blake2b_update(&ctx, transcript.data(), transcript.size());
		Bytes32 conf;
		crypto_blake2b_final(&ctx, conf.data());
		return conf;
	}

	bool VerifyCpaceConfirmation(const Bytes32& k, char who, std::span<const uint8_t> transcript, const Bytes32& conf)
	{
		const Bytes32 expected = CpaceConfirmation(k, who, transcript);
		return crypto_verify32(expected.data(), conf.data()) == 0;
	}

	Bytes32 SessionKey(const Bytes32& k, std::string_view nonceG, std::string_view nonceH)
	{
		crypto_blake2b_ctx ctx;
		crypto_blake2b_keyed_init(&ctx, 32, k.data(), k.size());
		Update(ctx, "session");
		Update(ctx, nonceG);
		Update(ctx, nonceH);
		Bytes32 ks;
		crypto_blake2b_final(&ctx, ks.data());
		return ks;
	}

	std::vector<uint8_t> PairedConfirmContext(std::span<const uint8_t> guestHello, std::span<const uint8_t> hostHello,
		std::string_view nonceH)
	{
		std::vector<uint8_t> c(guestHello.begin(), guestHello.end());
		c.insert(c.end(), hostHello.begin(), hostHello.end());
		c.insert(c.end(), nonceH.begin(), nonceH.end());
		return c;
	}
}
