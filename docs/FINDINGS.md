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

## 6. Why WiFi forwarding is slow with NSS

This is the one significant thing that is **not** fixed, and it is structural.

NSS acceleration works by taking `eth0`/`eth1` away from the kernel
(`nss_dp_override_data_plane`). For traffic NSS forwards itself, the packet
never enters Linux: ~78 000 pps, 944 Mbps, 0 % host CPU.

A packet the *host* injects takes a different route — descriptor write, DMA
sync, doorbell, UBI32 transmit, buffer returned through the empty-buffer queue,
**interrupt back to the host**. Measured during a wifi transfer:

```
tx packets          +108 468 over 14.2 s   ->  7 660 pps
nss_empty_buf_queue  +82 692                ->  5 840 /s
nss_queue0           +47 569                ->  3 359 /s
                                    1.2 interrupts per packet
```

That per-packet round trip caps host-injected traffic at ~7 700 pps ≈ 92 Mbps,
which is exactly where wifi forwarding lands. It is *worse* than no NSS at all
(308 Mbps) because the native `syn_gmac` path batches completions through NAPI
while the NSS handoff does not.

ECM cannot rescue it: its NSS front end needs an NSS interface number for every
interface in the path, `phy1-ap0` has none, so ECM never even attempts
acceleration (`pending_accel = 0`, not a failed attempt).

Two things were tried and are worth recording:

- **ECM redirect off** (`dev.nss.general.redirect=0`): 72 vs 70 Mbps. Not ECM.
- **GRO off** on the wifi netdevs: 70.8 → 92.1 Mbps. The non-linear skbs were
  100 % GRO-produced (`nr_frags` +8232 → +10), and NSS spends a descriptor and
  a DMA map per fragment. This does not beat the ceiling, it just stops wasting
  the round-trip budget on 515 giant skbs/s instead of 7 983 normal ones. It
  costs ~30 % of local wifi throughput (641 → 449 Mbps), so it is left off by
  default.

The vendor avoids all of this because its wifi driver registers the radios with
NSS (`nss-wifili`): wifi Rx happens *inside* NSS and the packet goes
WLAN ring → NSS → eth1 without touching the host, structurally identical to the
wired path. nwrt measured on this same hardware: **~624 Mbps at 25–35 % CPU**
(from `eth1_tx` deltas, ~78 MB/s).

`nss_wifili`'s NSS-side API is fully open in `qca-nss-drv`. The driver-side glue
is not: `qca-wifi-oss` in the QSDK 14 manifest is 75 files of crypto/qal/wmi
headers with no `osif_nss` or `wifili` anywhere, and QSDK 14's `nss-clients` is
entirely PPE-oriented. Writing that glue against ath11k is a research project —
the ring descriptions map (ath11k's `struct hal_srng` carries the same
`ring_base_paddr`/`entry_size`/register bases `nss_wifili_hal_srng_info` wants),
but mac80211 coexistence and debugging against a firmware blob are the hard
parts.

## 7. Traps worth remembering

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
  is passed to `sh -c`, so `kill` becomes suicide. Use a PID file.
