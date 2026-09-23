# CMCC PZ-L8 — OpenWrt with QSDK NSS hardware offload

[中文](README.md) | **English**

Qualcomm NSS hardware offload for the CMCC PZ-L8 (IPQ5018 + QCA8337 + QCN6122)
on an **unmodified OpenWrt 25.12.5 baseline** (kernel 6.12.94, aarch64). Every
driver comes straight from Qualcomm's own QSDK 14.0 sources rather than a
vendor SDK fork.

Wired forwarding and **both radios** run inside the NSS cores; the packets never
reach Linux.

Every green build of `main` publishes a [Release](../../releases) with four
files:

| file | use |
|---|---|
| `*-squashfs-sysupgrade.bin` | upgrading a router already running OpenWrt: `sysupgrade -n` |
| `*-uboot-recovery.fit` | the U-Boot web recovery at `192.168.10.10` — **the only file that page accepts** |
| `*-squashfs-factory.ubi` | a forced `sysupgrade -n -F` from a vendor image, which writes it with `ubiformat` |
| `*-initramfs-uImage.itb` | boots from RAM without touching flash |

---

## Measured on hardware

### Why this exists

There were three options for this board and each was missing something:

- **The vendor-derived community builds (v1.6 / nwrt)** are fast, but both are
  built on the closed qca-wifi driver, both sit on old kernels — `4.4.60` and
  `5.4.250` — and both are **32-bit**: each reports `armv7l`, on an ARMv8 SoC.
  Neither comes with a source tree you can build, so what you get is what there
  is.
- **The official OpenWrt image** has a current 64-bit kernel but no NSS.
  Forwarding goes back through Linux, and the DSA path saturates both cores at
  502 Mbit/s.
- And **the official 25.12.x image cannot bring WiFi up at all**.
  `Device/cmcc_pz-l8` in `target/linux/qualcommax/image/ipq50xx.mk` defines no
  `DEVICE_PACKAGES` — the same file uses it twelve times for other devices — so
  neither the ath11k firmware nor this board's board data is in the image and
  the radios never probe. The snapshot branch (r31246) does carry them; this
  project restores them with `feed/ipq-wifi-cmcc_pz-l8`.

So this build aims at the intersection: **the official baseline, a current
64-bit kernel, something you can build yourself, and NSS offload on top.**

Stock OpenWrt forwards through Linux with DSA on this board. With NSS:

| iperf3 `-P 4`, 15 s per direction | this build | OEM 501.8 | nwrt (closed stack) | stock OpenWrt |
|---|---|---|---|---|
| **wired ↔ WAN** | **949 up / 949 down Mbps, +0 % CPU** | 947 / 947, 9 % / 6 %※ | 924 / 926 @ 4 % | **502 @ 100 % CPU** |
| **5 GHz ↔ WAN** | 510 up / 438 down Mbps, +0.3 % CPU (80 MHz) | 612 / 899 (160 MHz), 15–16 %※ | ~624 @ 25–35 % | 308 |
| **2.4 GHz ↔ WAN** | 58 up / 61 down Mbps, +0.3 % CPU | 114 / 65, 8 %※ | — | — |

> ※ The **OEM column** was measured 2026-09-23 with the same iperf3 `-P 4` (this
> board's genuine factory firmware; see "The genuine OEM firmware as a baseline"
> below). Its CPU is **raw busy%** from `/proc/stat` mid-transfer, not delta over
> idle like this build's column, so read it only as "both cores far from
> saturated". The OEM 5 GHz here is 160 MHz vs this build's 80 MHz — the
> same-width comparison is the three-way table below.

CPU means the two Cortex-A53 host cores — the NSS UBI32 core is separate and not
counted — and this build's figures are **increments**: `/proc/stat` sampled once a second
throughout, minus an idle baseline from the same session, because the sampler
alone costs 2–5 %. The wired row lands on or below its own baseline: whatever
949 Mbps costs is smaller than this method can resolve.

The 5 GHz cell in the stock OpenWrt column cannot be the official image out of
the box: with no firmware and no board data the radio does not probe, so that
figure can only have been taken after putting the two missing packages back.

> This table is the older batch, taken with iperf3, and its 5 GHz row predates
> the NSS firmware moving from 12.2 to 12.5. The table below was measured later
> by a different method — **the two cannot be read across**.

### Three-way, same method

Both reference images were later flashed back onto this board and measured
properly: same board, same client (one Intel AX201), same server, same
afternoon, same method — four parallel HTTP streams of 20 s, with a host `/32`
route forcing the traffic through the router.

