/*
 * sc4-region-sync launcher
 *
 * Status.cpp - status file reading. The read is hand-written because ifstream
 * cannot open with FILE_SHARE_DELETE; parsing is the plugin's ParseInstanceStatus.
 */

#include "Launcher.h"
#include "InstanceStatus.h"
#include <cwctype>

namespace launcher
{
	bool ReadSharedFile(const std::wstring& path, std::string& out, size_t cap)
	{
		out.clear();
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
		while (true)
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
			if (out.size() + read > cap)
			{
				ok = false;  // over the cap: never a valid file
				break;
			}
			out.append(buffer, read);
		}
		CloseHandle(file);
		return ok;
	}

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
		if (!ReadSharedFile(path, text, 65536))
		{
			return false;
		}
		regionsync::KeyValues kv;
		regionsync::ParseKeyValues(text, kv);
		regionsync::InstanceStatus p;
		regionsync::ParseInstanceStatus(kv, p);
		if (kv["schemaVersion"] != "1" || !p.writer.Known())
		{
			return false;
		}
		InstanceStatus s;
		s.fileName = path.substr(path.find_last_of(L'\\') + 1);
		s.host = p.writer.host;
		s.pid = p.writer.pid;
		s.start = p.writer.start;
		s.userDir = Widen(p.userDir);
		s.state = Widen(p.state);
		s.region = Widen(p.region);
		s.regionX = p.regionX;
		s.regionZ = p.regionZ;
		s.cityName = Widen(p.cityName);
		s.conflict = p.conflict;
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
