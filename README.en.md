# CMCC PZ-L8 — OpenWrt with QSDK NSS offload

[中文](README.md) | **English**

Qualcomm NSS hardware offload on an **unmodified OpenWrt 25.12.5 baseline**
(kernel 6.12.94, aarch64) for the CMCC PZ-L8 (IPQ5018 + QCA8337 + QCN6122),
built from Qualcomm's own QSDK 14.0 sources rather than a vendor SDK fork.

Built images for every commit on `main` are on the
[Releases](../../releases) page - `sysupgrade.bin` to upgrade a router already
running OpenWrt, `factory.ubi` for a first install from U-Boot.

## Measured on hardware

Forwarding is LAN/WiFi → router → WAN, iperf3 `-P 4`, 15 s per direction.
CPU is the two Cortex-A53 host cores; the NSS UBI32 core is separate.

| Path | This build | nwrt (vendor stack) | Stock OpenWrt |
|---|---|---|---|
| **Wired ↔ WAN** | **949 up / 949 down Mbps, +0 % CPU** | 924 / 926 @ 4 % | 502 @ 100 % (DSA) |
| WiFi link itself (HE80 2×2) | 641 / 470 Mbps | — | — |
| **WiFi 5 GHz ↔ WAN** | **510 up / 438 down Mbps, +0.3 % CPU** | ~624 Mbps @ 25–35 % | 308 Mbps |
| **WiFi 2.4 GHz ↔ WAN** | **58 up / 61 down Mbps, +0.3 % CPU** | – | – |
| MemAvailable, everything up | **40 MB** | 36 MB | — |

The CPU column is incremental: `/proc/stat` is sampled once a second through
the run and the idle baseline of the same capture is subtracted, because the
sampler itself costs 2–5 % on these cores. Wired lands at or below its own
baseline — 949 Mbps costs nothing the measurement can resolve. nwrt's WiFi
figure is about 20 % faster than this build and carries 25–35 % host CPU with
it.

The 5 GHz row predates the NSS firmware moving from 12.2 to 12.5.

That column has gaps because the reference images were not measured for every
row at the time. Both v1.6 and nwrt have since been flashed onto this board and
measured properly - same client, same server, same afternoon - but with four
parallel HTTP streams rather than iperf3, so those numbers go in their own
table instead of into the empty cells above:

| four parallel HTTP streams, 20 s | v1.6 | nwrt | this build |
|---|---|---|---|
| wired LAN → WAN | 912.1 | 901.8 | **912.6** |
| 5 GHz at 80 MHz | - | - | 462 |
| **5 GHz at 160 MHz** | **730** | **722** | **697** |
| 2.4 GHz at 20 MHz | 98.8 | **119.4** | 75.0 |
| 2.4 GHz at 40 MHz | - | - | **112.5** |
| host CPU during 5 GHz | not measurable, see below | 16.1 % at 537 | **2.9 % at 441** |

**At matched width this build lands within 3.5-4.6 % of both references.** The
5 GHz gap was attributed to channel width on an arithmetic argument (722/425 is
1.70 against a bandwidth ratio of 2.0); running this build at 160 MHz turns
that into a measurement - 461.6 to 696.9, worth +51 %, leaving a remainder too
small to need its own explanation.

The CPU cell is a like-for-like comparison for the first time - same band, same
width, same method, each with its own idle control. nwrt spends 5.6x the CPU to
move 22 % more data.

160 MHz needs a country code set: under the default `country 00` the 5 GHz band
is split into two 80 MHz regulatory blocks and no 160 MHz channel exists. The
resulting channel covers DFS spectrum, so hostapd runs a 62 s CAC before it
beacons, on every start. That is why this is not the shipped default - and a
regulatory domain is not something an image should choose for its user.

This build's column was re-measured after flashing back onto the board, with
`n2h_high_water_core0` restored to the vendor's 16336. The CPU cell has an idle
control behind it: 3.1 % with no traffic, 5.9 % during the transfer, so the
traffic costs +2.8 points. Per Mbit/s that is 0.017 % against nwrt's 0.030 % -
but nwrt moves those bits over 160 MHz and this build over 80, so it is not a
like-for-like radio.

Neither reference is an unmodified OEM image: v1.6 is a community build on the
vendor's 4.4 SDK, nwrt one on 5.4, both with the closed qca-wifi driver.

