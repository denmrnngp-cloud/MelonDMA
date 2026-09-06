# Production release notes

Dated log of changes that must land in the **signed, notarized production
package** (`dist/MlxRDMA.pkg`) before the next release. Each entry is a
durable requirement, not a one-off note: keep it here until it is actually
implemented and verified in `mlx_production.sh`.

---

## 2026-09-02 — Driver boot-time startup (automatic card takeover at boot)

**Status:** implemented on the dev path, **not yet wired into the production
package**. Must be added before the next release.

### What changed

The DEXT was only taking the ConnectX card after a manual `takeover` (kill
Apple's `AppleEthernetMLX5` + `IOServiceRequestProbe` rematch). The intended
boot-time mechanism is kernelmanagerd's **guess-scan**: when
`DextRecordTable` is empty at boot, kernelmanagerd scans
`/Library/DriverExtensions` for **flat `.dext` bundles** (the layout Apple's
own system DEXTs use — `Info.plist` + the executable at the bundle root, no
`Contents/`), re-registers the persona **before** card matching, and the DEXT
wins the first match on `IOProbeScore` (5000 vs Apple's 1000).

Two fixes make this work:

1. **`src/dext/scripts/install-to-libde.sh`** now flattens the activated
   `.systemextension` (which is `Contents/`-based) into the flat layout
   `/Library/DriverExtensions/com.mlx5.rdma.dext.dext/{Info.plist, MlxRDMA}`.
   Before this fix kernelmanagerd logged `No dexts were found in
   /Library/DriverExtensions` and the persona never registered at boot.
2. **`src/dext/scripts/mlx_cold_takeover.sh`** stages the flat `.dext` before
   reboot (`prepare`) and refreshes it after a successful takeover
   (`resume` / `finalize`), so the boot recovery path stays populated.

### What must be added to the production release

- The production **installer/pkg** must place the flat `.dext` bundle at
  `/Library/DriverExtensions/com.mlx5.rdma.dext.dext/` (or the production
  loader must run the equivalent flatten step post-install), so that a cold
  reboot after install captures the card with no manual steps.
- A production **verification gate**: after `installer -pkg` + reboot, assert
  `ioreg -r -n ethernet@0` shows `MlxPCIDriver` as owner and
  `IODEXTMatchCount >= 2` **without** running `takeover`.
- Note: the dev path still relies on `boot-args` (`dextrelaunch=1`,
  `daily_max_dext_crashes=1000`) and ad-hoc `iocat` injection as fallback.
  Those are **dev-only** and must not ship; the production equivalent is the
  flat `.dext` + guess-scan (no SIP changes, no boot-args).

### Files touched (dev)

- `src/dext/scripts/install-to-libde.sh`
- `src/dext/scripts/mlx_cold_takeover.sh`

---

## 2026-09-05 — Local readiness and evidence update

**Status:** local/developer work complete; Apple-controlled production gates remain pending.

Completed locally: final-KV one-sided RDMA WRITE now emits `GGML_RPC_FINAL_DEST_ACTIVE ... rdma_write=1 trailing_copy=0`; live Metal negative cases reject `.private` and invalid buffers; `METAL_DMA_GATE PASS` covers GPU-to-peer, peer-to-Metal and 4 MiB indirect MR; the direct-UAR gate uses current `DIRECT_UAR_STATS`; CQ quota passes without a speculative change; and `SUPPORTED_HARDWARE.md` limits release claims to ConnectX-4 Lx PF `15b3:1015`.

Added workflows:

- `scripts/mlx_developer_package.sh` — local flat DEXT artifact, explicitly not production signed/notarized.
- `scripts/mlx_release_evidence.sh` — build, hardware, PCI, peer, network, DEXT and gate evidence with checksums.
- `docs/implementation-update-2026-09-05.md` — complete change log and verification record.
- `/Users/macstudio/llama.cpp/RDMA_BENCH_RECOMMENDATIONS.md` — reproducible RDMA benchmark contract.

Verification record:

```text
host-check=0
dext-check=0
metal-contract=0
quota=0
direct-uar=0
metal-live=0
```

Still required before production release: Apple Developer ID signatures, granted DriverKit/system-extension profiles, notarization, SIP-on clean-machine installation, privileged no-FLR/FLR recovery, destructive lifecycle matrix, and dedicated completion MSI-X proof. The production script must continue to fail closed when these are absent.
