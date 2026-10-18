/*
 * sc4-region-sync launcher
 *
 * Status.cpp - status file parsing. Same rules as LoadKeyValues in
 * src/NeighborSnapshot.cpp, hand-written because ifstream cannot open with
 * FILE_SHARE_DELETE.
 */

#include "Launcher.h"
#include <charconv>
#include <cwctype>

namespace
{
	template <typename T>
	bool ParseNumber(const std::string& text, T& out)
	{
		const char* first = text.data();
		const char* last = first + text.size();
		const auto [ptr, ec] = std::from_chars(first, last, out);
		return ec == std::errc() && ptr == last;
	}

	bool ReadWholeFile(const std::wstring& path, std::string& out)
	{
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		char buffer[4096];
		DWORD read = 0;
		bool ok = true;
		while (out.size() < 65536)
		{
			if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr))
			{
				ok = false;
				break;
			}
			if (read == 0)
			{
				break;
			}
			out.append(buffer, read);
		}
		CloseHandle(file);
		return ok;
	}

	std::map<std::string, std::string> ParseKeyValues(const std::string& text)
	{
		std::map<std::string, std::string> kv;
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t end = text.find('\n', pos);
			if (end == std::string::npos)
			{
				end = text.size();
			}
			std::string line = text.substr(pos, end - pos);
			pos = end + 1;
			if (!line.empty() && line.back() == '\r')
			{
				line.pop_back();
			}
			if (line.empty() || line[0] == ';' || line[0] == '#')
			{
				continue;
			}
			const size_t eq = line.find('=');
			if (eq == std::string::npos)
			{
				continue;
			}
			kv[line.substr(0, eq)] = line.substr(eq + 1);
		}
		return kv;
	}
}

namespace launcher
{
	std::wstring Widen(const std::string& utf8)
	{
		if (utf8.empty())
		{
			return {};
		}
		const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
		if (n <= 0)
		{
			return {};
		}
		std::wstring out(static_cast<size_t>(n), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
		return out;
	}

	std::wstring NormalizeDir(const std::wstring& dir)
	{
		std::wstring out = dir;
		for (wchar_t& c : out)
		{
			if (c == L'/') c = L'\\';
		}
		if (!out.empty())
		{
			wchar_t full[MAX_PATH]{};
			const DWORD n = GetFullPathNameW(out.c_str(), MAX_PATH, full, nullptr);
			if (n > 0 && n < MAX_PATH)
			{
				out = full;
			}
			wchar_t longPath[MAX_PATH]{};
			const DWORD m = GetLongPathNameW(out.c_str(), longPath, MAX_PATH);
			if (m > 0 && m < MAX_PATH)
			{
				out = longPath;
			}
		}
		while (!out.empty() && (out.back() == L'\\' || out.back() == L'/'))
		{
			out.pop_back();
		}
		for (wchar_t& c : out)
		{
			c = static_cast<wchar_t>(std::towlower(c));
		}
		return out;
	}

	bool ReadStatusFile(const std::wstring& path, InstanceStatus& out)
	{
		std::string text;
		if (!ReadWholeFile(path, text))
		{
			return false;
		}
		const auto kv = ParseKeyValues(text);
		const auto get = [&kv](const char* key) -> std::string
		{
			const auto it = kv.find(key);
			return it == kv.end() ? std::string() : it->second;
		};

		InstanceStatus s;
		s.host = get("host");
		if (get("schemaVersion") != "1" || !regionsync::IsValidWriterHost(s.host)
			|| !ParseNumber(get("pid"), s.pid) || !ParseNumber(get("start"), s.start) || s.pid == 0 || s.start == 0)
		{
			return false;
		}
		s.userDir = Widen(get("userDir"));
		s.state = Widen(get("state"));
		s.region = Widen(get("region"));
		ParseNumber(get("regionX"), s.regionX);
		ParseNumber(get("regionZ"), s.regionZ);
		s.cityName = Widen(get("cityName"));
		s.conflict = get("conflict") == "1";
		out = std::move(s);
		return true;
	}

	regionsync::writer::Liveness IsAlive(const InstanceStatus& s)
	{
		regionsync::WriterIdentity id;
		id.host = s.host;
		id.pid = s.pid;
		id.start = s.start;
		return regionsync::writer::IsAlive(id);
	}
}
