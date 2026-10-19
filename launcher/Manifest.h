/*
 * sc4-region-sync launcher
 *
 * Manifest.h - wire name rules, the MANIFEST text and the guest's per-tile diff
 * (docs/PLAN-LAN.md section 4). Game-free and Windows-free.
 */

#pragma once
#include "Frame.h"
#include "Journal.h"
#include <optional>
#include <set>

namespace lan
{
	constexpr uint64_t kMaxManifestFileSize = 1ull << 30;
	constexpr size_t kMaxManifestFiles = 1024;
	constexpr size_t kMaxRegionNameLength = 120;

	bool EqualsNoCase(std::string_view a, std::string_view b);

	// Printable ASCII, 1 to 120, none of <>:"/\|?*~, no leading or trailing space or dot,
	// not . or .., no reserved device name with any extension.
	bool IsValidRegionName(std::string_view s);
	// Region name rules plus *.sc4, region.ini or config.bmp (case-insensitive).
	bool IsValidRegionFileName(std::string_view s);
	// "x_z.snapshot", canonical, 0 <= x < sizeX, 0 <= z < sizeZ.
	bool ParseSnapshotName(std::string_view s, int32_t sizeX, int32_t sizeZ, Tile& t);
	// "<pid>-<start>.status", canonical, no overflow.
	bool ParseStatusName(std::string_view s, uint32_t& pid, uint64_t& start);

	struct ManifestEntry
	{
		std::string name;
		uint64_t size = 0;
		std::string hash;          // BLAKE2b-256, 64 lowercase hex
		std::optional<Tile> tile;  // from the host's .saves file
	};

	struct Manifest
	{
		std::string region;
		std::vector<ManifestEntry> files;
	};

	// MANIFEST: text = region, count; data = file.N=name|size|hash|tile lines (N from 0, tile may
	// be empty). The count and name caps bound the data well under the frame cap.
	// Both refuse bad names, case-only duplicates, an empty list and the caps.
	bool BuildManifestText(const Manifest& m, std::string& text, std::string& data, std::string& error);
	bool ParseManifest(std::string_view text, std::string_view data, Manifest& m, std::string& error);

	enum class DiffAction
	{
		Same,          // manifest hash equals the local file
		Download,      // GET into staging, then swap
		KeepNotSent,   // belongs to a kept tile: the local file (if any) stays, the manifest copy is not fetched.
		               // When a kept tile names no file, every unclaimed local *.sc4 that differs is kept too.
		Archive,       // local only, not in a kept tile
	};

	struct DiffItem
	{
		std::string name;        // manifest spelling, else the local one
		std::string localName;   // empty when there is no local file
		DiffAction action = DiffAction::Same;
	};

	struct DiffResult
	{
		std::vector<DiffItem> items;
		std::vector<Tile> keptTiles;
		uint64_t downloadBytes = 0;
	};

	// localHashes: every file in the guest's region folder, name -> 64 hex.
	// saves: the guest's .saves per tile (ParseSaveCounter). saveCountFailed: tiles whose writer's status has saveCountFailed=1.
	DiffResult DiffRegion(const Manifest& m, const std::map<std::string, std::string>& localHashes,
		const std::map<Tile, SaveCounter>& saves, const Journal& j, const std::set<Tile>& saveCountFailed);
}
