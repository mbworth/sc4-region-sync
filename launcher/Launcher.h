/*
 * sc4-region-sync launcher
 *
 * Launcher.h - shared types for the launcher window, settings, status
 * discovery, resource sampling and launch.
 */

#pragma once
#include "WriterIdentity.h"
#include <Windows.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace launcher
{
	inline constexpr wchar_t kDefaultWindowClass[] = L"GDriverClass--DirectX";

	struct Settings
	{
		std::wstring gameExe;
		std::wstring userDirRoot;
		std::wstring statusDirectory;
		std::wstring extraArgs;  // appended verbatim
		int width = 1600;
		int height = 900;
		int depth = 32;
		bool windowed = true;
		bool skipIntro = true;
		bool singleCpu = false;
		std::wstring renderer = L"DirectX";  // DirectX, Software or OpenGL
		std::wstring windowClass = kDefaultWindowClass;  // the game's main window (Q14); empty = any class
		int perInstanceRamMb = 1536;
		int reserveCores = 1;
		int reserveRamMb = 2048;
		int maxInstances = 16;
	};

	// launcher.ini next to the exe.
	std::wstring IniPath();
	std::wstring DocumentsFolder();

	// Creates launcher.ini next to the exe with defaults when missing.
	Settings LoadSettings();

	// DirectX, Software or OpenGL, matched ignoring case; anything else is DirectX.
	const wchar_t* RendererName(const std::wstring& renderer);

	// Modal Settings dialog. True when OK wrote launcher.ini.
	bool EditSettings(HWND owner, const Settings& current);

	// One status file, as written by the plugin (plan A1).
	struct InstanceStatus
	{
		std::wstring fileName;
		std::string host;
		DWORD pid = 0;
		uint64_t start = 0;
		std::wstring userDir;
		std::wstring state;
		std::wstring region;
		int regionX = 0;
		int regionZ = 0;
		std::wstring cityName;
		bool conflict = false;
	};

	// Whole file with full sharing (the plugin renames over it). False on a
	// read error or when it is larger than cap.
	bool ReadSharedFile(const std::wstring& path, std::string& out, size_t cap);

	// False when the file cannot be opened, is not schemaVersion 1, or lacks
	// host, pid or start.
	bool ReadStatusFile(const std::wstring& path, InstanceStatus& out);
	regionsync::writer::Liveness IsAlive(const InstanceStatus& s);

	// Full long path, backslashes, no trailing backslash, lower case.
	std::wstring NormalizeDir(const std::wstring& dir);

	struct ResourceSample
	{
		enum class Colour { Green, Amber, Red };
		Colour colour = Colour::Green;
		int running = 0;
		int memoryRoom = 0;
		int cpuRoom = 0;
		int capRoom = 0;
		int room = 0;
		std::wstring text;
		std::wstring tooltip;
	};

	// Called every 5 s with the pids of running instances.
	ResourceSample SampleResources(const Settings& settings, const std::vector<DWORD>& pids);

	struct PendingLaunch
	{
		DWORD pid = 0;
		HANDLE process = nullptr;
		ULONGLONG startedTick = 0;
		bool timedOut = false;  // Launch unblocked; the folder stays reserved until exit
		std::wstring label;
	};

	// Normalised user dir -> pending launch.
	using PendingMap = std::map<std::wstring, PendingLaunch>;

	// A launch not yet reported and not timed out blocks the next one.
	bool LaunchInFlight(const PendingMap& pending);

	// Checks the preconditions, starts the game and records the pending
	// launch. Shows a message and returns false on any failure.
	bool Launch(HWND owner, const Settings& settings, const ResourceSample& resources,
		int running, const std::vector<std::wstring>& liveUserDirs, PendingMap& pending);

	void ShowMessage(HWND owner, const std::wstring& text, UINT icon = MB_ICONWARNING);
	std::wstring Widen(const std::string& utf8);
}
