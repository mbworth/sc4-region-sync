# Cross-compile SC4RegionSync.dll on Linux with clang-cl + lld-link against
# the MSVC CRT and Windows SDK fetched by xwin. Same approach the NAM team
# uses for NAM.dll. Run scripts/setup-toolchain.sh once first.
#
#   make            -> build/SC4RegionSync.dll (Release)
#   make debug      -> build/SC4RegionSync.debug.dll
#   make launcher   -> build/SC4RegionSyncLauncher.exe
#   make clean
#
# Override XWIN if you put the splat somewhere else:
#   make XWIN=/opt/xwin

XWIN     ?= vendor/xwin
BUILD    ?= build

# LLVM 19+ is required by the MSVC STL that xwin fetches. If LLVM is not on
# PATH (no root on the box), scripts/setup-toolchain.sh --prebuilt installs
# it under ~/.local/llvm-20 and this picks it up automatically.
LLVM     ?= $(firstword $(wildcard $(HOME)/.local/llvm-20 $(HOME)/.local/llvm-19))
ifneq ($(LLVM),)
CLANG_CL ?= $(LLVM)/bin/clang-cl
LLVM_RC  ?= $(LLVM)/bin/llvm-rc
LLD_LINK ?= $(LLVM)/bin/lld-link
export PATH := $(LLVM)/bin:$(PATH)
export LD_LIBRARY_PATH := $(LLVM)/shimlib:$(LD_LIBRARY_PATH)
endif
CLANG_CL ?= clang-cl
LLVM_RC  ?= llvm-rc

GZCOM = vendor/gzcom-dll/gzcom-dll
UTIL  = vendor/sc4-dll-utilities/sc4-dll-utilities
WIL   = vendor/wil

SRCS = \
	src/NeighborSnapshot.cpp \
	src/InstanceStatus.cpp \
	src/RegionalCityBridge.cpp \
	src/NeighborMirror.cpp \
	src/EdgeNetwork.cpp \
	src/SyncSettings.cpp \
	src/InstanceMutex.cpp \
	src/WriterIdentity.cpp \
	src/SaveLock.cpp \
	src/TileLock.cpp \
	src/SaveHook641.cpp \
	src/RegionSyncDllDirector.cpp \
	$(GZCOM)/src/cRZBaseString.cpp \
	$(GZCOM)/src/cRZBaseUnknown.cpp \
	$(GZCOM)/src/cRZCellMap.cpp \
	$(GZCOM)/src/cRZCOMDllDirector.cpp \
	$(GZCOM)/src/cSC4BaseOccupantFilter.cpp \
	$(GZCOM)/src/StringResourceManager.cpp \
	$(UTIL)/src/DebugUtil.cpp \
	$(UTIL)/src/FileSystem.cpp \
	$(UTIL)/src/Logger.cpp \
	$(UTIL)/src/SC4NotificationDialog.cpp \
	$(UTIL)/src/SC4VersionDetection.cpp \
	$(UTIL)/src/StringConversion.cpp \
	$(UTIL)/src/StringViewUtil.cpp \
	$(UTIL)/src/Utf8TextOFStream.cpp

INCLUDES = /I src /I $(GZCOM)/include /I $(UTIL)/include /I $(WIL)/include

MSVC_INCLUDES = \
	/imsvc $(XWIN)/crt/include \
	/imsvc $(XWIN)/sdk/include/ucrt \
	/imsvc $(XWIN)/sdk/include/um \
	/imsvc $(XWIN)/sdk/include/shared

MSVC_LIBPATHS = \
	/libpath:$(XWIN)/crt/lib/x86 \
	/libpath:$(XWIN)/sdk/lib/um/x86 \
	/libpath:$(XWIN)/sdk/lib/ucrt/x86

