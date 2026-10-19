/*
 * sc4-region-sync launcher
 *
 * Journal.h - guest journal journal\<peerSiteId>\<region>.txt (docs/PLAN-LAN.md section 4).
 * One line per tile: "x_z=file|baseHash|baseCount". Game-free and Windows-free.
 */

#pragma once
#include "InstanceStatus.h"
#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lan
{
	struct Tile
	{
		int32_t x = 0;
		int32_t z = 0;
		auto operator<=>(const Tile&) const = default;
	};

	std::string TileKey(Tile t);                     // "x_z"
	bool ParseTileKey(std::string_view s, Tile& t);  // canonical: (0|[1-9][0-9]{0,3})_(same)

	struct JournalEntry
	{
		std::string file;       // empty when the host had no file for the tile
		std::string baseHash;   // 64 hex, or empty with file
		uint64_t baseCount = 0;
	};

	struct Journal
	{
		std::map<Tile, JournalEntry> tiles;
	};

	std::string BuildJournalText(const Journal& j);
	bool ParseJournalText(std::string_view text, Journal& j, std::string& error);

	using regionsync::SaveCounter;
	struct Manifest;

	// After a successful swap. Kept tiles are left as they were, so a guest save stays
	// "not sent" across reconnects. Every other tile seen in the manifest, the journal or
	// the .saves gets file and baseHash from the manifest (the file now on disk) and,
	// when it has a local .saves, baseCount = count.
	void UpdateJournalAfterSwap(Journal& j, const Manifest& m, const std::map<Tile, SaveCounter>& saves,
		const std::vector<Tile>& keptTiles);
}
