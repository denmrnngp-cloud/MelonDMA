# Makefile for MlxRDMA.dext (PCIDriverKit port of AppleMCX).
#
# Three build surfaces:
#   make check-host  — compile the portable IFC encoders + ABI headers as a
#                      host (userspace) binary, with ASan/UBSan. No DriverKit
#                      SDK needed. Mirrors AppleMCX's `make check-host`.
#   make check-dext  — generate iig headers and syntax-check all DEXT sources
#                      against the DriverKit SDK (no signing/loading needed).
#   make dext        — full compile + link into an unsigned Mach-O. Signing
#                      and packaging happen via codesign + pkgbuild after the
#                      PCIDriverKit entitlement lands (notes/21).
#
# check-host and check-dext run today, with no hardware and no Apple approval.

SDKROOT  ?= $(shell xcrun --sdk driverkit --show-sdk-path 2>/dev/null)
IIG      ?= $(shell xcrun --find iig 2>/dev/null)
HOST_CXX ?= clang++
CXX      ?= clang++

# 0 = release build (verbose debug logging compiled out), 1 = development.
#   make MLX_DEBUG=1 dext
MLX_DEBUG ?= 0

HOST_CFLAGS := -std=c++17 -Wall -Wextra -Werror -O0 -g \
               -fsanitize=address,undefined -fno-omit-frame-pointer
HOST_INCS  := -I Sources -I Sources/hw -I Sources/core -I Sources/ib \
              -I Sources/userclient

DEXT_INCS  := -I Sources -I Sources/hw -I Sources/core -I Sources/ib \
              -I Sources/userclient -I build/generated -I build/generated/MlxRDMA
DEXT_CFLAGS := -std=c++17 -arch arm64e -isysroot $(SDKROOT) $(DEXT_INCS) \
              -DMLX_DEBUG=$(MLX_DEBUG) \
              -fno-rtti -fno-exceptions \
              -Wno-inconsistent-missing-override -Wno-unused-private-field \
              -Wno-unused-function -Wno-unused-variable -Wno-sign-conversion
DEXT_SYNTAX := $(DEXT_CFLAGS) -fsyntax-only
DEXT_LDFLAGS := -F $(SDKROOT)/System/DriverKit/System/Library/Frameworks

IIG_CFLAGS := -x c++ -isysroot $(SDKROOT) -D__IIG=1 -Wno-inconsistent-missing-override $(DEXT_INCS)

PORTABLE_HDR := \
    Sources/hw/MlxIfcHelpers.hpp \
    Sources/hw/MlxRegs.hpp \
    Sources/hw/MlxWQE.hpp \
    Sources/hw/MlxDoorbell.hpp \
    Sources/hw/MlxP0Encoding.hpp \
    Sources/hw/mlx5_bits.h \
    Sources/userclient/MlxUCIO.h

IIG_SRCS   := Sources/MlxPCIDriver.iig Sources/MlxUserClient.iig
GEN_HDRS   := build/generated/MlxPCIDriver.h build/generated/MlxUserClient.h
DEXT_SRCS  := Sources/MlxPCIDriver.cpp Sources/MlxUserClient.cpp Sources/hw/MlxHCAConnectX4.cpp \
             Sources/core/MlxCmd.cpp Sources/core/MlxDMA.cpp Sources/core/MlxEQ.cpp \
             Sources/core/MlxFwPages.cpp Sources/core/MlxHealth.cpp Sources/core/MlxUAR.cpp \
             Sources/ib/MlxQP.cpp Sources/ib/MlxCQ.cpp Sources/ib/MlxMR.cpp \
             Sources/ib/MlxAH.cpp Sources/ib/MlxGID.cpp Sources/ib/MlxCC.cpp \
             Sources/ib/MlxRoCE.cpp
DEXT_HDRS  := $(shell find Sources -type f \( -name '*.h' -o -name '*.hpp' \))

SHIM_SRC   := usermode/librdma_shim/librdma_shim.c
SHIM_HDR   := usermode/librdma_shim/librdma_shim.h
VERBS_SRC  := usermode/libibverbs_compat/verbs_compat.c
VERBS_HDR  := usermode/libibverbs_compat/include/infiniband/verbs.h

