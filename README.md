# CMCC PZ-L8 — OpenWrt with QSDK NSS offload

Qualcomm NSS hardware offload on an **unmodified OpenWrt 25.12.5 baseline**
(kernel 6.12.94, aarch64) for the CMCC PZ-L8 (IPQ5018 + QCA8337 + QCN6122),
built from Qualcomm's own QSDK 14.0 sources rather than a vendor SDK fork.

## Measured on hardware

Forwarding is LAN/WiFi → router → WAN, iperf3 `-P 4`, 15 s per direction.
CPU is the two Cortex-A53 host cores; the NSS UBI32 core is separate.

| Path | This build | nwrt (vendor stack) | Stock OpenWrt |
|---|---|---|---|
| **Wired ↔ WAN** | **949 up / 949 down Mbps, +0 % CPU** | 924 / 926 @ 4 % | 502 @ 100 % (DSA) |
| WiFi link itself (HE80 2×2) | 641 / 470 Mbps | — | — |
| **WiFi 5 GHz ↔ WAN** | **510 up / 438 down Mbps, +0.3 % CPU** | ~624 Mbps @ 25–35 % | 308 Mbps |
| **WiFi 2.4 GHz ↔ WAN** | **58 up / 61 down Mbps, +0.3 % CPU** | – | – |
| MemAvailable, everything up | 18–20 MB | 36 MB | — |

The CPU column is incremental: `/proc/stat` is sampled once a second through
the run and the idle baseline of the same capture is subtracted, because the
sampler itself costs 2–5 % on these cores. Wired lands at or below its own
baseline — 949 Mbps costs nothing the measurement can resolve. nwrt's WiFi
figure is about 20 % faster than this build and carries 25–35 % host CPU with
it.

Wired forwarding runs at line rate with the host CPU **completely idle** — the
packets never enter Linux. **WiFi forwarding now does too**, on both radios and
in both directions: the host sees about a hundred frames per gigabyte and the
rest is forwarded inside the NSS core.

An earlier version of this file said WiFi offload was a structural consequence
of ath11k not being an NSS-managed interface and could not be fixed by tuning.
That was wrong. It is fixed; [docs/WIFILI.md](docs/WIFILI.md) is what it took
and what is still open. The numbers above are the forwarded path only — a client
on WiFi, through the router with NAT, to a wired host — because traffic sourced
on the router itself measures its own userspace and says nothing about the
offload.

## What works

- NSS core boots, ECM offload stack loads automatically at every boot, with a
  self-disarming safety net
- External QCA8337 driven by `qca-ssdk`/swconfig instead of DSA — two
  independent GMACs, MTU 1500, no DSA tag
- Dual-band WiFi (2.4 GHz + 5 GHz HE80) on ath11k
- Status LED, WAN DHCP, LuCI, sysupgrade

## Known limitations

- WiFi offload arms itself at boot now, but the handover still lives in an
  out-of-tree module rather than in ath11k, and a radio whose handover fails
  cannot receive at all. See [docs/WIFILI.md](docs/WIFILI.md).
- **Memory is tight.** 256 MB board; ath11k's data-path rings are patched down
  from the upstream sizes or nothing fits alongside NSS.
- Monitor-mode capture on the radios is effectively disabled by those ring sizes.
- `qca-ssdk-shell` (`ssdk_sh`) does not build — `-fPIC` does not reach
  `src/sal/sd` through its recursive make.

## Building

### GitHub Actions

Push, or run the **Build CMCC PZ-L8 (NSS)** workflow manually. Images land in
the run's artifacts, and a manual run can also publish a release.

### Locally

```sh
git clone https://github.com/openwrt/openwrt -b v25.12.5
git clone <this repo> pzl8-nss
pzl8-nss/scripts/setup.sh ./openwrt
cd openwrt && make -j$(nproc)
```

`setup.sh` is idempotent and prints what it touches.

## Flashing

`sysupgrade -n` from a running OpenWrt, or the U-Boot web recovery at
`192.168.10.10`. Default LAN address is **192.168.10.1**.

The stock `platform.sh` aborts sysupgrade on this board — it calls
`elecom_upgrade_prepare()`, which expects an A/B rootfs pair that PZ-L8 does not
have, and the flash silently does nothing. Patched here.

## Layout

```
config/          .config seed (diffconfig output)
docs/            engineering notes - read FINDINGS.md first
feed/            our packages: qca-nss-drv, -ecm, -clients, qca-mcs,
                 nss-firmware, ipq-wifi, qca-ssdk-shell
files/           rootfs overlay: the nss-offload init script
manifest/        QSDK 14.0 revisions these sources are pinned to
openwrt/
  0001-*.patch   edits to files OpenWrt already ships
  tree/          files OpenWrt does not have, copied in verbatim
scripts/setup.sh applies all of the above to a clean checkout
```

## Why the sources are pinned where they are

- **NSS driver / ECM / mcs**: QSDK 14.0 (`NHSS.QSDK.14.0.r9-00040-O`) from
  git.codelinaro.org.
- **NSS clients** (pppoe, qdisc, vlan-mgr): QSDK **12.5.5**. QSDK 14's client
  feed is PPE-only and IPQ5018 has no PPE block — Qualcomm's own
  `nss-ppe/Makefile` lists only ipq52xx/53xx/54xx/95xx/96xx.
- **NSS firmware**: `NSS.FW.12.2-156-MP.R`. The 12.5 line ignores H2N
  checksum-generation flags on this SoC and 11.4 refuses VAP allocation.
- **Data plane**: OpenWrt's own `qca-nss-dp` (`syn_gmac_dp`), the same driver
  the vendor firmware uses. No `qca-dwmac-nss` shim: that exists for trees where
  ipq50xx was moved to upstream stmmac, which is not the case here.

## Credits

QSDK sources are Qualcomm's. Packaging skeletons and many kernel-6.x fixes
derive from the community NSS feeds by Julius Bairaktaris and Stanislaw Pal
(kuncy7), both GPL. The ath11k ring-size reduction follows the approach of
openwrt/openwrt PR #21495, which was not merged.
