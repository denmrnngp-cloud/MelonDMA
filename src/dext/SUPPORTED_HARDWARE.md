# Supported Hardware

## Supported

- Mellanox/NVIDIA ConnectX-4 Lx PF
- PCI vendor/device: `15b3:1015`
- Link: Ethernet RoCEv2
- Tested transport: ConnectX-4 Lx behind Thunderbolt on Apple Silicon
- Tested peer: Linux NVIDIA ConnectX RoCE endpoint

Support means the exact PCI match, firmware bring-up, RC traffic, MR/CQ/QP
lifecycle, direct-UAR, Metal shared-memory, and peer gates have passed on this
model.

## Not Supported or Not Claimed

- ConnectX-4 PF `15b3:1013`
- ConnectX-4 VF `15b3:1014`
- ConnectX-4 Lx VF `15b3:1016`
- ConnectX-5, ConnectX-6, ConnectX-7, ConnectX-8
- InfiniBand link layer
- UD, DC, XRC, SRQ, multicast, raw Ethernet QPs
- Software RoCE
- GPUDirect or Metal `.private` memory

The HCA factory contains family scaffolding, but the published DEXT personality
matches only `0x101515b3`. No other device is a supported release target until
it has a dedicated backend/capability validation and its own live acceptance
record.
