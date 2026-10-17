/*
 * sc4-region-sync - a SimCity 4 DLL plugin that keeps the region in sync
 * across multiple running game instances.
 *
 * NeighborSnapshot.h
 *
 * The small, plain-data summary of a city that crosses the border between
 * game instances. It mirrors the fields the game itself keeps in
 * cISC4RegionalCity for every tile in the region: the values a neighbor
 * reads when it needs to know about this city, without loading it.
 *
 * This file has no game dependencies on purpose so the format can be
 * unit-tested outside the game and read by other tools.
 */

#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <array>

namespace regionsync
{
	// The game indexes commodities by a small integer. The exact meaning of
	// each index is one of the things the Dump cheat exists to confirm; see
	// docs/OPEN-QUESTIONS.md. We carry enough slots to cover power, water,
	// garbage and whatever else the game tracks.
	constexpr size_t kCommoditySlots = 8;

	struct CommodityStats
	{
		float exported = 0.0f;
		float imported = 0.0f;
		float produced = 0.0f;
		float demanded = 0.0f;
		float costPerUnit = 0.0f;
	};

	// Population buckets the game reports per RCI group through
	// cISC4RegionalCity::GetPopulation(uint32_t type). The type IDs are the
	// standard SC4 RCI group IDs used by the demand simulator.
	struct RCIGroupPopulation
	{
		uint32_t groupId = 0;
		int32_t population = 0;
	};

	constexpr size_t kRCIGroupSlots = 12;

	struct NeighborSnapshot
	{
		// Identity
		int32_t regionX = 0;
		int32_t regionZ = 0;
		int32_t sizeX = 0;
		int32_t sizeZ = 0;
		uint32_t citySerial = 0;
		std::string cityName;
		bool established = false;

		// Provenance
		uint32_t schemaVersion = 1;
		int64_t writtenUnixTime = 0;   // wall clock on the writing machine
		int32_t simDateNumber = 0;     // cISC4Simulator::GetSimDateNumber of the writer
		uint32_t simYear = 0;
		uint32_t simMonth = 0;
		uint32_t simDay = 0;

		// Summary the demand/neighbor simulators consume
		int32_t population = 0;
		int32_t commercialJobs = 0;
		int32_t industrialJobs = 0;
		float workforcePercentage = 0.0f;
		int8_t mayorRating = 0;
		float budget = 0.0f;
		float income = 0.0f;

		std::array<CommodityStats, kCommoditySlots> commodities{};
		std::array<RCIGroupPopulation, kRCIGroupSlots> rciGroups{};
		size_t rciGroupCount = 0;
	};

	// Simple line-oriented key=value text format. No external JSON dependency,
	// trivially diffable, and readable by a PowerShell one-liner.
	bool WriteSnapshotFile(const std::filesystem::path& path, const NeighborSnapshot& snapshot, std::string& error);
	bool ReadSnapshotFile(const std::filesystem::path& path, NeighborSnapshot& snapshot, std::string& error);

	// "<x>_<z>.snapshot"
	std::string SnapshotFileName(int32_t regionX, int32_t regionZ);
}
