# Engineering findings

Everything here was measured on a CMCC PZ-L8, not inferred. Where a claim rests
on reading rather than measurement it says so.

Hardware: IPQ5018 (2× Cortex-A53, 256 MB) + QCA8337 switch on MDIO 0x11 +
QCN6122 5 GHz radio. Switch port 5 ←MDI→ IPQ5018 internal GE PHY → GMAC0
(`eth0`, LAN); port 6 ←SGMII→ uniphy → GMAC1 (`eth1`, WAN).

---

## 1. Why DSA had to go

The upstream board support drives the QCA8337 with mainline DSA. That caps this
device at ~502 Mbps with the host CPU pegged, and NSS cannot fix it:

- qca8k's 2-byte tag sits at offset 12, so NSS reads `0x80xx` as the ethertype
  and exceptions 100 % of traffic to the CPU
- ECM cannot build an interface hierarchy over DSA user ports
- DSA binds one conduit, so GMAC0 is simply wasted

Tell-tale symptom of the tag: `eth1 mtu 1502`.

Replacing DSA with `qca-ssdk` + swconfig gives two independent GMACs at MTU
1500 — the precondition for NSS being able to parse L3 at all.

## 2. Two real defects in qca-ssdk

Both are fixed in `openwrt/tree/package/kernel/qca-ssdk/patches/`.

### 2.1 MDIO register access through a vendor hook that does not exist

`qca_mii_raw_read()/_write()/_update()` call `sw_read`/`sw_write` out of
`struct qca_mdio_data` — QSDK's own qca-mdio driver private data, offsets
168/176. OpenWrt builds mainline `CONFIG_MDIO_IPQ4019=y`, whose `bus->priv` is a
**32-byte** `struct ipq4019_mdio_data`. Fetching the hooks reads ~136 bytes past
the allocation and then *calls whatever is there as a function pointer*, holding
`bus->mdio_lock`.

MDIO wedges; procd has already armed the watchdog before preinit, so the board
reboot-loops. Measured: ~73 s period, `d_boots` reached 468 over 9.5 hours, no
link on any port.

Device 0 (the internal ESS) is unaffected because it uses
`switch_access_mode = "local bus"`. **This is why upstream OpenWrt ships
`ISISC_ENABLE=disable`** — with the mainline MDIO driver, ssdk cannot reach an
MDIO-attached switch at all.

Fix: implement the register protocol directly, the way mainline
`drivers/net/dsa/qca/qca8k-8xxx.c` does for this silicon — page to PHY 0x18
register 0, then the value as two 16-bit halves, low first.

### 2.2 uniphy forced speed cleared by the port reset

`adpt_mp_uniphy_mode_ctrl_set()` sets `ch0_force_speed`, then
`adpt_mp_gcc_uniphy_port_reset()` power-cycles the port, returning
`UNIPHY_CHANNEL0_INPUT_OUTPUT_4` to its `0x844` reset default and clearing the
bit that was just set.

It only matters when the SGMII peer is *also* forced. Here the peer is the
QCA8337's port 6, forced to 1000/full by `qca,ar8327-initvals`, so it sends no
auto-negotiation code words while the uniphy waits for one. Both ends report a
link and not one frame crosses.

Found by dumping the register after a full init and seeing exactly `0x844`.

## 3. The NSS data plane failure — and its actual cause

Loading `qca-nss-drv` left the box reachable but with **eth0 carrierless**:
`nss_dp_open()` bails at `data_plane_ops->open()` before `phy_start()`, so the
PHY is never brought up. `n2h_h2n_data_pkts` and `n2h_n2h_data_pkts` stayed 0
while the firmware happily sent 30 000 control messages.

Chasing it produced three wrong answers before the right one, all discarded on
evidence:

| Hypothesis | Killed by |
|---|---|
| Clock indices wrong | DTB indices 98/104–109 match mainline `gcc-ipq5018.c` exactly |
| Driver/firmware ABI mismatch (QSDK 14 vs FW 12.2) | The upstream feed pairs the *same* driver commit with the same firmware |
| Missing `extra_pbuf_core0` | The sysctl's `-EINVAL` turned out to mean *the firmware did not answer in time*, not *bad value* — a symptom, not the cause |