| four parallel HTTP streams, 20 s | OEM 501.8 | v1.6 | nwrt | this build |
|---|---|---|---|---|
| wired LAN → WAN | 902.9 | 912.1 | 901.8 | **912.6** |
| **5 GHz at 160 MHz** | 711 | **730** | **722** | **697** |
| 5 GHz at 80 MHz | — | — | — | 462 |
| 2.4 GHz at 20 MHz | ~92 (59–124)※ | 98.8 | **119.4** | 75.0 |
| 2.4 GHz at 40 MHz (`noscan`) | — | — | — | **112.5** |
| host CPU during 5 GHz | 17 % at 711 | not measurable, see below | 16.1 % at 537 | **2.9 % at 441** |

The **OEM column is now this board's unmodified factory firmware** (501.8,
measured 2026-09-23 by the same method; see "The genuine OEM firmware as a
baseline" below). The other two references are still not factory: **v1.6** is a
community build on the vendor's 4.4 SDK, **nwrt** one on 5.4, both with the closed
qca-wifi driver.

> ※ The OEM 2.4 GHz swings hard: six rounds 51.8 / 59.1 / 69.7 / 113.9 / 116 / 124,
> flipping between a ~59 and a ~120 state with neighbour contention (this unit is
> in a live environment with 20+ nearby networks). The good state ~116 matches the
> iperf3 table's 114.

**Wired is a three-way tie** at the client NIC's line rate, so it measures the
client rather than the router.

**At matched width, 5 GHz is within 3.5–4.6 %.** Attributing the gap to channel
width used to be inference (722 / 425 ≈ 1.70 against a bandwidth ratio of 2.0);
running this build at 160 MHz made it a measurement — 462 to 697, worth +51 %,
leaving a remainder too small to need its own explanation.

**The CPU cell is a like-for-like comparison for the first time** (same band,
same width, same method, each with its own idle control): nwrt spends 5.6× the
CPU to move 22 % more data. v1.6's cell is empty because that box has no idle
time to begin with — four hung `hostapd_cli` processes pin both cores, load
average 7.5, 0 % idle. Which argues the same point from the other side: it still
reaches 730 Mbit/s with both cores saturated, so the forwarding is not going
through the host CPU.

**2.4 GHz is the one row where this build clearly trails.** Not for want of
configuration: the AP advertises two streams MCS 0-11 and the firmware really
does use two streams, 20 MHz and a 0.8 µs guard interval — the rate control just
settles at MCS 6–7. The firmware's own statistics say why: 13.5 % of MPDUs are
never acknowledged, 22 % are requeued, and the channel measures 44–58 % busy
with almost none of it ours. The same rate control puts 13 % of its PPDUs at
MCS 9–11 on the much quieter 5 GHz radio, so it is not broken — it is answering
a link that drops one frame in seven. Full working in
[docs/WIFILI.md](docs/WIFILI.md).

### The genuine OEM firmware, as a baseline

Measured 2026-09-23. This board's **genuine factory firmware** — OpenWrt Chaos
Calmer 501.8 / SPF11.4_CSU2, kernel 4.4.60, **32-bit**. The OEM ships with SSH
off; to take the baseline, its U-Boot web-recovery page RAM-booted our initramfs,
which mounted the OEM overlay and enabled SSH temporarily (no boot partition was
flashed). **Its throughput is folded into the "OEM" column of the two tables
above** (iperf3 and four-parallel HTTP, one each); the full capture and a
whole-NAND backup are in `oem-baseline/` (contains real credentials, not committed).

A few things only the genuine OEM could settle:

- **The OEM's NSS firmware is `NSS.MP.11.4-10-R`** (md5 `f4f67348…`), not 12.5.
  This closes the earlier open question of which firmware the factory used: nwrt
  (third-party) runs 12.5, v1.6 (community) runs 11.4-3-R, and the **factory image
  runs 11.4-10-R** — three distinct blobs. The case for 12.5 here (self-measured
  +65 %) is unaffected.
- **The OEM loads both ECM and SFE (`shortcut_fe`), but SFE does nothing**: ECM is
  built with the NSS front-end only (`ecm_nss_ipv4`, no `ecm_sfe`); under load all
  six connections sit in `ecm_nss_ipv4/accelerated_count` while SFE's connection
  table is empty — flows accelerated in NSS never traverse the Linux forward path
  where SFE hooks. The OEM's 947 / 612 / 899 are all NSS. This build likewise uses
  ECM→NSS only (the platform lacks the three kernel hooks SFE needs).
- **Its memory carve-out matches nwrt, not this build**: the OEM hands the kernel
  ~174 MB and reserves 11 MB (`Memory: 167140K/178176K … 11036K reserved`); this
  build hands it the full 256 MB and reserves 86 MB.
