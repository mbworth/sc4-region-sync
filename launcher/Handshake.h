/*
 * sc4-region-sync launcher
 *
 * Handshake.h - HELLO field formats and CPace pairing over X25519 (docs/PLAN-LAN.md
 * section 3). Game-free and Windows-free. Randomness (scalars, nonces) comes from the caller.
 */

#pragma once
#include "Frame.h"

namespace lan
{
	constexpr uint32_t kProto = 1;
	constexpr size_t kMaxHelloRegionBytes = 3584;   // keeps the host HELLO under the 4 KiB handshake cap

	bool IsValidSiteId(std::string_view s);       // 32 lowercase hex
	bool IsValidSite(std::string_view s);         // 1 to 15 of [A-Za-z0-9-]
	bool IsValidBuild(std::string_view s);        // 0.2.0+abc1234
	bool IsValidPluginsHash(std::string_view s);  // 64 lowercase hex
	bool IsValidNonce(std::string_view s);        // 32 lowercase hex
	bool IsValidY(std::string_view s);            // 64 lowercase hex
	bool IsValidPairCode(std::string_view s);     // 6 digits
	bool IsValidReason(std::string_view s);       // 1 to 16 of [a-z]

	struct Hello
	{
		uint32_t proto = kProto;
		std::string role;          // host or guest
		std::string site;
		std::string siteId;
		std::string nonce;
		bool pair = false;
		std::string build;
		std::string pluginsHash;
		// Host only.
		std::string result;        // ok or refused; empty from the guest
		std::string reason;
		std::vector<std::string> regions;   // region.0 .. region.N-1
		std::string y;             // pairing only
	};

	// False when any field breaks its format. Regions past kMaxHelloRegionBytes of text are dropped;
	// compare ParseHello(text).regions.size() with h.regions.size() to log the truncation.
	bool BuildHelloText(const Hello& h, std::string& text);
	bool ParseHello(std::string_view text, Hello& h, std::string& error);

	// PAKE {Y, conf} and CONFIRM {conf}.
	std::string BuildPakeText(const Bytes32& y, const Bytes32& conf);
	bool ParsePakeText(std::string_view text, Bytes32& y, Bytes32& conf);
	std::string BuildConfirmText(const Bytes32& conf);
	bool ParseConfirmText(std::string_view text, Bytes32& conf);

	// P = crypto_elligator_map(BLAKE2b-256("sc4rs-cpace" || code || hostSiteId || guestSiteId || nonceG || nonceH)).
	// False on a malformed input.
	bool CpaceGenerator(std::string_view code, std::string_view hostSiteId, std::string_view guestSiteId,
		std::string_view nonceG, std::string_view nonceH, Bytes32& p);
	// Y = X25519(scalar, P). False for an all-zero (low-order) result.
	bool CpacePublic(const Bytes32& scalar, const Bytes32& p, Bytes32& y);
	// guestHello || hostHello || Y_G || Y_H (raw frames as on the wire).
	std::vector<uint8_t> CpaceTranscript(std::span<const uint8_t> guestHello, std::span<const uint8_t> hostHello,
		const Bytes32& yG, const Bytes32& yH);
	// Z = X25519(scalar, Y_peer), K = BLAKE2b-256(Z || transcript). False when Y_peer or Z is
	// all-zero (low-order point). Z is wiped.
	bool CpaceKey(const Bytes32& scalar, const Bytes32& yPeer, std::span<const uint8_t> transcript, Bytes32& k);
	// conf = keyed BLAKE2b-256(K, "conf" || who || transcript), who = 'G' or 'H'.
	Bytes32 CpaceConfirmation(const Bytes32& k, char who, std::span<const uint8_t> transcript);
	bool VerifyCpaceConfirmation(const Bytes32& k, char who, std::span<const uint8_t> transcript, const Bytes32& conf);

	// Ks = keyed BLAKE2b-256(K, "session" || nonceG || nonceH).
	Bytes32 SessionKey(const Bytes32& k, std::string_view nonceG, std::string_view nonceH);

	// Paired reconnect, frame MACs with K via EncodeHandshakeFrame / VerifyFrameMac:
	//   guest HELLO   context empty
	//   host HELLO    context = guest HELLO raw
	//   guest CONFIRM context = PairedConfirmContext(...)
	std::vector<uint8_t> PairedConfirmContext(std::span<const uint8_t> guestHello, std::span<const uint8_t> hostHello,
		std::string_view nonceH);
}