.PHONY: all check-host check-dext check-plist check-iig dext sign app shim verbs-compat clean probe phase2-gate phase3-gate phase3-direct-uar-gate immediate-gate write-immediate-gate cq-event-gate r5-gate p0-gate stable-driver-gate production-preflight production-package p2-2-gate p3-gate p3-local-gate

all: check-host check-dext shim probe

# ---- host self-test: portable IFC encoders + bit helpers ----
check-host: build/mlx_host_tests build/mlx_shim_tests build/mlx_service_match_tests build/mlx_ibverbs_tests build/mlx_mkey_indirect_tests build/mlx_abi_properties_tests build/mlx_mkey_index_tests
	@echo "=== running host encoder tests ==="
	./build/mlx_host_tests
	@echo "=== running shim smoke tests ==="
	./build/mlx_shim_tests
	@echo "=== running service matching tests ==="
	./build/mlx_service_match_tests
	@echo "=== running ibverbs compatibility tests ==="
	./build/mlx_ibverbs_tests
	@echo "=== running ABI property tests ==="
	./build/mlx_abi_properties_tests
	@echo "=== running mkey hash-index tests ==="
	./build/mlx_mkey_index_tests

build/mlx_host_tests: Tests/test_all.cpp $(PORTABLE_HDR) | build
	$(HOST_CXX) $(HOST_CFLAGS) $(HOST_INCS) Tests/test_all.cpp -o $@