**The cause**: three NSS driver features were disabled in the build, and the
12.2 firmware streams stats for them unconditionally.

```
# CONFIG_NSS_DRV_CRYPTO_ENABLE is not set    -> interface 167  NSS_CRYPTO_CMN
# CONFIG_NSS_DRV_LSO_RX_ENABLE is not set    -> interface 178  NSS_LSO_RX
# CONFIG_NSS_DRV_UDP_ST_ENABLE is not set    -> interface 224  NSS_UDP_ST
```

with `NSS_SPECIAL_IF_START = DYNAMIC_IF_START(28) + MAX_DYNAMIC(128) = 156`.
Those are exactly the three interface numbers that were flooding
`Callback not registered for interface N` — a 3-for-3 match. Every such message
hit `cb == NULL` and was dropped, at ~2500/s, drowning the N2H ring so the
firmware could not even answer `NSS_PHYS_IF_OPEN`.

nwrt's `stats/` directory has `lso_rx` and `udp_st` nodes; ours did not. That
difference is what pointed at the flags.

After enabling them:

| | before | after |
|---|---|---|
| `n2h_rx_queue[0]_drops` | 897 | 0 |
| `n2h_payload_alloc_fails` | 2547 | 5 |
| `Callback not registered` | flooding | none |
| eth0 | no carrier | link up, 944 Mbps |

## 4. Buffer tuning comes from the vendor's memory profile

`lib/wifi/qcawificfg80211.sh` in the vendor firmware reads the DT marker
`MP_256` / `MP_512` and scales NSS accordingly:

```sh
# extra pbuf core0 = (high_water_core0 - (NSS + OCM buffers)) * pbuf_size
# where NSS+OCM buffers = 11720 and pbuf_size = 160 bytes
sysctl_cmd dev.nss.n2hcfg.extra_pbuf_core0      800000
sysctl_cmd dev.nss.n2hcfg.n2h_high_water_core0  16336
```

Our board reports `n2h_pbuf_def_total_count 9984` + `ocm 1500` = 11484, matching
the vendor's 11720 constant. `nss-offload` applies the MP_256 values, and the
kernel log then matches nwrt's line for line:

```
qca-nss 7a00000.nss: Additional pbufs of size 802816 got added to NSS
```

## 5. Memory: why ath11k does not fit

ath11k sizes its DP rings from compile-time constants in `dp.h` that are the
same for every chip and tuned for IPQ8074/IPQ6018 boards with 512 MB–1 GB.
Measured cost on this 256 MB board: **MemAvailable 87 MB → 7.7 MB** the moment
both radios finish QMI init.

The cost is the buffer pools, not the descriptors — a descriptor is ~16 bytes, a
RXDMA ring entry is backed by a ~2.7 KB skb, a 170× difference. IPQ5018 also
sets `.rxdma1_enable = true`, so the monitor rings are really allocated.

The vendor has no such problem because **its ring sizes are runtime INI values
scaled by the same MP_256 profile**, and — more fundamentally — with wifi
offloaded to NSS the Rx buffers come out of NSS's pbuf pool instead of the host
skb allocator. `extra_pbuf_core0` and `dp_nss_comp_ring_size` are set in the same
branch of that script because they are two halves of one memory budget.

Reducing the rings (values from PR #21495, plus `DP_RXDMA_MONITOR_DESC_RING_SIZE`
which that PR misses) recovers ~36 MB:

| | MemAvailable |
|---|---|
| stock rings + NSS + both radios | 1.6 MB — reboot loop |
| reduced rings, same load | 18–20 MB |

## 6. Why WiFi needed an offload of its own

Accelerating the wired path does not carry WiFi with it. The reason is worth
keeping, because it is what forced the `nss-wifili` work, and because the
numbers below are what that work has to be measured against.