- **The NSS config `n2h pool 4096 + high_water 16336`** is confirmed on the genuine
  OEM (previously seen only on nwrt).

---

## Flashing

The default LAN address is **192.168.10.1**.

**Already running OpenWrt** — `sysupgrade -n <image>-squashfs-sysupgrade.bin`

**From a vendor image such as v1.6** — its `platform_do_upgrade` never reads the
sysupgrade tar; it calls `do_flash_ubi`, which is `ubiformat`. So feed it the
**`.ubi`** with `-F`:

```sh
sysupgrade -n -F <image>-squashfs-factory.ubi
```

**Recovery / U-Boot web page (`192.168.10.10`, hold the button at power-on)** —
that page runs `imgaddr=$fileaddr && source $imgaddr:script`, so it accepts
**only a FIT carrying a flashing script**. Upload a `.ubi` and it reports success
and writes nothing.

```sh
curl -F "firmware=@<image>-uboot-recovery.fit" http://192.168.10.10/
```

Both routes, the flash layout and what the bootloader actually checks are in
[docs/RECOVERY.md](docs/RECOVERY.md). Stock `platform.sh` aborts sysupgrade on
this board — it calls `elecom_upgrade_prepare()`, which expects an A/B rootfs
pair that the PZ-L8 does not have — and is patched here.

### V2 units: the Fudan flash

One batch of this model (V2) ships the Fudan Micro **FM25LS01** SPI-NAND in
place of the earlier ESMT F50D1G41LB. Neither mainline nor OpenWrt 25.12.5 knows
that ID — `fmsh.c` carries only FM25S01A (`0xE4`) and FM25S01BI3 (`0xd4`) — so
the flash is not detected at all on a V2 board.

This build carries the entry:
`openwrt/tree/target/linux/generic/pending-6.12/440-mtd-spinand-add-support-for-FudanMicro-FM25LS01.patch`
(ID `0xA5`, 2048-byte pages, 128-byte OOB, 64 pages per block, 1024 blocks). It
lives in `pending-` rather than `backport-` because it is not upstream. Worth
knowing: `fmsh.c` is not a stock kernel file either — OpenWrt adds it through
two generic backports, 401 and 435.

