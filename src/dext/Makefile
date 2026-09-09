# Makefile for MlxRDMA.dext (PCIDriverKit port of AppleMCX).
#
# Two build surfaces:
#   make check-dext  — generate iig headers and syntax-check all DEXT sources
#                      against the DriverKit SDK (no signing/loading needed).
#   make dext        — full compile + link into an unsigned Mach-O. Signing
#                      and packaging happen via codesign + pkgbuild after the
#                      PCIDriverKit entitlement lands.
#
# Userspace libraries (macOS SDK, no DriverKit):
#   make shim        — librdma_shim.dylib (the IOUserClient bridge)
#   make verbs-compat — libibverbs.dylib (the verbs-compatible API)
#
# Diagnostic tools (the "ibverbs-utils" of MelonDMA):
#   make utils       — probe, perf test, port counters, benches, limits

SDKROOT  ?= $(shell xcrun --sdk driverkit --show-sdk-path 2>/dev/null)
IIG      ?= $(shell xcrun --find iig 2>/dev/null)
CXX      ?= clang++

# 0 = release build (verbose debug logging compiled out), 1 = development.
#   make MLX_DEBUG=1 dext
MLX_DEBUG ?= 0

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

IIG_SRCS   := Sources/MlxPCIDriver.iig Sources/MlxUserClient.iig
GEN_HDRS   := build/generated/MlxPCIDriver.h build/generated/MlxUserClient.h
DEXT_SRCS  := Sources/MlxPCIDriver.cpp Sources/MlxUserClient.cpp Sources/hw/MlxHCAConnectX4.cpp \
             Sources/core/MlxCmd.cpp Sources/core/MlxDMA.cpp Sources/core/MlxEQ.cpp \
             Sources/core/MlxFwPages.cpp Sources/core/MlxHealth.cpp Sources/core/MlxUAR.cpp \
             Sources/ib/MlxQP.cpp Sources/ib/MlxCQ.cpp Sources/ib/MlxMR.cpp \
             Sources/ib/MlxAH.cpp Sources/ib/MlxGID.cpp Sources/ib/MlxCC.cpp \
             Sources/ib/MlxSRQ.cpp Sources/ib/MlxRoCE.cpp
DEXT_HDRS  := $(shell find Sources -type f \( -name '*.h' -o -name '*.hpp' \))

SHIM_SRC   := usermode/librdma_shim/librdma_shim.c
SHIM_HDR   := usermode/librdma_shim/librdma_shim.h
VERBS_SRC  := usermode/libibverbs_compat/verbs_compat.c
VERBS_HDR  := usermode/libibverbs_compat/include/infiniband/verbs.h

UTILS := build/mlx_probe build/mlx_irq_probe build/mlx_perf_test \
         build/mlx_datapath_bench build/mlx_rtt_bench build/mlx_blocking_rtt \
         build/mlx_qp_scale build/mlx_port_counters build/mlx_gate_report

.PHONY: all check-dext check-plist check-iig dext sign app shim verbs-compat \
        clean probe utils datapath-bench rtt-bench blocking-rtt qp-scale \
        port-counters gate-report irq-probe perf-test \
        production-preflight production-package

all: check-dext shim verbs-compat utils

utils: $(UTILS)
	@echo "=== built diagnostic tools ==="

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
	cp -R $(DEXT_BUNDLE) $(APP)/Contents/Library/SystemExtensions/com.melondma.rdma.dext.systemextension
	codesign --force --options runtime --timestamp=none \
	    --entitlements loader/Loader.entitlements -s "$(SIGN_ID)" $(APP)
	@echo "=== built $(APP) — run: $(APP)/Contents/MacOS/mlx_activate ==="

# ---- userspace libraries (macOS SDK, no DriverKit) ----
shim: build/librdma_shim.dylib
	@echo "=== built librdma_shim.dylib ==="

verbs-compat: build/libibverbs.dylib
	@echo "=== built libibverbs.dylib compatibility layer ==="

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

# ---- diagnostic tools (the "ibverbs-utils" of MelonDMA) ----
# Signed with Apple Development: an ad-hoc signature (--sign -) does NOT work —
# the kernel rejects the com.apple.developer.* entitlement without a trusted
# Team ID, so IOServiceOpen returns kIOReturnNotPermitted (0xe00002e2).