The 5 GHz gap is mostly channel width: both references run **160 MHz** and the
client associates at 1922-2162 Mbit/s, ath11k runs 80 MHz and the same client
associates at 1201. v1.6 associates 12 % above nwrt and delivers 1 % more, so
both have hit the same ceiling.

v1.6's CPU cell is empty because that box has no idle time to begin with - four
hung `hostapd_cli` processes pin both cores (load average 7.5, 0 % idle). Which
turns out to prove the point: it still reaches 730 Mbit/s with both cores
saturated, so the forwarding is not going through the host CPU.

[docs/WIFILI.md](docs/WIFILI.md) has the rest, including the configuration
differences and the places this build diverges from both references on purpose.

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

## Wi-Fi defaults, which come up on their own

A fresh flash is connectable without going through LuCI first:

| | SSID | channel | width |
|---|---|---|---|
| 2.4 GHz | `PZ-L8-2G-XXXX` | auto (ACS) | asks for HE40, usually settles at 20 MHz |
| 5 GHz | `PZ-L8-5G-XXXX` | 36 | **HE160 (160 MHz)** |

Encryption `psk2+ccmp`, key `pzl8test2026`.

`XXXX` is the last two bytes of this board's MAC, uppercase and unseparated,
appended on first boot by `/etc/uci-defaults/97-pzl8-wifi-ssid` - the same
naming the vendor image uses, so two boards running this image do not collide.
Rename them to anything; that script only touches a name it still recognises as
the shipped one.

### Two things to change

**The key.** This image is published, so this key is public - anyone who reads
the repository can join a router still using it. **Change it first.**

```sh
uci set wireless.default_radio0.key='your key'
uci set wireless.default_radio1.key='your key'
uci commit wireless && wifi reload
```

**The country code, which ships as `CN`.** Not a cosmetic preference: under the
default world domain (`country 00`), `iw reg get` splits 5 GHz into
`5170-5250 @ 80` and `5250-5330 @ 80` with the upper half DFS and passive-scan,
**no 160 MHz channel exists at all**, and radio1 silently falls back to 80 MHz.
Set it to where the board actually is.

### Two behaviours that follow

**5 GHz takes about a minute to appear on every boot.** Under CN the only
160 MHz segment is 5170-5330 centred on 5250, and it covers DFS spectrum, so
hostapd must finish a channel-availability check before the first beacon:

```
hostapd: phy0-ap0: DFS-CAC-START ... cac_time=60s
hostapd: phy0-ap0: DFS-CAC-COMPLETED success=1 ... radar_detected=0
hostapd: phy0-ap0: interface state DFS->ENABLED
```

5 GHz is invisible until it finishes; 2.4 GHz is unaffected and comes up first.
Radar can also move the channel later. Both are the price of 160 MHz on this
board - `HE80` removes them and costs about a third of the throughput
(697 to 462 Mbit/s measured).

**2.4 GHz usually runs at 20 MHz, not 40.** The config asks for `HE40` but does
not set `noscan`, so the 802.11 coexistence scan drops it to 20 MHz where
neighbouring BSSs are dense - which is exactly what both reference images
(v1.6 and nwrt) do. To hold 40 MHz regardless:

```sh
uci set wireless.radio0.noscan='1'
uci commit wireless && wifi reload
```

Worth about +50 % here (75.0 to 112.5 Mbit/s), at the cost of ignoring what 25
other networks are asking for; the airtime it gains comes from them. **Off by
default.**

## Hardware readouts on the status page

Stock LuCI's overview shows no CPU model, no temperatures and no acceleration
engine load. The data sources were always there; what was missing was somewhere
to put them, so this build adds a section:

| row | source |
|---|---|
| Processor | `/proc/device-tree/cpus/cpu@0/compatible` plus cpufreq - aarch64's `/proc/cpuinfo` has no `model name` |
| CPU load | two `/proc/stat` samples, differenced in the browser |
| CPU temperature | the `/sys/class/thermal/` zone whose type contains `cpu` |
| Wi-Fi temperature | the `/sys/class/hwmon/` entries named `ath11k_hwmon`, one per radio |
| NSS/PPE utilisation | `/sys/kernel/debug/qca-nss-drv/stats/cpu_load_ubi` |

Three files, and nothing LuCI ships is modified:

```
files/usr/libexec/rpcd/luci.pzl8                       rpcd plugin, one call for everything
files/usr/share/rpcd/acl.d/luci-pzl8.json              read access to that one method
files/www/luci-static/resources/view/status/include/15_pzl8_hardware.js
```