The patch comes from
[CrazyBoyFeng/openwrt-pz-l8](https://github.com/CrazyBoyFeng/openwrt-pz-l8), by
way of ImmortalWrt's `400-mtd-spinand-Support-fmsh.patch` and originally
Rockchip BSP.

**About that ECC figure.** The patch declares `NAND_ECCREQ(8, 512)` where the
datasheet specifies 1 bit per 512 on-die. That is not a typo: the QPIC
controller does **not** use the chip's on-die ECC, it configures its own
hardware ECC from the declared requirement. This board's own dmesg shows the
mechanism — our chip is the ESMT, declared at 1 bit:

```
qcom_snand 79b0000.spi: ECC strength requirement of 1-bit(s) is unsupported, trying 4-bits
```

So the declared figure decides what Linux uses, and it must match what U-Boot
wrote with, or UBI written by the bootloader reads back `-74 EUCLEAN`. The 8 is
what the patch author measured on their own V2 board. **If a V2 unit does not
boot after flashing and reports EUCLEAN, that line is the first thing to look
at.**

**What was not verified:** this board is a V1 — its dmesg reads `ESMT SPI NAND
was found` with a 64-byte OOB — so the path was verified only as far as *the
patch applies cleanly and builds*. It has never run on real FM25LS01 hardware
here, and the chip ID, geometry and ooblayout are taken from the upstream patch
without an independent check against the datasheet. The risk to a V1 board is
one row in a chip table, reached only on ID `0xA5`.

---

## Wi-Fi defaults, which come up on their own

A fresh flash is connectable without going through LuCI first.

| | SSID | channel | width |
|---|---|---|---|
| 2.4 GHz | `PZ-L8-2G-XXXX` | auto (ACS) | asks for HE40, usually settles at 20 MHz |
| 5 GHz | `PZ-L8-5G-XXXX` | 36 | **HE160 (160 MHz)** |

Encryption `psk2+ccmp`, key `pzl8test2026`. `XXXX` is the last two bytes of this
board's MAC, uppercase and unseparated, appended on first boot by
`/etc/uci-defaults/97-pzl8-wifi-ssid` — the same naming the vendor image uses, so
two boards running this image do not collide. Rename them to anything; that
script only touches a name it still recognises as the shipped one.

### Two things to change

**The key.** This image is published, so this key is public — anyone who reads
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
160 MHz segment is 5170–5330 centred on 5250, and it covers DFS spectrum, so
hostapd must finish a channel-availability check before the first beacon:

```
hostapd: phy0-ap0: DFS-CAC-START ... cac_time=60s
hostapd: phy0-ap0: DFS-CAC-COMPLETED success=1 ... radar_detected=0
hostapd: phy0-ap0: interface state DFS->ENABLED
```

5 GHz is invisible until it finishes; 2.4 GHz is unaffected and comes up first.
Radar can also move the channel later. This is inherent to 160 MHz on this
board — **every** contiguous 160 MHz channel in 5 GHz reaches into DFS spectrum,
because the two non-DFS blocks are 100 MHz and 125 MHz wide and neither can hold
one. `HE80` removes it and costs about a third of the throughput (697 to 462).

**2.4 GHz usually runs at 20 MHz, not 40.** The config asks for `HE40` but does
not set `noscan`, so the 802.11 coexistence scan drops it to 20 MHz where
neighbouring BSSs are dense — which is exactly what both reference images do. To
hold 40 MHz regardless:

```sh
uci set wireless.radio0.noscan='1'
uci commit wireless && wifi reload
```

Worth about +50 % here (75.0 to 112.5 Mbit/s), at the cost of ignoring what 25
other networks are asking for; the airtime it gains comes from them. **Off by
default.**

---

## Full cone NAT

Images built by CI carry
[openwrt-sonic-fullcone](https://github.com/mufeng05/openwrt-sonic-fullcone):
SONiC's fullcone NAT kernel patch ported to OpenWrt, with nftables, fw4 and LuCI
support. **It is off by default**, and turning it on takes two switches:

1. under Network → Firewall → General Settings, tick **Fullcone NAT** — the
   global gate;
2. edit the wan zone and tick **Fullcone NAT** in its General Settings (it only
   appears while the zone masquerades).

Or:

```sh
uci set firewall.@defaults[0].fullcone='1'
uci set firewall.@zone[1].fullcone='1'
uci commit firewall && /etc/init.d/firewall restart
```

Connections are still accelerated by NSS with it on (ECM `accel_mode=2`), and
wired download holds at 949 Mbit/s, the same as with it off. **Connections that
already existed when it was turned on are not affected**: NAT is decided when a
connection is set up, and one already handed to NSS survives even a conntrack
flush, so reboot once after enabling it. Restricting it to some protocols,
the rules it generates and its known limitations are in that project's own
README.

---

## The web interface

### Hardware readouts on the status page

![The status overview page while routing 985 Mbit/s](docs/img/overview.png)

The whole page, read while **routing 985 Mbit/s**. What this build adds is the
Hardware section in it: 8 % CPU, 14 % NSS, eleven accelerated connections, and
both ports at line rate in opposite directions. The data path is inside the NSS
cores and the main CPU is largely idle — making that visible is what the section
is for. The only smudged fields are the client hostnames, MACs, DUIDs and
addresses in the two DHCP lease tables and the associated-stations table. The
interface is in Chinese by default; see below.

Stock LuCI's overview shows no CPU model, no temperatures and no acceleration
engine load. The data sources were always there; what was missing was somewhere
to put them, so this build adds a section:

| row | source |
|---|---|
| Processor | `/proc/device-tree/cpus/cpu@0/compatible` plus cpufreq — aarch64's `/proc/cpuinfo` has no `model name` |
| CPU load | two `/proc/stat` samples, differenced in the browser |
| CPU temperature | the `/sys/class/thermal/` zone whose type contains `cpu` |
| Wi-Fi temperature | the `/sys/class/hwmon/` entries named `ath11k_hwmon`, one per radio |
| NSS utilisation | `/sys/kernel/debug/qca-nss-drv/stats/cpu_load_ubi` |
| Accelerated connections | `/sys/kernel/debug/ecm/ecm_db/connection_count` |
| Port throughput | netdev byte counters on the `nss-dp` ports, differenced |

Three files, and nothing LuCI ships is modified:

```
files/usr/libexec/rpcd/luci.pzl8                       rpcd plugin, one call for everything
files/usr/share/rpcd/acl.d/luci-pzl8.json              read access to that one method
files/www/luci-static/resources/view/status/include/15_pzl8_hardware.js
```

The status page's `index.js` builds its section list with `fs.list()` over the
include directory, so **dropping a file in is enough** — no need to rewrite
upstream's `10_system.js` the way nwrt does. An rpcd plugin rather than a handful
of front-end `fs.read` calls, because the page polls every few seconds and each
read is its own ubus round trip — and because `cpu_load_ubi` lives in debugfs,
readable only by root, which rpcd already is.

A few choices that are not obvious:

- **NSS, not NSS/PPE.** The packet processing engine is an IPQ807x / IPQ60xx /
  IPQ95xx block; **IPQ5018 does not have one**.
- **Throughput comes from `eth0` / `eth1`, not `br-lan`.** Measured: during one
  transfer both ports moved 989 Mbit/s while `br-lan` saw 52, because
  accelerated traffic never reaches the Linux bridge.
- **Wi-Fi temperature rows are keyed on the device-tree node, not the phy
  index.** phy numbering is not stable — one `wifi reload` moved the 2.4 GHz
  radio from phy0 to phy1 here — while `c000000.wifi` and `b00a040.wifi` do not
  move.
- **The rate interval is measured** (`Date.now()` difference) rather than assumed
  from the poll period, and a negative delta from an interface that went down
  reads as unknown rather than as a negative rate.

**Ports are resolved by asking netifd, not by guessing.** This first derived WAN
from whichever port carried the default route, which only holds when the uplink
sits directly on a physical port: with PPPoE the route is on `pppoe-wan` and with
a tagged uplink on `eth1.2`, neither of which is an nss-dp port, so every port
fell back to LAN — and a dumb AP has no default route at all. A port is now
claimed by the interface whose `device` it is, whose `device` is a VLAN of it, or
whose `device` is the bridge it belongs to. The point is that **netifd's
`device` stays the layer-2 device** — verified on the board, where a PPPoE
interface over a dummy reported `device = pppdummy` with `l3_device` empty.
**Nothing is assumed to be called wan or lan, and nothing is assumed to exist**;
a port no interface claims is still shown, under its own name.

### Two more pages

![Network → Wireless](docs/img/wireless.png)

radio0 is the IPQ5018's own 2.4 GHz, radio1 is the QCN6122 — **both radios run,
both offloaded to NSS**. 5 GHz sits on channel 36 at 160 MHz, and the
`160 MHz, HE-MCS 10, HE-NSS 2` line under the associated station is the
**per-station** negotiated rate, which needed a driver change to obtain; see
[docs/WIFILI.md](docs/WIFILI.md). The client's MAC and hostname are smudged out.

![Network → Switch](docs/img/switch.png)

This is what replacing DSA with `qca-ssdk` and swconfig looks like: **a
`CPU (eth0)` column and a `CPU (eth1)` column side by side**, two independent
GMACs each acting as a CPU port, rather than DSA binding one conduit and wasting
the other. Ports are MTU 1500 with no DSA tag. The reasoning is in
[docs/FINDINGS.md](docs/FINDINGS.md), section 1.

### Interface language

The image carries Chinese: four `luci-i18n-*-zh-cn` packages plus this build's
own `pzl8.zh-cn.lmo` for the labels above. On first boot
`/etc/uci-defaults/96-pzl8-luci-lang` sets `luci.main.lang` to `zh_cn`, **only
while it is still LuCI's own default of `auto`** — after a choice made in
System > Language, the script runs again from a new image on sysupgrade but
leaves that choice alone. To follow the browser instead:
`uci set luci.main.lang=auto`.

The msgids stay English and the Chinese lives in a catalogue, so an English
interface still reads English here rather than hardcoded Chinese. The source is
`po/pzl8.zh-cn.po`; the compiled `.lmo` is committed alongside it because the
build only copies `files/` and has no step that compiles a `.po`:

```sh
po2lmo po/pzl8.zh-cn.po files/usr/lib/lua/luci/i18n/pzl8.zh-cn.lmo
```

`po2lmo` is a luci-base host tool, under `staging_dir/hostpkg/bin/` in an OpenWrt
build tree. The server merges **every** `.lmo` for the requested language in
`/usr/lib/lua/luci/i18n/` and ships them in one response, so one file is enough.

---

## What works

- The NSS cores come up, and the ECM offload stack loads on every boot
- The external QCA8337 is driven by `qca-ssdk`/swconfig rather than DSA — two
  independent GMACs, MTU 1500, no DSA tag
- ath11k on both bands, **both radios offloaded**; 5 GHz at 160 MHz by default
- Multiple SSIDs per radio, `option isolate`, and per-station receive rates in
  `iw station dump` — each needed a driver change, see
  [docs/WIFILI.md](docs/WIFILI.md)
- SQM that actually shapes (`sqm-scripts-nss` plus the NSS qdiscs), though
  it is **not installed by default** - see below
- macvlan for WAN multi-dial, **accelerated** — the interfaces must be
  `option mode 'private'`, which is the only mode ECM takes; see
  [docs/FINDINGS.md](docs/FINDINGS.md)
- Full cone NAT, off by default and still accelerated by NSS when on — see above
- Status LEDs, WAN DHCP, LuCI (in Chinese), sysupgrade

## Known limitations

- **SQM is not installed by default.** Ticking `sqm-scripts` drags in eighteen
  packages - cake, ifb, tc and the whole legacy xtables compatibility stack
  behind `iptables-nft` - none of which this image needs otherwise, because fw4
  is nftables-native here and the data path is in NSS. To have it, tick the one
  package **`sqm-scripts-nss`** in menuconfig and the dependencies follow,
  including the two NSS kernel modules (`kmod-qca-nss-drv-qdisc` and
  `kmod-qca-nss-drv-igs`).
- Once installed, **QoS must use the `nss-edma` script**: the data path is
  inside the NSS cores, Linux qdiscs never see the traffic, and cake or
  fq_codel **silently do nothing**. See [docs/WIFILI.md](docs/WIFILI.md).
- **Memory is tight.** A 256 MB board reports 173 MB of MemTotal — 48 MB of the
  reservation is Q6 wireless firmware and cannot be reduced — and ath11k takes
  46 MB of what is left. Its data-path rings are already smaller than upstream's,
  or it would not fit alongside NSS.
- Monitor-mode capture on the radios is effectively disabled by those ring sizes.
- **The firewall's flow offloading switches do nothing here.** Hardware
  offloading is `[fixed]` off — `qca-nss-dp` has no flowtable support — and
  software offloading spins idle, because ECM takes the connection before
  netfilter's forward path sees it. Measured: with it on, the flowtable received
  zero connections while NSS accelerated as usual. Both stay off.
- 2.4 GHz throughput trails both reference images; see the three-way comparison
  above.
- **Channel analysis does not work on 5 GHz at 160 MHz.** No 160 MHz block in
  5 GHz avoids DFS channels, and mac80211 refuses a scan while the channel
  context has radar detection enabled (`iw scan` returns `Resource busy`), so
  LuCI's channel analysis sees nothing but the local AP there. To survey the
  band, drop radio1 to HE80 on 36–48 or 149–161, scan, and put it back. 2.4 GHz
  has no DFS and is unaffected.
- **Radar drops 5 GHz to 80 MHz and it does not come back by itself.** Neither
  the 30-minute non-occupancy period expiring, nor `wifi reload`, nor a runtime
  channel switch restores it; only `wifi down radio1 && wifi up radio1`, which
  re-runs CAC (64 s measured, 5 GHz alone down). **Restarting the radio while
  the NOP is still running leaves 5 GHz down entirely**, so do not. A watchdog
  to automate the recovery was written and dropped: a minute without 5 GHz costs
  more than sitting at 80 MHz.
- **Noise below -100 dBm always displays as -100 under Realtime Graphs →
  Wireless.** That is upstream LuCI, left alone. Phy Rate on the same page is
  wrong upstream too — its source, `luci-bwc`, keeps the rate in a `uint16_t` of
  kbit/s, so anything above 65.5 Mbit/s wraps and 1921.5 Mbit/s reads as
  20 Mbit/s — and this build widens it to 32 bits with the patch under
  `openwrt/feeds/luci/modules/luci-mod-status/patches/`; measured, 2161.3 Mbit/s
  reads correctly and matches iwinfo. The page also plots the *associated
  client's* signal and rate, so with no client connected it is all zeroes by
  design.
- `qca-ssdk-shell` (`ssdk_sh`) does not build — `-fPIC` does not survive its
  recursive make into `src/sal/sd`.

---

## Building

### GitHub Actions

Every green build of `main` publishes a [release](../../releases). The images are
also attached to the run as artifacts, but artifacts expire and releases do not.
Pull requests build without publishing; a manual run of **Build CMCC PZ-L8
(NSS)** can opt out by unticking `make_release`.

The U-Boot recovery FIT is built by CI from the same factory image it publishes,
and **the build fails if it grows past the bootloader's 32 MiB limit** — better a
failed build than a file that is rejected at the one moment it is needed.

CI also clones [luci-theme-argon](https://github.com/jerrykuku/luci-theme-argon)
into `package/` and builds it in. It is **not** in OpenWrt's luci feed — at the
v25.12.5 pin that feed carries bootstrap, material, openwrt and openwrt-2020 and
nothing else — so putting `CONFIG_PACKAGE_luci-theme-argon=y` in the seed
achieves nothing on its own: any tree without the package has defconfig
**silently drop it**, and the image builds fine without the theme.

This happens **in CI only, not in `setup.sh`**: that script's job is applying
this project to a tree, and a local build should not reach out to a third-party
repository it was never asked about. To have it locally, fetch it yourself:

```sh
git clone --depth 1 https://github.com/jerrykuku/luci-theme-argon \
    openwrt/package/luci-theme-argon
cd openwrt && make menuconfig     # tick it under LuCI → Themes
```

It tracks master rather than a pinned commit, so the commit each build actually
used is recorded in `build-info.txt` as `argon_commit` — "latest" is not an
answer to "which one is in this image".

Installing it does not switch to it. Pick it under System → Language and
Interface, or:

```sh
uci set luci.main.mediaurlbase='/luci-static/argon'
uci commit luci
```

CI also runs [openwrt-sonic-fullcone](https://github.com/mufeng05/openwrt-sonic-fullcone)'s
own `add_sonic_fullcone.sh` after `setup.sh`, which puts the fullcone NAT kernel,
libnftnl, nftables and fw4 patches and the LuCI options into the tree. It is
CI-only like Argon and tracks master the same way, with the commit each build
used recorded in `build-info.txt` as `fullcone_commit`. That script skips
anything it cannot place and still exits 0, so the workflow requires it to
report `0 skipped` and fails the build otherwise — an image that claims fullcone
NAT and has none is worse than a failed build.

To have it locally, run it after `setup.sh` and before building:

```sh
cd openwrt
curl -sSL https://raw.githubusercontent.com/mufeng05/openwrt-sonic-fullcone/master/add_sonic_fullcone.sh | bash
```

**A tree that has been built before needs a full rebuild after adding it**
(`make clean && make -j$(nproc)`). Patch 984 inserts a member in the middle of
`struct nf_conn`, which moves every member after it, and an out-of-tree module
such as ECM that is not rebuilt reads the wrong fields.

### Locally

```sh
git clone https://github.com/openwrt/openwrt -b v25.12.5
git clone https://github.com/mufeng05/openwrt-cmcc-pz-l8-nss pzl8-nss
pzl8-nss/scripts/setup.sh ./openwrt
cd openwrt && make -j$(nproc)
```

`setup.sh` is idempotent and prints everything it touches.

### Choosing your own packages

After `setup.sh` this is an ordinary OpenWrt tree:

```sh
cd openwrt
make menuconfig        # add LuCI apps, tools, whatever you want
make -j$(nproc)
```

`setup.sh` seeds the configuration only when there is **no** `.config`, so
re-running it to pick up new commits will not discard your selection.
`RESEED=1 pzl8-nss/scripts/setup.sh ./openwrt` asks for the repository's own
configuration back. Either way it ends with `make defconfig`, so packages added
upstream get their defaults.

A few symbols have to stay on, and menuconfig will not stop you turning them off
— two of them are not packages at all:

| | |
|---|---|
| `CONFIG_NSS_DRV_WIFIOFFLOAD_ENABLE` | builds the wifili and wifi_vdev half of qca-nss-drv. Without it `ath11k.ko` **will not link** — modpost reports ten undefined `nss_wifili_*` symbols |
| `CONFIG_NSS_FIRMWARE_VERSION_12_5` | picks the firmware blob **and**, through qca-nss-drv's patch 0022, the wifili message ABI. Turning it off still **builds**, but the peer-stats array stride ends up 16 bytes shorter than what the firmware sends |
| `CONFIG_PACKAGE_kmod-qca-nss-drv` | the NSS driver itself |
| `CONFIG_PACKAGE_nss-firmware-ipq50xx` | the firmware blob |
| `CONFIG_LUCI_LANG_zh_Hans` | the Chinese interface. Note that `CONFIG_PACKAGE_luci-i18n-*-zh-cn=y` does **not** work: those are `HIDDEN` symbols, cannot hold a value from a seed file, and defconfig discards them and recomputes from their default |

To freeze your selection into the repository, write it back to the seed:

```sh
cd openwrt
./scripts/diffconfig.sh > ../pzl8-nss/config/cmcc_pz-l8.config
```

The workflow re-checks those symbols after `defconfig`, so a seed that lost one
reports `MISS <symbol>` within seconds rather than dying in modpost forty minutes
later — or shipping an image that built fine and came up in English.

---

## Layout

```
config/          .config seed (diffconfig output)
docs/            engineering notes — start with FINDINGS.md; img/ holds the
                 screenshots the READMEs use
feed/            this project's packages: qca-nss-drv, -ecm, -clients, qca-mcs,
                 nss-firmware, ipq-wifi, qca-ssdk-shell
files/           rootfs overlay: nss-offload init scripts, the LuCI readouts,
                 the Chinese catalogue
manifest/        the QSDK 14.0 revisions these sources are pinned to
po/              LuCI translation source (compiled into the .lmo under files/)
openwrt/
  0001-*.patch   changes to files OpenWrt already ships
  tree/          files OpenWrt does not have, copied in place
  feeds/         patches for packages from other feeds, copied once the feeds
                 exist
scripts/setup.sh applies all of the above to a clean tree
scripts/mkrecovery.py  builds the U-Boot recovery FIT from a factory.ubi
```

## Why the sources are pinned where they are

- **NSS driver / ECM / mcs**: QSDK 14.0 (`NHSS.QSDK.14.0.r9-00040-O`), from
  git.codelinaro.org.
- **NSS clients** (pppoe, qdisc, vlan-mgr): QSDK **12.5.5**. The QSDK 14 client
  feed is PPE-only, and IPQ5018 has no PPE block — Qualcomm's own
  `nss-ppe/Makefile` lists ipq52xx/53xx/54xx/95xx/96xx and nothing else.
- **NSS firmware**: `NSS.FW.12.5-210-MP.R` with the matching wifili ABI. The trap
  is that **the blob cannot be swapped on its own** — qca-nss-drv's patch 0022
  hides four `uint32_t` behind `NSS_FIRMWARE_VERSION_12_5`, and all four sit
  **inside the per-peer array** of the peer-stats message, so choosing the
  firmware means choosing the ABI with it or every peer after the first reads at
  the wrong offset. 12.5 was chosen on measurement: two A/B cycles, Wi-Fi median
  253 to 417 Mbit/s, non-overlapping. Only the 11.4 line still supports mesh.
- **Data plane**: OpenWrt's own `qca-nss-dp` (`syn_gmac_dp`), the same driver the
  vendor image uses. Not the `qca-dwmac-nss` shim, which is for trees that move
  ipq50xx onto upstream stmmac; this is not one of those.

## Credits

The QSDK sources are Qualcomm's, from <https://git.codelinaro.org/clo/qsdk>.

The packaging skeleton and a great many kernel 6.x fixes come from the community
NSS feeds, all GPL:

- Julius Bairaktaris — <https://github.com/JuliusBairaktaris>. `nss-packages`
  originates there, and so does `openwrt-nss-edma`, the tree this project reads
  against throughout `docs/WIFILI.md`. The iproute2 NSS qdisc and nssmirred
  patches in `openwrt/tree/`, and several qualcommax kernel patches, carry his
  Signed-off-by too.
- Stanislaw Pal (kuncy7) — <https://github.com/kuncy7>. This project checks both
  trees out from his forks.
- Sean K (qosmio), who repackages the NSS firmware blobs —
  <https://github.com/qosmio/qca-sdk-nss-fw>
- CrazyBoyFeng — <https://github.com/CrazyBoyFeng/openwrt-pz-l8>, where the
  FM25LS01 flash patch comes from.
- The SONiC fullcone NAT kernel patch is by Akhilesh Samineni (Broadcom), ported
  to OpenWrt by
  [openwrt-sonic-fullcone](https://github.com/mufeng05/openwrt-sonic-fullcone).

The ath11k ring-shrinking approach follows
<https://github.com/openwrt/openwrt/pull/21495>, which was not merged.

---

## Licence

The top-level [LICENSE](LICENSE) is GPL-2.0-only, which is the licence of this
project's own code. The repository is mixed, though, and the
`SPDX-License-Identifier` at the head of each file is what governs:

| | licence |
|---|---|
| this project's own code (`feed/nss-wifili-probe/src/`, `ipq5018-nss.dtsi`) | `GPL-2.0-only` |
| the ath11k NSS offload patch (`991-ath11k-nss-wifili-offload.patch`) | `BSD-3-Clause-Clear`, following QSDK and mainline ath11k |
| patches to files OpenWrt or Linux already ship | whatever the patched work is under |
| package Makefiles under `feed/` | GPL-2.0, as OpenWrt's packages are |

Two things worth stating:

- **Qualcomm's QSDK sources are not in this repository.** `feed/` carries
  Makefiles and patches; the sources are fetched at build time from
  git.codelinaro.org, pinned to the commits listed in the previous section.
- **The NSS firmware is a binary blob and is likewise not in this repository.**
  It is downloaded at build time from
  [qosmio/qca-sdk-nss-fw](https://github.com/qosmio/qca-sdk-nss-fw) and carries
  its own distribution terms.
