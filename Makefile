# Cross-compile SC4RegionSync.dll on Linux with clang-cl + lld-link against
# the MSVC CRT and Windows SDK fetched by xwin. Same approach the NAM team
# uses for NAM.dll. Run scripts/setup-toolchain.sh once first.
#
#   make            -> build/SC4RegionSync.dll (Release)
#   make debug      -> build/SC4RegionSync.debug.dll
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
	src/RegionalCityBridge.cpp \
	src/NeighborMirror.cpp \
	src/SyncSettings.cpp \
	src/InstanceMutex.cpp \
	src/RegionSyncDllDirector.cpp \
	$(GZCOM)/src/cRZBaseString.cpp \
	$(GZCOM)/src/cRZCOMDllDirector.cpp \
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

.PHONY: all release debug clean check-toolchain test deploy logs

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

clean:
	rm -rf $(BUILD) src/*.obj *.obj

# Host-side unit test for the game-free snapshot format.
test:
	mkdir -p $(BUILD)
	g++ -std=c++20 -Wall -I src tests/snapshot_roundtrip.cpp src/NeighborSnapshot.cpp -o $(BUILD)/snapshot_roundtrip
	$(BUILD)/snapshot_roundtrip $(BUILD)/snapshot-test

# ---------------------------------------------------------------------------
# Windows game box over SSH (OpenSSH server on the Windows side, key auth).
#   make deploy   -> copy build/ DLL + INI into the game's Documents Plugins
#   make logs     -> pull each instance's SC4RegionSync.log into ingame_tests/
SC4_HOST    ?= user@gamebox
SC4_PLUGINS ?= C:/Users/player/Documents/SimCity 4/Plugins
SC4_INSTANCES ?= C:/Users/player/Documents/SimCity 4 Instances

deploy: release
	@ssh -o BatchMode=yes $(SC4_HOST) 'if (Get-Process "SimCity 4" -ErrorAction SilentlyContinue) { Write-Error "SimCity 4 is running; close it first"; exit 1 }'
	scp -o BatchMode=yes -q $(BUILD)/SC4RegionSync.dll $(BUILD)/SC4RegionSync.ini "$(SC4_HOST):$(SC4_PLUGINS)/"
	@echo "Deployed to $(SC4_HOST):$(SC4_PLUGINS)"

logs:
	@mkdir -p ingame_tests
	@for i in 1 2; do scp -o BatchMode=yes -q "$(SC4_HOST):$(SC4_INSTANCES)/Instance$$i/SC4RegionSync.log" ingame_tests/instance$$i.log 2>/dev/null && echo "ingame_tests/instance$$i.log" || echo "Instance$$i: no log"; done