COMMON_FLAGS = -target i386-pc-windows-msvc -fuse-ld=lld-link -ferror-limit=20 \
	-Wno-inconsistent-missing-override -Wno-microsoft-include \
	/std:c++20 /EHsc /LD /W3 /Gy /Gd \
	/D _WIN32 /D WIN32 /D _UNICODE /D UNICODE /D _USRDLL /D _WINDLL \
	/D _CRT_SECURE_NO_WARNINGS /D SC4REGIONSYNC_EXPORTS \
	$(INCLUDES) $(MSVC_INCLUDES)

RELEASE_FLAGS = /MD /O2 /Oi /D NDEBUG
DEBUG_FLAGS   = /MDd /Od /Zi /D _DEBUG

LIBS = version.lib ole32.lib user32.lib

RES = $(BUILD)/version.res

.PHONY: all release debug launcher clean check-toolchain test deploy logs

all: release

check-toolchain:
	@test -d $(XWIN)/crt/include || (echo "xwin splat not found at $(XWIN). Run scripts/setup-toolchain.sh" && exit 1)
	@command -v $(CLANG_CL) >/dev/null || (echo "$(CLANG_CL) not found. Install clang + lld (apt: clang lld llvm) or run scripts/setup-toolchain.sh --prebuilt" && exit 1)
	@command -v lld-link >/dev/null || (echo "lld-link not found. Install lld" && exit 1)
	@command -v $(LLVM_RC) >/dev/null || (echo "$(LLVM_RC) not found. Install llvm" && exit 1)

$(BUILD):
	mkdir -p $(BUILD)

# version.rc is UTF-16 with a BOM (MSVC rc wants that). llvm-rc's clang
# preprocessor does not read UTF-16, so compile a UTF-8 copy from build/.
$(RES): src/version.rc src/version.h src/resource.h | $(BUILD)
	iconv -f UTF-16 -t UTF-8 src/version.rc > $(BUILD)/version.rc
	$(LLVM_RC) /I src /I $(XWIN)/sdk/include/um /I $(XWIN)/sdk/include/shared /FO $@ $(BUILD)/version.rc

release: check-toolchain $(RES)
	$(CLANG_CL) $(COMMON_FLAGS) $(RELEASE_FLAGS) /o $(BUILD)/SC4RegionSync.dll $(SRCS) \
		/link $(MSVC_LIBPATHS) $(LIBS) $(RES) /DLL /OUT:$(BUILD)/SC4RegionSync.dll
	cp src/SC4RegionSync.ini $(BUILD)/
	@echo "Built $(BUILD)/SC4RegionSync.dll"

debug: check-toolchain $(RES)
	$(CLANG_CL) $(COMMON_FLAGS) $(DEBUG_FLAGS) /o $(BUILD)/SC4RegionSync.debug.dll $(SRCS) \
		/link $(MSVC_LIBPATHS) $(LIBS) $(RES) /DLL /DEBUG /OUT:$(BUILD)/SC4RegionSync.debug.dll
	@echo "Built $(BUILD)/SC4RegionSync.debug.dll"

# Launcher: a plain Win32 exe, static CRT, its own flags (no /LD, no plugin defines).
LAUNCHER_SRCS = launcher/main.cpp launcher/Status.cpp launcher/Settings.cpp \
	launcher/Resources.cpp launcher/Launch.cpp src/WriterIdentity.cpp
LAUNCHER_FLAGS = -target i386-pc-windows-msvc -fuse-ld=lld-link -ferror-limit=20 \
	/std:c++20 /EHsc /W3 /Gy /Gd /MT /O2 /Oi \
	/D WIN32 /D _UNICODE /D UNICODE /D NDEBUG /D NOMINMAX \
	/D _WIN32_WINNT=0x0A00 /D WINVER=0x0A00 \
	/I launcher /I src $(MSVC_INCLUDES)
LAUNCHER_LIBS = kernel32.lib user32.lib gdi32.lib comctl32.lib shell32.lib psapi.lib dwmapi.lib
LAUNCHER_RES = $(BUILD)/launcher.res
LAUNCHER_EXE = $(BUILD)/SC4RegionSyncLauncher.exe

