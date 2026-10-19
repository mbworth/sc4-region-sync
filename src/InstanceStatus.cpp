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

	template <typename T>
	bool ParseWhole(std::string_view text, T& out)
	{
		const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
		return r.ec == std::errc() && r.ptr == text.data() + text.size();
	}

	// "<x>_<z>[,<x>_<z>...]"; malformed entries are skipped.
	std::vector<std::pair<int32_t, int32_t>> ParseTileList(std::string_view text)
	{
		std::vector<std::pair<int32_t, int32_t>> tiles;
		while (!text.empty() && tiles.size() < regionsync::kMaxListEntries)
		{
			const size_t comma = text.find(',');
			const std::string_view item = text.substr(0, comma);
			text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
			const size_t sep = item.find('_');
			int32_t x = 0, z = 0;
			if (sep != std::string_view::npos && ParseWhole(item.substr(0, sep), x) && ParseWhole(item.substr(sep + 1), z))
			{
				tiles.emplace_back(x, z);
			}
		}
		return tiles;
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
		return !ownHost.empty() && contents.writer.Known() && SameHost(contents.writer.host, ownHost)
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
		if (!s.saveCountFailedTiles.empty())
		{
			out << "saveCountFailed=";
			for (size_t i = 0; i < s.saveCountFailedTiles.size(); i++)
			{
				out << (i ? "," : "") << s.saveCountFailedTiles[i].first << '_' << s.saveCountFailedTiles[i].second;
			}
			out << '\n';
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
		s.saveCountFailedTiles = ParseTileList(Value(kv, "saveCountFailed"));
		ReadKeyValue(kv, "writtenUnixTime", s.writtenUnixTime);
	}

	std::string SaveCounterFileName(int32_t regionX, int32_t regionZ)
	{
		return std::to_string(regionX) + "_" + std::to_string(regionZ) + ".saves";
	}

	std::string FormatSaveCounter(const SaveCounter& c)
	{
		std::ostringstream out;
		out << "; sc4-region-sync save counter\n";
		out << "count=" << c.count << '\n';
		out << "file=" << OneLine(c.file) << '\n';
		if (!c.prevFile.empty() && c.prevFile != c.file)
		{
			out << "prevFile=" << OneLine(c.prevFile) << '\n';
		}
		out << "lastSaveUnix=" << c.lastSaveUnix << '\n';
		out << "simDateNumber=" << c.simDateNumber << '\n';
		out << "population=" << c.population << '\n';
		out << "cityName=" << OneLine(c.cityName) << '\n';
		if (c.writer.Known() && IsValidWriterHost(c.writer.host))
		{
			out << "host=" << c.writer.host << '\n';
			out << "pid=" << c.writer.pid << '\n';
			out << "start=" << c.writer.start << '\n';
		}
		if (c.lastOk >= 0)
		{
			out << "lastOk=" << (c.lastOk ? 1 : 0) << '\n';
		}
		return out.str();
	}

	bool ReadSaveCounter(const std::filesystem::path& path, SaveCounter& c, std::string& error)
	{
		error.clear();
		c = SaveCounter{};
		std::error_code ec;
		const bool exists = std::filesystem::exists(path, ec);
		if (!ec && !exists)
		{
			return true;
		}
		KeyValues kv;
		if (!LoadKeyValues(path, kv, error))
		{
			return false;
		}
		if (!ReadKeyValue(kv, "count", c.count))
		{
			error = "No valid count in " + path.string();
			return false;
		}
		ParseSaveCounter(kv, c);
		return true;
	}

	void ParseSaveCounter(std::string_view text, SaveCounter& c)
	{
		KeyValues kv;
		ParseKeyValues(text, kv);
		ParseSaveCounter(kv, c);
	}

	void ParseSaveCounter(const KeyValues& kv, SaveCounter& c)
	{
		c = SaveCounter{};
		ReadKeyValue(kv, "count", c.count);
		c.file = Value(kv, "file");
		c.prevFile = Value(kv, "prevFile");
		ReadKeyValue(kv, "lastSaveUnix", c.lastSaveUnix);
		ReadKeyValue(kv, "simDateNumber", c.simDateNumber);
		ReadKeyValue(kv, "population", c.population);
		c.cityName = Value(kv, "cityName");
		if (const std::string host = Value(kv, "host"); IsValidWriterHost(host))
		{
			WriterIdentity w;
			w.host = host;
			if (ReadKeyValue(kv, "pid", w.pid) && ReadKeyValue(kv, "start", w.start) && w.Known())
			{
				c.writer = std::move(w);
			}
		}
		int32_t ok = -1;
		if (ReadKeyValue(kv, "lastOk", ok))
		{
			c.lastOk = ok != 0 ? 1 : 0;
		}
	}
}