NSS acceleration works by taking `eth0`/`eth1` away from the kernel
(`nss_dp_override_data_plane`). For traffic NSS forwards itself, the packet
never enters Linux: ~78 000 pps, 944 Mbps, 0 % host CPU.

A packet the *host* injects takes a different route — descriptor write, DMA
sync, doorbell, UBI32 transmit, buffer returned through the empty-buffer queue,
**interrupt back to the host**. Measured during a wifi transfer, before the
radios were offloaded:

```
tx packets          +108 468 over 14.2 s   ->  7 660 pps
nss_empty_buf_queue  +82 692                ->  5 840 /s
nss_queue0           +47 569                ->  3 359 /s
                                    1.2 interrupts per packet
```

That per-packet round trip caps host-injected traffic at ~7 700 pps ≈ 92 Mbps,
which is exactly where wifi forwarding landed. It was *worse* than no NSS at all
(308 Mbps), because the native `syn_gmac` path batches completions through NAPI
while the NSS handoff does not.

ECM cannot rescue it: its NSS front end needs an NSS interface number for every
interface in the path, `phy1-ap0` has none, so ECM never even attempts
acceleration (`pending_accel = 0`, not a failed attempt).

Two things were tried against that ceiling. Both are obsolete now and are kept
because they say where the cost was:

- **ECM redirect off** (`dev.nss.general.redirect=0`): 72 vs 70 Mbps. Not ECM.
- **GRO off** on the wifi netdevs: 70.8 → 92.1 Mbps. The non-linear skbs were
  100 % GRO-produced (`nr_frags` +8232 → +10), and NSS spends a descriptor and
  a DMA map per fragment. That did not beat the ceiling, it only stopped wasting
  the round-trip budget on 515 giant skbs/s instead of 7 983 normal ones, and it
  cost ~30 % of local wifi throughput (641 → 449 Mbps).

The only way past it is the one the vendor takes: register the radios with NSS
(`nss-wifili`), so wifi Rx happens *inside* NSS and the packet goes
WLAN ring → NSS → eth1 without touching the host, structurally identical to the
wired path. nwrt, measured on this same hardware, gets **~624 Mbps at 25–35 %
CPU** that way (from `eth1_tx` deltas, ~78 MB/s).

**That is what this build now does**, and it is the single largest thing in it.
`nss-wifili-probe` hands each radio's data path to NSS once ath11k has brought
it up — both radios, at boot, with nothing to run by hand:

```
nss: handing the data path of slot 0 over
nss: slot 0 data path is NSS-owned        c000000.wifi  (2.4 GHz)
nss: slot 1 data path is NSS-owned        b00a040.wifi  (5 GHz)
```

Measured after: **697 Mbit/s on 5 GHz at 160 MHz with 2.9 % host CPU**, against
the 92 Mbps ceiling above. The full derivation — what had to be right, the
per-radio handover, the Rx descriptor pool, and the firmware ABI it is tied to —
is in [WIFILI.md](WIFILI.md).

The source situation that made this look impossible was real, and is why it took
what it did: `nss_wifili`'s NSS-side API is fully open in `qca-nss-drv`, but the
driver-side glue is not. `qca-wifi-oss` in the QSDK 14 manifest is 75 files of
crypto/qal/wmi headers with no `osif_nss` or `wifili` anywhere, and QSDK 14's
`nss-clients` is entirely PPE-oriented. What made it tractable is that the ring
descriptions map: ath11k's `struct hal_srng` carries the same
`ring_base_paddr`/`entry_size`/register bases that `nss_wifili_hal_srng_info`
wants. mac80211 coexistence and debugging against a firmware blob were the hard
parts, as expected.

## 7. Every accelerated connection's statistics were being discarded

Noticed on a macvlan: 436 MB through `eth1` moved the counter on the macvlan
over it by 2.9 MB. The offload helper that folds an accelerator's counts into a
macvlan was the obvious suspect and was not at fault — it adds exactly what ECM
hands it, and ECM had nothing to hand it. ECM's own database agreed:
`adv_stats.to_data_total` read **197 bytes** for a flow that had moved half a
gigabyte.