# launcher.rc is UTF-8, so no iconv step.
$(LAUNCHER_RES): launcher/launcher.rc launcher/launcher.manifest launcher/resource.h src/version.h | $(BUILD)
	$(LLVM_RC) /I launcher /I src /FO $@ launcher/launcher.rc

launcher: check-toolchain $(LAUNCHER_RES)
	$(CLANG_CL) $(LAUNCHER_FLAGS) /o $(LAUNCHER_EXE) $(LAUNCHER_SRCS) \
		/link $(MSVC_LIBPATHS) $(LAUNCHER_LIBS) $(LAUNCHER_RES) /SUBSYSTEM:WINDOWS /OUT:$(LAUNCHER_EXE)
	@echo "Built $(LAUNCHER_EXE)"

clean:
	rm -rf $(BUILD) src/*.obj *.obj

# Host-side unit test for the game-free snapshot and status formats.
test:
	mkdir -p $(BUILD)
	g++ -std=c++20 -Wall -I src tests/snapshot_roundtrip.cpp src/NeighborSnapshot.cpp src/InstanceStatus.cpp -o $(BUILD)/snapshot_roundtrip
	$(BUILD)/snapshot_roundtrip $(BUILD)/snapshot-test

# ---------------------------------------------------------------------------
# Windows game box over SSH (OpenSSH server on the Windows side, key auth).
#   make deploy   -> copy build/ DLL + INI into the game's Documents Plugins
#   make logs     -> pull each instance's SC4RegionSync.log into ingame_tests/
SC4_HOST    ?= user@gamebox
SC4_PLUGINS ?= C:/Users/player/Documents/SimCity 4/Plugins
SC4_INSTANCES ?= C:/Users/player/Documents/SimCity 4 Instances
SC4_LAUNCHER ?= C:/Users/player/SC4RegionSyncLauncher

deploy: release launcher
	@ssh -o BatchMode=yes $(SC4_HOST) 'if (Get-Process "SimCity 4" -ErrorAction SilentlyContinue) { Write-Error "SimCity 4 is running; close it first"; exit 1 }; if (Get-Process "SC4RegionSyncLauncher" -ErrorAction SilentlyContinue) { Write-Error "SC4RegionSyncLauncher is running; close it first"; exit 1 }'
	scp -o BatchMode=yes -q $(BUILD)/SC4RegionSync.dll $(BUILD)/SC4RegionSync.ini "$(SC4_HOST):$(SC4_PLUGINS)/"
	@echo "Deployed to $(SC4_HOST):$(SC4_PLUGINS)"
	@ssh -o BatchMode=yes $(SC4_HOST) 'New-Item -ItemType Directory -Force -Path "$(SC4_LAUNCHER)" | Out-Null'
	scp -o BatchMode=yes -q $(LAUNCHER_EXE) "$(SC4_HOST):$(SC4_LAUNCHER)/"
	@echo "Deployed launcher to $(SC4_HOST):$(SC4_LAUNCHER)"

logs:
	@mkdir -p ingame_tests
	@# scp from this box truncates at 200 KB, so the log travels as base64 over ssh.
	@for i in 1 2 3; do ssh -o BatchMode=yes "$(SC4_HOST)" "powershell -NoProfile -Command \"[Convert]::ToBase64String([IO.File]::ReadAllBytes('$(SC4_INSTANCES)/Instance$$i/SC4RegionSync.log'))\"" 2>/dev/null | tr -d '\r\n' | base64 -d > ingame_tests/instance$$i.log.tmp && [ -s ingame_tests/instance$$i.log.tmp ] && mv ingame_tests/instance$$i.log.tmp ingame_tests/instance$$i.log && echo "ingame_tests/instance$$i.log ($$(wc -c < ingame_tests/instance$$i.log) bytes)" || { rm -f ingame_tests/instance$$i.log.tmp; echo "Instance$$i: no log"; }; done
