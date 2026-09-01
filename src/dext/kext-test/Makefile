# Makefile for MlxTestKext — standalone Phase-1a test kext (parallel to Apple dext)
#
# Usage:
#   make          # build build/MlxTestKext.kext (signed with dev cert)
#   make load     # sudo kmutil load
#   make unload   # sudo kmutil unload
#
# ponytail: no Xcode project, no dSYM, sign with the same Apple Development
# cert as the dext (accepted because SIP is disabled on this box).

KEXT       = MlxTestKext
BUNDLE_ID  = com.mlx5.test.kext
SDKROOT   := $(shell xcrun --show-sdk-path)
CERT       := $(shell security find-identity -v -p codesigning 2>/dev/null | grep "Apple Development" | head -1 | sed -E 's/.*"(.*)"/\1/')
BUILD      = build
KEXT_DIR   = $(BUILD)/$(KEXT).kext
CONT       = $(KEXT_DIR)/Contents

CXXFLAGS = -arch arm64e \
           -isysroot $(SDKROOT) \
           -std=c++17 \
           -Wall \
           -fno-exceptions -fno-rtti -fno-stack-protector \
           -fapple-kext -DKERNEL -DKERNEL_PRIVATE \
           -nostdinc \
           -I$(SDKROOT)/System/Library/Frameworks/Kernel.framework/Headers \
           -I. \
           -O2 -g

LDFLAGS = -arch arm64e \
          -mkernel \
          -isysroot $(SDKROOT) \
          -nostdlib \
          -lkmod -lkmodc++ \
          -Wl,-kext

.PHONY: all clean load unload log

all: $(CONT)/MacOS/$(KEXT)

$(CONT)/MacOS/$(KEXT): MlxTestKext.cpp
	@mkdir -p $(CONT)/MacOS
	clang++ $(CXXFLAGS) -c MlxTestKext.cpp -o $(BUILD)/MlxTestKext.o
	clang++ $(LDFLAGS) $(BUILD)/MlxTestKext.o -o $(CONT)/MacOS/$(KEXT)
	cp MlxTestKext.kext/Contents/Info.plist $(CONT)/Info.plist
	@if [ -n "$(CERT)" ]; then \
		codesign -s "$(CERT)" --force --timestamp=none $(KEXT_DIR); \
	else \
		codesign -s - --force --timestamp=none $(KEXT_DIR); \
	fi
	@echo "Built $(KEXT_DIR)"
	@echo "Cert: $(if $(CERT),$(CERT),ad-hoc)"

load:
	sudo kmutil load -p $(CURDIR)/$(KEXT_DIR)

unload:
	sudo kmutil unload -p $(CURDIR)/$(KEXT_DIR)

log:
	log show --last 5m --predicate 'eventMessage CONTAINS "MlxTestKext"' --info --style compact

clean:
	rm -rf $(BUILD)