NSS reports per-connection counters only when polled. ECM polls once a second,
walking the firmware's connection table in chunks — `nss_ipv4_conn_sync_many_msg`,
`index` in, `count` and `next` out — and feeds each returned record to
`ecm_interface_stats_update()`, which is what advances conntrack, the ECM
database, and the counters of every virtual interface in the path. Instrumented,
546 polls across twelve seconds of gigabit traffic returned ACK with
**`count = 0` every time**, `next` stepping 0, 40 … 2040 and round again: the
whole table, ten times over, reporting nothing.

The count was not zero on the wire. `qca-nss-drv` clamped it before ECM saw it.
Patch 0112 bounds `count` by `ncm->len`, and the firmware leaves `ncm->len` at
the request length, so the capacity computes to zero and every count clamps to
zero — silently, because `nss_warning` is compiled out unless the debug level is
raised. `ncm->len` cannot be made to work either: the length check above that
switch rejects any message longer than `struct nss_ipv4_msg`, which has no room
for the array.

`size` is the field that describes the buffer. It is a *request* field, filled
in by the host and returned untouched, and `nss_core_send_cmd()` shows why it is
the right one: the request is an skb of `max(size, buf_size)` — ECM asks for
`PAGE_SIZE` — sent with `H2N_BIT_FLAG_BUFFER_REUSABLE`, so the response comes
back in that same buffer. Capping at `PAGE_SIZE` makes the bound the allocation
itself, since `nss_core_send_cmd()` refuses to send anything larger.

Measured on the fixed build, 500 MB through the macvlan, idle to idle:

| | before | after |
|---|---|---|
| macvlan bytes vs `eth1` bytes | 2.9 MB of 436 MB (0.7 %) | 538.7 MB of 545.2 MB (98.8 %) |
| macvlan packets vs `eth1` packets | — | 359 164 of 359 276 (99.97 %) |
| ECM `adv_stats.to_data_total` | 197 B | 1.11 GB |
| conntrack on an accelerated flow | frozen | advancing |

The remaining 1.2 % is the Ethernet header: ECM's counts are L3 bytes, and
folding them into an L2 device loses 14 bytes a packet — `eth1` averages 1517
bytes/packet over the same traffic, the macvlan 1500.

None of this was macvlan-specific. Anything whose counters ECM has to fold in by
hand — conntrack accounting, PPPoE, VLAN, bridge — read near zero for
accelerated traffic.

## 8. 160 MHz means DFS, and DFS costs more than the CAC

No 160 MHz block in 5 GHz avoids radar channels: 36–64 contains 52–64, 100–128
is entirely DFS, and 149–165 tops out at 80 MHz here. Running the default HE160
therefore means running with radar detection enabled, and two things follow that
are not the 60-second CAC everybody expects.

**A radar event drops the radio to 80 MHz and nothing puts it back.** hostapd
announces a switch to the non-DFS half and stays there. Measured with
`dfs_simulate_radar` in ath11k's debugfs:

- the 30-minute non-occupancy period expiring does **not** bring it back;
- `wifi reload` does not either — the log shows only "Reloaded settings", it
  never re-selects a channel and never starts a CAC;
- a runtime channel switch is refused: `switch_chan` to 160 MHz returns
  "(extension) channel is disabled", because after the NOP those channels are
  *usable* again but not *available* until another CAC, and a channel switch
  announcement cannot run one.

What works is restarting that one radio, which re-runs CAC: 64 s measured from
`DFS-CAC-START` to `DFS->ENABLED`, with only that band down. **Do not try it
while the NOP is still running** — hostapd then fails to start rather than
falling back, and 5 GHz is left down entirely.

A watchdog that waited out the NOP and then restarted the radio was written and
deliberately dropped: a minute without 5 GHz is worse than sitting at 80 MHz for
anyone with traffic on it, and where radar is genuinely present, retrying every
half hour is worse still.

**Scanning is blocked outright.** mac80211 refuses a scan while the channel
context has radar detection enabled — the radio cannot leave a channel it has
passed CAC on without having to redo it:

| radio1 | `iw dev phy1-ap0 scan` |
|---|---|
| ch36 HE160, centre 50, block 36–64 (DFS) | `Resource busy (-16)`; `iwinfo`: "No event received" |
| ch36 HE80, centre 42, block 36–48 (no DFS) | returns neighbours normally |

That is why LuCI's channel analysis shows nothing but the local AP on 5 GHz
while 2.4 GHz lists everything in range. It is not a LuCI fault and there is no
fix short of not using 160 MHz. To survey the band, drop radio1 to HE80 on
36–48 or 149–161, scan, and put it back.

## 9. macvlan for WAN multi-dial

Several dial-up sessions over one WAN port means one macvlan per session, and
without acceleration that traffic falls back to software forwarding. It works
here, under two conditions.

**The kernel needs two helpers QSDK has and OpenWrt does not.** ECM calls
`macvlan_get_mode()` and `macvlan_offload_stats_update()`; neither exists
anywhere in 6.12, so selecting `kmod-macvlan` made `ecm_interface.c` fail with
two implicit-declaration errors and stopped the build. Both are small enough to
live in `include/linux/if_macvlan.h` beside `macvlan_count_rx()` — `struct
macvlan_dev` already exposes the mode and the per-CPU stats — and are added by
`620-macvlan-add-offload-helpers.patch`.

**ECM accelerates `mode private` and nothing else.**
`ecm_interface_macvlan_mode_is_valid()` returns true for `MACVLAN_MODE_PRIVATE`
and false for bridge, vepa and passthru, with no knob anywhere:

```
config device
	option type    'macvlan'
	option name    'wan2'
	option ifname  'eth1'
	option mode    'private'
```

Measured: the macvlan takes its own DHCP lease, the server on the far side sees
every connection sourced from the macvlan's address rather than the parent's,
and a single stream runs at 988 Mbit/s with 2.17 % host CPU and the connections
present in ECM's database — accelerated, not falling back.

## 10. Traps worth remembering

- **Backup files inside `base-files/` ship to the device.** A stray
  `01_leds.pristine` was executed by `/bin/board_detect`, which globs
  `/etc/board.d/*`, and its stale entry won because it sorts after the real
  script. Keep backups out of the tree. (Cost this project two rounds — once as
  `02_network.orig`, once as `.pristine`.)
- **Partition labels are lowercase here** — `0:art`, not `0:ART`.
  `caldata_extract` against the wrong name fails silently.
- **`sysupgrade` reporting "Connection failed" is normal**: procd severs the
  ubus connection as it takes over. The flash does happen.
- **A watchdog script must not call anything that can block.** `sync` hangs on a
  wedged filesystem and `ping` hangs on a wedged network; both defeated a
  deadman timer that then never fired. `echo b > /proc/sysrq-trigger` only.
- **`pgrep -f <pattern>` matches the script's own command line** when the script
  is passed to `sh -c`, so `kill` becomes suicide. Use a PID file. The same
  self-match makes `pgrep -f 'make -j8'` report a build as still running long
  after it finished.
- **Do not drop a locally built kernel module into a released image.** A
  `qca-nss-drv.ko` built here and copied over the one in a running release
  rebooted the board the moment traffic was accelerated — reproducibly, with two
  different variants of the change under test, and again with that change
  reverted, so it was not the change. The stock modules never did it, and a
  whole image built from the same tree never did it either. The one concrete
  difference found is that the module in `build_dir` is unstripped, 6.1 MB
  against the packaged 410 KB; the mechanism was never established, because the
  oops cannot get out — the network is what dies, and this image has no
  `netconsole`. Build the image. It takes eight minutes on a warm tree.
- **Once `nss-offload` has self-disabled, `/etc/init.d/nss-offload start` will
  not bring it back.** `nss_load()` returns early when `qca_nss_drv` is already
  present, and ath11k pulls it in at boot whatever the flag says, so `start`
  quietly does nothing but re-apply the pbuf tuning. Remove `/etc/nss-disabled`
  *and* `/etc/nss-boot-pending`, then reboot.
