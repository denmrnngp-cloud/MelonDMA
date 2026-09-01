# Third-party notices

MelonDMA is licensed as a whole under **GPL-2.0** ([LICENSE](LICENSE)).
This file documents exactly what the code is derived from, and what is
deliberately kept out of this repository.

> **History.** MelonDMA started as an MIT-licensed clean-room effort that
> intended to derive only from dual-licensed (GPL-2.0/OpenIB-BSD) Linux kernel
> sources. During development, the implementation path changed to *porting*
> existing GPL-2.0-only mlx5 driver code to DriverKit. That makes the result a
> derived work of GPL-2.0-only sources, so the project relicensed to GPL-2.0
> when that code landed in this repository. The earlier "no GPL code included"
> statement in this file is superseded by this notice.

## What this repository derives from

### AppleMCX (GPL-2.0-only) — primary porting donor

Source: [github.com/acyst/AppleMCX](https://github.com/acyst/AppleMCX),
kept in-tree at [`donors/applemcx/`](donors/applemcx/).

The DriverKit extension in [`src/dext/`](src/dext/) is a port of AppleMCX's
mlx5 driver logic (itself a port of MLNX_OFED 5.9 `mlx5_core`/`mlx5_ib`):

- `Sources/core/` — command interface (`MlxCmd`), DMA (`MlxDMA`), event queues
  (`MlxEQ`), firmware pages (`MlxFwPages`), health (`MlxHealth`), UAR
  (`MlxUAR`), from `donors/applemcx/Sources/core/`.
- `Sources/ib/` — QP/CQ/MR/AH/GID/CC/RoCE object layer, from
  `donors/applemcx/Sources/ib/`.
- `Sources/hw/` — register definitions, WQE layouts, doorbell layout,
  `mlx5_bits.h`, portable encoders, from `donors/applemcx/Sources/hw/`.
- `Sources/userclient/` + `usermode/librdma_shim/` — the `IOUserClient` ABI
  (`MlxUCIO.h`) and the userspace library, from
  `donors/applemcx/Sources/userclient/` and `donors/applemcx/usermode/libmlx/`.

Copyright © AppleMCX authors, licensed under GPL-2.0
([donors/applemcx/LICENSE](donors/applemcx/LICENSE)).

### verbifrost (GPL-2.0-only) — secondary reference

Source: [github.com/EternaPeptix/verbifrost](https://github.com/EternaPeptix/verbifrost),
kept in-tree at [`donors/verbifrost/`](donors/verbifrost/).

Early exploration (the abandoned kext prototype, now in
[`archive/kext-prototype/`](archive/kext-prototype/)) was based on verbifrost;
parts of the register/bitfield headers in `src/dext/Sources/hw/` (notably
`mlx5_bits.h`) trace back to it.

Copyright © verbifrost authors, licensed under GPL-2.0
([donors/verbifrost/LICENSE.GPL](donors/verbifrost/LICENSE.GPL)).

### Linux kernel mlx5 / MLNX_OFED (GPL-2.0 OR OpenIB-BSD, dual-licensed)

The upstream origin of all of the above: `include/linux/mlx5/mlx5_ifc.h`,
`drivers/infiniband/hw/mlx5/`, `drivers/net/ethernet/mellanox/mlx5/core/` in
the Linux kernel and MLNX_OFED drops. Mellanox's dual-license header grants a
choice of GPL-2.0 or the OpenIB.org BSD license; since this repository is
GPL-2.0 as a whole, the GPL-2.0 grant is the one in effect for anything
transcribed from these files.

> Copyright (c) 2013-2015, Mellanox Technologies, Ltd. All rights reserved.

### rxe-reference (various, GPL-2.0 Linux kernel)

Soft-RoCE (`rxe`) kernel sources kept as a protocol reference at
[`donors/rxe-reference/`](donors/rxe-reference/) — used to cross-check RoCEv2
packet formats during early software-protocol exploration (notes 04/07).
Linux kernel sources, GPL-2.0.

## What is deliberately *not* in this repository

- **Mellanox Adapters Programmer's Reference Manual (PRM)** and the **InfiniBand
  Architecture specification** PDFs. Both are proprietary/copyrighted
  documents. They were used privately as references (register layouts, command
  semantics); neither the PDFs nor extracted text/images from them are
  distributed here. They remain in the private project workspace only.
- **Prebuilt binaries**: kext `.pkg` builds from the abandoned prototype and
  dext build artifacts are excluded from git (see `.gitignore`).
- **Apple XNU/DriverKit kernel sources** (`IOService.cpp`, `IOUserClient.cpp`,
  `IOUserServer.cpp`), APSL-2.0 licensed. Consulted locally as reference while
  researching driver-matching internals for the PCI-ownership-takeover work in
  `notes/25`–`27` (`IOService::probeCandidates`, dext crash/rematch counters,
  `IOCatalogueSendData`). No code from them is transcribed into this
  repository's own sources — the takeover mechanism only calls documented/
  reverse-engineered DriverKit and IOKit entry points — and the files
  themselves are excluded from git (see `.gitignore`) since a repository
  declared GPL-2.0 as a whole shouldn't also ship full copies of
  differently-licensed Apple source at the root.

## Provenance discipline going forward

- Files ported from a donor carry a `Ported from:` header comment naming the
  donor source file.
- New files that transcribe structures/constants from `mlx5_ifc.h` or the PRM
  note the source in a comment rather than presenting magic numbers.
- No proprietary document text is committed to this repository.
- If a future contribution would change the licensing basis, the project
  license changes visibly with it — not silently.