# test_shim links against the shim dylib built by `make shim`.
build/mlx_shim_tests: Tests/test_shim.c build/librdma_shim.dylib | build
	clang -std=c11 -Wall -Wextra -Werror -g \
	    -isysroot $(shell xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/librdma_shim -I Sources/userclient -I Sources/hw \
	    -L build -lrdma_shim -rpath $(CURDIR)/build \
	    Tests/test_shim.c -o $@

build/mlx_service_match_tests: Tests/test_service_match.c Sources/userclient/MlxServiceMatch.h | build
	clang -std=c11 -Wall -Wextra -Werror -g \
	    -isysroot $(shell xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I Sources/userclient -framework IOKit -framework CoreFoundation \
	    Tests/test_service_match.c -o $@

build/mlx_mkey_indirect_tests: Tests/test_mkey_indirect_standalone.cpp $(PORTABLE_HDR) Sources/hw/MlxP0EncodingIndirect.hpp | build
	$(HOST_CXX) $(HOST_CFLAGS) $(HOST_INCS) Tests/test_mkey_indirect_standalone.cpp -o $@

# P0.2: O(1) lkey/rkey hash index (no hardware, C11).
build/mlx_mkey_index_tests: Tests/test_mkey_index.c Sources/core/MlxKeyIndex.hpp | build
	clang -std=c11 -Wall -Wextra -Werror -g \
	    -isysroot $(shell xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I Sources/core \
	    Tests/test_mkey_index.c -o $@

# P2.2: ABI layout/selector/feature-bit contract (no hardware, C11).
build/mlx_abi_properties_tests: Tests/test_abi_properties.c Sources/userclient/MlxUCIO.h | build
	clang -std=c11 -Wall -Wextra -Werror -g \
	    -isysroot $(shell xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I Sources/userclient \
	    Tests/test_abi_properties.c -o $@

build/mlx_ibverbs_tests: Tests/test_ibverbs_compat.c build/libibverbs.dylib | build
	clang -std=c11 -Wall -Wextra -Werror -g \
	    -isysroot $(shell xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/libibverbs_compat/include \
	    -L build -libverbs -rpath $(CURDIR)/build \
	    Tests/test_ibverbs_compat.c -o $@

# ---- userspace probe tool (notes/35), builds with macOS SDK ----
# Signed with Apple Development using tools/reinit.entitlements
# (userclient-access). An ad-hoc signature (--sign -) does NOT work: the
# kernel rejects the com.apple.developer.* entitlement without a trusted
# Team ID, so IOServiceOpen returns kIOReturnNotPermitted (0xe00002e2).
build/mlx_probe: tools/mlx_probe.c Sources/userclient/MlxUCIO.h \
                 Sources/userclient/MlxServiceMatch.h tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O0 \
	    -isysroot $(shell xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I Sources/userclient -I Sources/hw \
	    -framework IOKit -framework CoreFoundation \
	    tools/mlx_probe.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@
	@echo "=== built mlx_probe: $@ (signed with $(SIGN_ID)) ==="

build/mlx_stable_gate: tools/mlx_stable_gate.c Sources/userclient/MlxUCIO.h \
                       Sources/userclient/MlxServiceMatch.h tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I Sources/userclient -framework IOKit -framework CoreFoundation \
	    tools/mlx_stable_gate.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

stable-driver-gate: build/mlx_stable_gate
	@chmod +x tools/run_stable_driver_gate.sh
	@echo "=== built stable-driver no-FLR reopen/explicit-TAKE gate ==="

production-preflight:
	@chmod +x scripts/mlx_production.sh
	./scripts/mlx_production.sh preflight

production-package:
	@chmod +x scripts/mlx_production.sh
	./scripts/mlx_production.sh package

# ---- DEXT iig header generation + syntax check against DriverKit SDK ----
check-plist:
	plutil -lint MlxRDMA.dext/Contents/Info.plist
	plutil -lint MlxRDMA.dext/Contents/MlxRDMA.entitlements
	@tag="$$(plutil -extract IOKitPersonalities.MlxPCIDriver.MlxBuildTag raw MlxRDMA.dext/Contents/Info.plist)"; \
	  test -n "$$tag" && grep -Fq "build $$tag" Sources/MlxPCIDriver.cpp || { \
	    echo "MlxBuildTag and the driver log tag are out of sync" >&2; \
	    exit 1; \
	}
	@test "$$(plutil -extract IOKitPersonalities.MlxPCIDriver.MlxRDMAUserClient.IOClass raw MlxRDMA.dext/Contents/Info.plist)" = "IOUserUserClient"
	@test "$$(plutil -extract IOKitPersonalities.MlxPCIDriver.MlxRDMAUserClient.IOUserClass raw MlxRDMA.dext/Contents/Info.plist)" = "MlxUserClient"
	@test "$$(plutil -extract IOKitPersonalities.MlxPCIDriver.MlxRDMAUserClient.IOServiceDEXTEntitlements.0 raw MlxRDMA.dext/Contents/Info.plist)" = "com.apple.developer.driverkit.userclient-access"
	@test "$$(/usr/libexec/PlistBuddy -c 'Print :com.apple.developer.driverkit.transport.pci:0:IOPCIMatch' MlxRDMA.dext/Contents/MlxRDMA.entitlements)" = "0x101515b3"
	@if /usr/libexec/PlistBuddy -c 'Print :com.apple.developer.driverkit.allow-any-userclient-access' \
	    MlxRDMA.dext/Contents/MlxRDMA.entitlements >/dev/null 2>&1; then \
	    echo "production DEXT must not allow arbitrary user clients" >&2; exit 1; \
	fi
	@if plutil -extract MlxRDMAUserClient xml1 -o - \
	    MlxRDMA.dext/Contents/Info.plist >/dev/null 2>&1; then \
	    echo "MlxRDMAUserClient must be a personality property for IOService::Create" >&2; \
	    exit 1; \
	fi

check-iig: $(GEN_HDRS)
	@if grep -Eq '^class[[:space:]]+KERNEL[[:space:]]+(MlxPCIDriver|MlxUserClient)' \
	    Sources/MlxPCIDriver.iig Sources/MlxUserClient.iig; then \
	    echo "DEXT implementation classes must not use the IIG KERNEL qualifier" >&2; \
	    exit 1; \
	fi
	@awk '/case IOService_NewUserClient_ID:/{ seen=1; next } seen && /#if KERNEL/{ exit 1 } seen && /NewUserClient_Invoke/{ exit 0 } END { if (!seen) exit 1 }' \
	    build/generated/MlxPCIDriver.cpp || { \
	    echo "NewUserClient is missing from the user-side generated dispatch" >&2; \
	    exit 1; \
	}
	@if grep -Eq 'IOService__NewUserClient_ID|_NewUserClient_Impl' \
	    build/generated/MlxPCIDriver.cpp build/generated/MlxPCIDriver.h; then \
	    echo "private _NewUserClient bridge must remain owned by IOService" >&2; \
	    exit 1; \
	fi
	@grep -Eq 'IOUserClient__ExternalMethod_ID' build/generated/MlxUserClient.cpp || { \
	    echo "MlxUserClient lost the inherited ExternalMethod IORPC route" >&2; \
	    exit 1; \
	}

check-dext: check-plist check-iig
	@echo "=== syntax-checking DEXT sources under DriverKit SDK ==="
	@status=0; for f in $(DEXT_SRCS); do \
	    log=$$(mktemp); \
	    if $(CXX) $(DEXT_SYNTAX) $$f >$$log 2>&1; then \
	        echo "  $$f -> ok"; \
	    else \
	        head -20 $$log; echo "  $$f -> FAILED"; status=1; \
	    fi; \
	    rm -f $$log; \
	done; exit $$status

# iig emits #include <MlxRDMA/*.h> (framework-name) from the generated impls.
build/generated/MlxPCIDriver.h: Sources/MlxPCIDriver.iig | build/generated build/generated/MlxRDMA
	$(IIG) --def Sources/MlxPCIDriver.iig --header $@ \
	    --impl build/generated/MlxPCIDriver.cpp --framework-name MlxRDMA \
	    --deployment-target 20.0 -- $(IIG_CFLAGS)
	ln -sf ../MlxPCIDriver.h build/generated/MlxRDMA/MlxPCIDriver.h

build/generated/MlxUserClient.h: Sources/MlxUserClient.iig | build/generated build/generated/MlxRDMA
	$(IIG) --def Sources/MlxUserClient.iig --header $@ \
	    --impl build/generated/MlxUserClient.cpp --framework-name MlxRDMA \
	    --deployment-target 20.0 -- $(IIG_CFLAGS)
	ln -sf ../MlxUserClient.h build/generated/MlxRDMA/MlxUserClient.h

# Each iig invocation emits the implementation together with its header.
# Explicit side-effect targets keep `make dext` correct after `make clean`.
build/generated/MlxPCIDriver.cpp: build/generated/MlxPCIDriver.h
	@test -f $@

build/generated/MlxUserClient.cpp: build/generated/MlxUserClient.h
	@test -f $@

build build/generated build/generated/MlxRDMA:
	mkdir -p $@

# ---- full DEXT link + bundle assembly ----
# SIP is disabled on this machine: with systemextensionsctl developer mode the
# dext can be signed locally and loaded without Apple's entitlement grant.
DEXT_IMPLS := build/generated/MlxPCIDriver.cpp build/generated/MlxUserClient.cpp
DEXT_BUNDLE := build/MlxRDMA.dext

build/MlxRDMA: $(DEXT_IMPLS) $(DEXT_SRCS) $(DEXT_HDRS) $(GEN_HDRS) | build
	$(CXX) $(DEXT_CFLAGS) $(DEXT_LDFLAGS) \
	    $(DEXT_IMPLS) $(DEXT_SRCS) \
	    -framework DriverKit -framework PCIDriverKit -o $@

dext: build/MlxRDMA | build
	rm -rf $(DEXT_BUNDLE)
	mkdir -p $(DEXT_BUNDLE)/Contents/MacOS
	cp build/MlxRDMA $(DEXT_BUNDLE)/Contents/MacOS/MlxRDMA
	cp MlxRDMA.dext/Contents/Info.plist $(DEXT_BUNDLE)/Contents/
	@echo "=== built $(DEXT_BUNDLE) (unsigned; make sign) ==="

# Local signing for developer-mode loading (SIP disabled).
SIGN_ID ?= Apple Development
sign: dext
	codesign --force --options runtime --timestamp=none \
	    --entitlements MlxRDMA.dext/Contents/MlxRDMA.entitlements \
	    -s "$(SIGN_ID)" $(DEXT_BUNDLE)
	codesign -dv $(DEXT_BUNDLE) 2>&1 | head -6

# Host app embedding the dext; activation requires an app using the
# SystemExtensions framework (systemextensionsctl install no longer exists).
APP := build/MlxRDMA.app

app: check-plist sign
	swiftc -O -framework SystemExtensions loader/mlx_activate.swift -o build/mlx_activate
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Library/SystemExtensions
	cp build/mlx_activate $(APP)/Contents/MacOS/mlx_activate
	cp loader/LoaderInfo.plist $(APP)/Contents/Info.plist
	# modern macOS scans *.systemextension (the .dext name is pre-Catalina);
	# sysextd requires the directory name to match CFBundleIdentifier
	cp -R $(DEXT_BUNDLE) $(APP)/Contents/Library/SystemExtensions/com.mlx5.rdma.dext.systemextension
	codesign --force --options runtime --timestamp=none \
	    --entitlements loader/Loader.entitlements -s "$(SIGN_ID)" $(APP)
	@echo "=== built $(APP) — run: $(APP)/Contents/MacOS/mlx_activate ==="

# ---- userspace shim (macOS SDK, no DriverKit) ----
shim: build/librdma_shim.dylib
	@echo "=== built librdma_shim.dylib ==="

verbs-compat: build/libibverbs.dylib
	@echo "=== built libibverbs.dylib compatibility layer ==="

probe: build/mlx_probe
	@echo "=== built mlx_probe ==="

build/mlx_phase2_gate: tools/mlx_phase2_gate.c build/librdma_shim.dylib \
                      usermode/librdma_shim/librdma_shim_diag.h \
                      tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/librdma_shim -I Sources/userclient -I Sources/hw \
	    -L build -lrdma_shim -rpath $(CURDIR)/build \
	    -framework IOKit -framework CoreFoundation \
	    tools/mlx_phase2_gate.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

phase2-gate: build/mlx_phase2_gate
	@echo "=== built Phase 2 hardware gate: $< ==="

# P0.2/P0.3 gates are self-contained (no librdma_shim) but still need the
# userclient-access entitlement to open the DEXT — an unsigned binary gets
# kIOReturnNotPermitted from IOServiceOpen.
build/mlx_isolation_gate: tools/mlx_isolation_gate.c tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I Sources/userclient -I Sources/hw \
	    -framework IOKit -framework CoreFoundation \
	    tools/mlx_isolation_gate.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

build/mlx_lifetime_gate: tools/mlx_lifetime_gate.c tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I Sources/userclient -I Sources/hw \
	    -framework IOKit -framework CoreFoundation \
	    tools/mlx_lifetime_gate.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

p0-2-gate: build/mlx_isolation_gate
	@echo "=== built P0.2 isolation gate: $< ==="

build/mlx_quota_gate: tools/mlx_quota_gate.c Sources/userclient/MlxUCIO.h \
                      Sources/userclient/MlxServiceMatch.h tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I Sources/userclient -I Sources/hw \
	    -framework IOKit -framework CoreFoundation \
	    tools/mlx_quota_gate.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

p1-1-gate: build/mlx_quota_gate
	@chmod +x tools/run_p1_1_quota_gate.sh
	@echo "=== built P1.1 per-client quota gate: $< ==="

build/mlx_abi_fuzz_gate: tools/mlx_abi_fuzz_gate.c Sources/userclient/MlxUCIO.h \
                         Sources/userclient/MlxServiceMatch.h tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I Sources/userclient -I Sources/hw \
	    -framework IOKit -framework CoreFoundation \
	    tools/mlx_abi_fuzz_gate.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

p2-2-gate: build/mlx_abi_fuzz_gate
	@chmod +x tools/run_p2_2_abi_fuzz_gate.sh
	@echo "=== built P2.2 ABI fuzz gate: $< ==="

build/mlx_soak_gate: tools/mlx_soak_gate.c Sources/userclient/MlxUCIO.h \
                     Sources/userclient/MlxServiceMatch.h tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I Sources/userclient -I Sources/hw \
	    -framework IOKit -framework CoreFoundation \
	    tools/mlx_soak_gate.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

p1-4-gate: build/mlx_soak_gate
	@chmod +x tools/run_p1_4_soak_gate.sh
	@echo "=== built P1.4 resource-counter soak gate: $< ==="

p0-3-gate: build/mlx_lifetime_gate
	@echo "=== built P0.3 lifetime gate: $< ==="

phase3-gate: build/mlx_phase2_gate
	@chmod +x tools/run_phase3_pipeline_gate.sh tools/run_phase3_write_gate.sh \
	    tools/run_phase3_multi_qp_gate.sh tools/run_phase3_direct_uar_gate.sh
	@echo "=== built Phase 3 pipeline/WRITE/direct-UAR gates ==="

phase3-direct-uar-gate: build/mlx_phase2_gate
	@chmod +x tools/run_phase3_direct_uar_gate.sh
	@echo "=== built Phase 3 direct-UAR gate ==="

immediate-gate: build/mlx_phase2_gate
	@chmod +x tools/run_immediate_gate.sh
	./tools/run_immediate_gate.sh

write-immediate-gate: build/mlx_phase2_gate
	@chmod +x tools/run_immediate_gate.sh
	R5_WRITE_IMMEDIATE=1 ./tools/run_immediate_gate.sh

build/mlx_cq_event_gate: tools/mlx_cq_event_gate.c build/libibverbs.dylib tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/libibverbs_compat/include -L build -libverbs \
	    -rpath $(CURDIR)/build -framework IOKit -framework CoreFoundation \
	    tools/mlx_cq_event_gate.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

cq-event-gate: build/mlx_cq_event_gate
	@chmod +x tools/run_cq_event_gate.sh
	./tools/run_cq_event_gate.sh

# P3: inline SEND + RC atomics + GID enumeration + CQ arming + QoS(SL).
build/mlx_p3_gate: tools/mlx_p3_gate.c build/libibverbs.dylib tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/libibverbs_compat/include -L build -libverbs \
	    -rpath $(CURDIR)/build -framework IOKit -framework CoreFoundation \
	    tools/mlx_p3_gate.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

build/mlx_p3_local_gate: tools/mlx_p3_local_gate.c build/librdma_shim.dylib \
                         tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/librdma_shim -L build -lrdma_shim \
	    -rpath $(CURDIR)/build -framework IOKit -framework CoreFoundation \
	    tools/mlx_p3_local_gate.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

p3-local-gate: build/mlx_p3_local_gate
	@chmod +x tools/run_p3_local_gate.sh
	@echo "=== built P3 local gate (caps/GID/arm/inline bounds) ==="

p3-gate: build/mlx_p3_gate
	@chmod +x tools/run_p3_gate.sh
	@echo "=== built P3 peer gate (inline + atomics + SL + solicited arm) ==="

p0-gate: check-host check-dext phase2-gate
	@chmod +x tools/run_p0_gate.sh
	./tools/run_p0_gate.sh

r5-gate: check-host check-dext phase2-gate
	@chmod +x tools/run_r5_gate.sh
	./tools/run_r5_gate.sh

# Convenience: rebuild everything (host tests + probe + dext + sign + app)
rebuild: clean check-host probe dext sign app
	@echo "=== rebuild complete ==="

build/librdma_shim.dylib: $(SHIM_SRC) $(SHIM_HDR) Sources/userclient/MlxUCIO.h \
                         Sources/userclient/MlxServiceMatch.h Sources/hw/MlxWQE.hpp | build
	clang -std=c11 -Wall -Wextra -Werror -dynamiclib \
	    -isysroot $(shell xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/librdma_shim -I Sources/userclient -I Sources/hw \
	    -framework IOKit -framework CoreFoundation \
	    -install_name @rpath/librdma_shim.dylib \
	    $(SHIM_SRC) -o $@

build/libibverbs.dylib: $(VERBS_SRC) $(VERBS_HDR) build/librdma_shim.dylib | build
	clang -std=c11 -Wall -Wextra -Werror -dynamiclib \
	    -isysroot $(shell xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/libibverbs_compat/include -I usermode/librdma_shim \
	    -L build -lrdma_shim -rpath $(CURDIR)/build \
	    -install_name @rpath/libibverbs.dylib \
	    $(VERBS_SRC) -o $@

clean:
	rm -rf build
