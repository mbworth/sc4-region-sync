/*
 * sc4-region-sync
 *
 * InstanceStatus.h - one file per running instance under
 * <store base>\_instances, read by the launcher (docs/PLAN-LAUNCHER.md A1).
 * Game-free and Windows-free.
 */

#pragma once
#include "NeighborSnapshot.h"

namespace regionsync
{
	struct InstanceStatus
	{
		uint32_t schemaVersion = 1;
		WriterIdentity writer;     // host, pid, start
		std::string userDir;       // -UserDir, no trailing backslash; empty without one
		std::string state = "starting";   // starting, region, city
		std::string region;        // region directory name; empty when state is starting
		// City fields; written only when state is city.
		int32_t regionX = -1;
		int32_t regionZ = -1;
		std::string cityName;
		bool conflict = false;
		// Sticky for the process: tiles whose .saves write failed. Key saveCountFailed=<x>_<z>[,...].
		std::vector<std::pair<int32_t, int32_t>> saveCountFailedTiles;
		int64_t writtenUnixTime = 0;
	};

	// <store base>\_saves\<region>\<x>_<z>.saves, one per tile, rewritten on every save attempt.
	struct SaveCounter
	{
		uint32_t count = 0;
		std::string file;          // save file basename, UTF-8
		std::string prevFile;      // basename the city was loaded from, when different
		int64_t lastSaveUnix = 0;
		int32_t simDateNumber = 0;
		int32_t population = 0;
		std::string cityName;
		WriterIdentity writer;
		int32_t lastOk = -1;       // 1 ok, 0 failed, -1 absent (save in progress or interrupted)
	};

	// "<x>_<z>.saves"
	std::string SaveCounterFileName(int32_t regionX, int32_t regionZ);
	std::string FormatSaveCounter(const SaveCounter& c);
	void ParseSaveCounter(std::string_view text, SaveCounter& c);
	void ParseSaveCounter(const KeyValues& kv, SaveCounter& c);
	// Missing file: count 0, true. False on a read error or an unparsable count.
	bool ReadSaveCounter(const std::filesystem::path& path, SaveCounter& c, std::string& error);

	// "<pid>-<start>.status"
	std::string InstanceStatusFileName(uint32_t pid, uint64_t start);

	// Startup reaper rule for "<pid>-<start>.status" or ".status.tmp": true only
	// when the contents name the same pid and start as the file name and our
	// host. A half-written or foreign file is kept. The caller still checks
	// that the writer is Dead before deleting.
	bool IsReapable(const std::string& fileName, const InstanceStatus& contents, const std::string& ownHost);

	bool WriteInstanceStatus(const std::filesystem::path& path, const InstanceStatus& status, std::string& error);
	bool ReadInstanceStatus(const std::filesystem::path& path, InstanceStatus& status, std::string& error);
	// Text form, shared with the launcher (which reads the file itself with share-delete).
	void ParseInstanceStatus(std::string_view text, InstanceStatus& status);
	void ParseInstanceStatus(const KeyValues& kv, InstanceStatus& status);
}
