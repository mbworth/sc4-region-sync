/*
 * sc4-region-sync
 *
 * InstanceStatus.cpp - see InstanceStatus.h.
 */

#include "InstanceStatus.h"
#include <charconv>
#include <sstream>

namespace
{
	// Control characters would break the line format.
	std::string OneLine(const std::string& s)
	{
		std::string out = s;
		for (char& c : out)
		{
			if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
		}
		return out;
	}

	std::string Value(const regionsync::KeyValues& kv, const char* key)
	{
		auto it = kv.find(key);
		return it != kv.end() ? it->second : std::string();
	}
}

namespace regionsync
{
	std::string InstanceStatusFileName(uint32_t pid, uint64_t start)
	{
		return std::to_string(pid) + "-" + std::to_string(start) + ".status";
	}

	bool IsReapable(const std::string& fileName, const InstanceStatus& contents, const std::string& ownHost)
	{
		std::string_view name = fileName;
		for (std::string_view suffix : { std::string_view(".status.tmp"), std::string_view(".status") })
		{
			if (name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix)
			{
				name.remove_suffix(suffix.size());
				break;
			}
		}
		const size_t dash = name.find('-');
		if (dash == std::string_view::npos)
		{
			return false;
		}
		uint32_t pid = 0;
		uint64_t start = 0;
		const char* first = name.data();
		const char* mid = first + dash;
		const char* last = first + name.size();
		const auto p = std::from_chars(first, mid, pid);
		const auto t = std::from_chars(mid + 1, last, start);
		if (p.ec != std::errc() || p.ptr != mid || t.ec != std::errc() || t.ptr != last)
		{
			return false;
		}
		return !ownHost.empty() && contents.writer.Known() && contents.writer.host == ownHost
			&& contents.writer.pid == pid && contents.writer.start == start;
	}

	bool WriteInstanceStatus(const std::filesystem::path& path, const InstanceStatus& s, std::string& error)
	{
		std::ostringstream out;
		out << "; sc4-region-sync instance status\n";
		out << "schemaVersion=" << s.schemaVersion << '\n';
		if (s.writer.Known() && IsValidWriterHost(s.writer.host))
		{
			out << "host=" << s.writer.host << '\n';
			out << "pid=" << s.writer.pid << '\n';
			out << "start=" << s.writer.start << '\n';
		}
		out << "userDir=" << OneLine(s.userDir) << '\n';
		out << "state=" << OneLine(s.state) << '\n';
		out << "region=" << OneLine(s.region) << '\n';
		if (s.state == "city")
		{
			out << "regionX=" << s.regionX << '\n';
			out << "regionZ=" << s.regionZ << '\n';
			out << "cityName=" << OneLine(s.cityName) << '\n';
			out << "conflict=" << (s.conflict ? 1 : 0) << '\n';
		}
		out << "writtenUnixTime=" << s.writtenUnixTime << '\n';
		return WriteFileAtomic(path, out.str(), error);
	}

	bool ReadInstanceStatus(const std::filesystem::path& path, InstanceStatus& s, std::string& error)
	{
		error.clear();
		KeyValues kv;
		if (!LoadKeyValues(path, kv, error))
		{
			return false;
		}
		ParseInstanceStatus(kv, s);
		return true;
	}

	void ParseInstanceStatus(std::string_view text, InstanceStatus& s)
	{
		KeyValues kv;
		ParseKeyValues(text, kv);
		ParseInstanceStatus(kv, s);
	}

	void ParseInstanceStatus(const KeyValues& kv, InstanceStatus& s)
	{
		s = InstanceStatus{};
		ReadKeyValue(kv, "schemaVersion", s.schemaVersion);
		// All three or none, as for snapshots.
		if (const std::string host = Value(kv, "host"); IsValidWriterHost(host))
		{
			WriterIdentity w;
			w.host = host;
			if (ReadKeyValue(kv, "pid", w.pid) && ReadKeyValue(kv, "start", w.start) && w.Known())
			{
				s.writer = std::move(w);
			}
		}
		s.userDir = Value(kv, "userDir");
		s.state = Value(kv, "state");
		s.region = Value(kv, "region");
		ReadKeyValue(kv, "regionX", s.regionX);
		ReadKeyValue(kv, "regionZ", s.regionZ);
		s.cityName = Value(kv, "cityName");
		int32_t conflict = 0;
		s.conflict = ReadKeyValue(kv, "conflict", conflict) && conflict != 0;
		ReadKeyValue(kv, "writtenUnixTime", s.writtenUnixTime);
	}
}