The status page's `index.js` builds its section list with `fs.list()` over the
include directory, so **dropping a file in is enough** - no need to rewrite
`10_system.js` the way nwrt does.

An rpcd plugin rather than a handful of front-end `fs.read` calls, because the
page polls every few seconds and each read is its own ubus round trip - and
because `cpu_load_ubi` lives in debugfs, readable only by root, which rpcd
already is.

**The Wi-Fi temperature rows are labelled by device-tree node, not phy index.**
phy numbering is not stable: one `wifi reload` moved the 2.4 GHz radio from
phy0 to phy1 on this board, while `c000000.wifi` and `b00a040.wifi` do not
move.

### Interface language

The image carries Chinese: four `luci-i18n-*-zh-cn` packages plus this build's
own `pzl8.zh-cn.lmo` for the six labels above. On first boot
`/etc/uci-defaults/96-pzl8-luci-lang` sets `luci.main.lang` to `zh_cn`, **only
while it is still LuCI's own default of `auto`** - after a choice made in
System > Language, the script runs again from a new image on sysupgrade but
leaves that choice alone.

To follow the browser instead: `uci set luci.main.lang=auto`.

The msgids stay English and the Chinese lives in a catalogue, so an English
interface still reads English here rather than hardcoded Chinese. The source is
`po/pzl8.zh-cn.po`; the compiled `.lmo` is committed alongside it because the
build only copies `files/` and has no step that compiles a `.po`. After editing
the source:

```sh
po2lmo po/pzl8.zh-cn.po files/usr/lib/lua/luci/i18n/pzl8.zh-cn.lmo
```

`po2lmo` is a luci-base host tool, found under `staging_dir/hostpkg/bin/` in an
OpenWrt build tree.

The server merges **every `.lmo` for the requested language** in
`/usr/lib/lua/luci/i18n/` and ships them to the browser in one response from
`/cgi-bin/luci/admin/translations/<lang>`, so dropping one file in is enough.

## What works

- NSS core boots, ECM offload stack loads automatically at every boot, with a
  self-disarming safety net
- External QCA8337 driven by `qca-ssdk`/swconfig instead of DSA — two
  independent GMACs, MTU 1500, no DSA tag
- Dual-band WiFi (2.4 GHz + 5 GHz HE80) on ath11k, offloaded on both radios
- Several SSIDs per radio, `option isolate`, and per-station rx rate in
  `iw station dump` - all of which needed driver work, see
  [docs/WIFILI.md](docs/WIFILI.md)
- SQM that actually shapes, through `sqm-scripts-nss` and the NSS qdiscs
- Status LED, WAN DHCP, LuCI, sysupgrade

## Known limitations

- QoS must use the `nss-edma` SQM script: the data path is in the NSS
  cores, so Linux qdiscs never see the traffic and cake or fq_codel
  silently do nothing. See [docs/WIFILI.md](docs/WIFILI.md).
- **Memory is tight.** A 256 MB board reports 173 MB of MemTotal - 48 MB of
  the reservation is the Q6 wireless firmware and cannot shrink - and ath11k
  takes 46 MB of what is left. ath11k's data-path rings are patched down from
  the upstream sizes or nothing fits alongside NSS. MemAvailable lands at
  40 MB; `docs/WIFILI.md` has where the rest goes.
- Monitor-mode capture on the radios is effectively disabled by those ring sizes.
- The firewall's flow-offload switches do nothing here. Hardware offload is
  `[fixed]` off - `qca-nss-dp` has no flowtable support - and software offload
  is inert because ECM hands connections to NSS before the netfilter forward
  path sees them. Measured: with it on, the flowtable took zero connections
  while NSS kept accelerating. Leave both off.
- `qca-ssdk-shell` (`ssdk_sh`) does not build — `-fPIC` does not reach
  `src/sal/sd` through its recursive make.

## Building

### GitHub Actions

Every green build of `main` publishes a [release](../../releases) with the
sysupgrade, factory and initramfs images, their sha256sums and the manifest.
The images are also attached to the run as artifacts, which expire; the
release does not. Pull requests build but do not publish, and a manual
**Build CMCC PZ-L8 (NSS)** run can opt out by unticking `make_release`.

### Locally

