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
		int64_t writtenUnixTime = 0;
	};

	// "<pid>-<start>.status"
	std::string InstanceStatusFileName(uint32_t pid, uint64_t start);

	// Startup reaper rule for "<pid>-<start>.status" or ".status.tmp": true only
	// when the contents name the same pid and start as the file name and our
	// host. A half-written or foreign file is kept. The caller still checks
	// that the writer is Dead before deleting.
	bool IsReapable(const std::string& fileName, const InstanceStatus& contents, const std::string& ownHost);

	bool WriteInstanceStatus(const std::filesystem::path& path, const InstanceStatus& status, std::string& error);
	bool ReadInstanceStatus(const std::filesystem::path& path, InstanceStatus& status, std::string& error);
}