# Device probe / status (the ibv_devinfo equivalent).
build/mlx_probe: tools/mlx_probe.c Sources/userclient/MlxUCIO.h \
                 Sources/userclient/MlxServiceMatch.h tools/diagnostic.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O0 \
	    -isysroot $(shell xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I Sources/userclient -I Sources/hw \
	    -framework IOKit -framework CoreFoundation \
	    tools/mlx_probe.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/diagnostic.entitlements $@

probe: build/mlx_probe
	@echo "=== built mlx_probe ==="

# Interrupt/vector probe: rebind the completion EQ to a chosen interrupt index.
build/mlx_irq_probe: tools/mlx_irq_probe.c build/librdma_shim.dylib tools/diagnostic.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/librdma_shim -L build -lrdma_shim \
	    -rpath $(CURDIR)/build -framework IOKit -framework CoreFoundation \
	    tools/mlx_irq_probe.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/diagnostic.entitlements $@

irq-probe: build/mlx_irq_probe
	@echo "=== built mlx_irq_probe ==="

# Perf test: QueryPerf/QueryInterrupts counters + the current EQ timer period.
build/mlx_perf_test: tools/mlx_perf_test.c build/libibverbs.dylib tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/libibverbs_compat/include -L build -libverbs \
	    -rpath $(CURDIR)/build -framework IOKit -framework CoreFoundation \
	    tools/mlx_perf_test.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

perf-test: build/mlx_perf_test
	@echo "=== built mlx_perf_test ==="

# Datapath baseline: per-message telemetry over a loopback RDMA WRITE.
build/mlx_datapath_bench: tools/mlx_datapath_bench.c build/libibverbs.dylib tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/libibverbs_compat/include -L build -libverbs \
	    -rpath $(CURDIR)/build -framework IOKit -framework CoreFoundation \
	    tools/mlx_datapath_bench.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

datapath-bench: build/mlx_datapath_bench
	@echo "=== built datapath baseline bench: $< ==="

# Direct-path latency with percentiles (the ibv_rc_pingpong equivalent).
build/mlx_rtt_bench: tools/mlx_rtt_bench.c build/libibverbs.dylib tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/libibverbs_compat/include -L build -libverbs \
	    -rpath $(CURDIR)/build -framework IOKit -framework CoreFoundation \
	    tools/mlx_rtt_bench.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

rtt-bench: build/mlx_rtt_bench
	@echo "=== built direct-path latency bench: $< ==="

# Blocking completion-path bench: arms the CQ and waits on the event.
build/mlx_blocking_rtt: tools/mlx_blocking_rtt.c build/libibverbs.dylib tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/libibverbs_compat/include -L build -libverbs \
	    -rpath $(CURDIR)/build -framework IOKit -framework CoreFoundation \
	    tools/mlx_blocking_rtt.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

blocking-rtt: build/mlx_blocking_rtt
	@echo "=== built blocking completion-path bench: $< ==="

# QP scaling in operations per second on a small message.
build/mlx_qp_scale: tools/mlx_qp_scale.c build/libibverbs.dylib tools/reinit.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/libibverbs_compat/include -L build -libverbs \
	    -rpath $(CURDIR)/build -framework IOKit -framework CoreFoundation \
	    tools/mlx_qp_scale.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/reinit.entitlements $@

qp-scale: build/mlx_qp_scale
	@echo "=== built QP scaling bench: $< ==="

# The card's own port counters: the only receive-side view of the wire when
# the DEXT owns the port and no netif exists. --pcie adds MPEIN.
build/mlx_port_counters: tools/mlx_port_counters.c build/librdma_shim.dylib tools/diagnostic.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/librdma_shim -L build -lrdma_shim \
	    -rpath $(CURDIR)/build -framework IOKit -framework CoreFoundation \
	    tools/mlx_port_counters.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/diagnostic.entitlements $@

port-counters: build/mlx_port_counters
	@echo "=== built port counter reader: $< ==="

# Unified report: port counters, PCIe line, per-op counters and DCQCN params.
build/mlx_gate_report: tools/mlx_gate_report.c build/librdma_shim.dylib tools/diagnostic.entitlements | build
	clang -std=c11 -Wall -Wextra -Werror -g -O2 \
	    -isysroot $$(xcrun --sdk macosx --show-sdk-path) -arch arm64 \
	    -I usermode/librdma_shim -L build -lrdma_shim \
	    -rpath $(CURDIR)/build -framework IOKit -framework CoreFoundation \
	    tools/mlx_gate_report.c -o $@
	codesign --force --options runtime --timestamp=none --sign "$(SIGN_ID)" \
	    --entitlements tools/diagnostic.entitlements $@

gate-report: build/mlx_gate_report
	@echo "=== built unified gate report: $< ==="

clean:
	rm -rf build