```sh
git clone https://github.com/openwrt/openwrt -b v25.12.5
git clone https://github.com/mufeng05/openwrt-cmcc-pz-l8-nss pzl8-nss
pzl8-nss/scripts/setup.sh ./openwrt
cd openwrt && make -j$(nproc)
```

`setup.sh` is idempotent and prints what it touches.

### Choosing your own packages

After `setup.sh`, the tree is an ordinary OpenWrt tree:

```sh
cd openwrt
make menuconfig        # add LuCI apps, tools, whatever you want
make -j$(nproc)
```

`setup.sh` seeds `.config` only when there is not one already, so re-running it
to pick up new commits will not discard your selection. `RESEED=1
pzl8-nss/scripts/setup.sh ./openwrt` goes back to the shipped config on
purpose. Either way it finishes with `make defconfig`, so packages added
upstream since your config was written get their defaults filled in.

Four symbols must stay on, and menuconfig will not stop you turning them off
because two of them are not packages. `setup.sh` checks them on every run and
warns:

| | |
|---|---|
| `CONFIG_NSS_DRV_WIFIOFFLOAD_ENABLE` | builds the wifili and wifi_vdev half of qca-nss-drv. Without it `ath11k.ko` does not link - ten undefined `nss_wifili_*` symbols at modpost |
| `CONFIG_NSS_FIRMWARE_VERSION_12_5` | picks the firmware blob **and**, through qca-nss-drv's patch 0022, the wifili message ABI. Turning it off does not fail the build; it leaves the peer-stats array stride 16 bytes short of what the firmware sends |
| `CONFIG_PACKAGE_kmod-qca-nss-drv` | the NSS driver itself |
| `CONFIG_PACKAGE_nss-firmware-ipq50xx` | the blob |

To keep a selection in the repo, write it back over the seed:

```sh
cd openwrt
./scripts/diffconfig.sh > ../pzl8-nss/config/cmcc_pz-l8.config
```

The workflow re-checks the same four symbols after `defconfig`, so a seed that
loses one fails in seconds with `MISS <symbol>` instead of forty minutes later
in modpost.

## Flashing

`sysupgrade -n` from a running OpenWrt, or the U-Boot web recovery at
`192.168.10.10`. Default LAN address is **192.168.10.1**.

The two want different image formats and neither is obvious - the web recovery
runs `source $imgaddr:script` and therefore needs a FIT with a flashing script
in it, not a `.ubi`. [docs/RECOVERY.md](docs/RECOVERY.md) has both paths, the
flash layout, and what the bootloader checks.

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
- **NSS firmware**: `NSS.FW.12.5-210-MP.R`, and the matching wifili ABI. An
  earlier version of this file said 12.2-156 because 12.5 was believed not to
  work on this SoC; it does, and it is about 65 % faster on Wi-Fi across two
  A/B cycles. The catch is that the blob cannot be swapped on its own -
  qca-nss-drv's patch 0022 puts four `uint32_t` behind
  `NSS_FIRMWARE_VERSION_12_5`, all of them inside the per-peer array of the
  peer-stats message, so choosing the firmware has to choose the ABI with it or
  every peer after the first is read at the wrong offset. 11.4 is still the
  only line that accepts mesh.
- **Data plane**: OpenWrt's own `qca-nss-dp` (`syn_gmac_dp`), the same driver
  the vendor firmware uses. No `qca-dwmac-nss` shim: that exists for trees where
  ipq50xx was moved to upstream stmmac, which is not the case here.

## Credits

QSDK sources are Qualcomm's, from
<https://git.codelinaro.org/clo/qsdk>.

Packaging skeletons and many kernel-6.x fixes derive from community NSS feeds,
all GPL:

- Julius Bairaktaris — <https://github.com/JuliusBairaktaris>. `nss-packages`
  originates with him, and `openwrt-nss-edma` — the tree this project compares
  against all through `docs/WIFILI.md` — is his. His Signed-off-by is also on
  the iproute2 NSS qdisc and nssmirred patches and several of the qualcommax
  kernel patches carried in `openwrt/tree/`.
- Stanislaw Pal (kuncy7) — <https://github.com/kuncy7>. This project's
  checkouts of both trees are of his forks.
- Sean K (qosmio), who repackages the NSS firmware blobs —
  <https://github.com/qosmio/qca-sdk-nss-fw>

The ath11k ring-size reduction follows the approach of
<https://github.com/openwrt/openwrt/pull/21495>, which was not merged.
