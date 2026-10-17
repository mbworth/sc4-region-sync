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
CLANG_CL ?= clang-cl
LLVM_RC  ?= llvm-rc

GZCOM = vendor/gzcom-dll/gzcom-dll
UTIL  = vendor/sc4-dll-utilities/sc4-dll-utilities
WIL   = vendor/wil

SRCS = \
	src/NeighborSnapshot.cpp \
	src/RegionalCityBridge.cpp \
	src/SyncSettings.cpp \
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

.PHONY: all release debug clean check-toolchain

all: release

check-toolchain:
	@test -d $(XWIN)/crt/include || (echo "xwin splat not found at $(XWIN). Run scripts/setup-toolchain.sh" && exit 1)
	@command -v $(CLANG_CL) >/dev/null || (echo "$(CLANG_CL) not found. Install clang + lld (apt: clang lld llvm)" && exit 1)
	@command -v lld-link >/dev/null || (echo "lld-link not found. Install lld" && exit 1)
	@command -v $(LLVM_RC) >/dev/null || (echo "$(LLVM_RC) not found. Install llvm" && exit 1)

$(BUILD):
	mkdir -p $(BUILD)

# version.rc is UTF-16 with a BOM; llvm-rc handles that directly.
$(RES): src/version.rc src/version.h src/resource.h | $(BUILD)
	$(LLVM_RC) /I src /I $(XWIN)/sdk/include/um /I $(XWIN)/sdk/include/shared /FO $@ src/version.rc

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
