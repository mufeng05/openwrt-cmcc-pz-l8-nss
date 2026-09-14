# WiFi NSS offload on the PZ-L8

Both radios run through the NSS core: the packets a station sends to the WAN,
and the packets coming back, are forwarded by the UBI32 core and never enter
Linux.

## Measured

Forwarded path only — a client on WiFi, through the router with NAT, to a wired
host on the WAN side. That is what NSS accelerates; a `dd | nc` sourced on the
router itself measures its own userspace and says nothing about the offload.

iperf3 `-P 4`, 15 s per direction — the same shape as the wired numbers in
the README, so the two are comparable.

| | Down | Up | Router CPU, loaded / idle baseline |
|---|---|---|---|
| 5 GHz (QCN6122, HE80 2×2, 1201 Mbps link) | 438 Mbit/s | 510 Mbit/s | 4.2 % / 4.0 % |
| 2.4 GHz (IPQ5018, 287 Mbps link) | 61 Mbit/s | 58 Mbit/s | 4.3 % / 3.9 % |
| wired, for reference | 949 Mbit/s | 949 Mbit/s | 4.5 % / 5.5 % |

One association each, `tx_fail 0`, zero NSS traps, zero Oops. The host sees
about 100 frames per gigabyte — everything else is forwarded inside NSS.

Both halves of the CPU figure are given because the per-second sampler costs
2–5 % on these cores by itself; what forwarding adds is the difference, 0.3 %
on either radio and nothing measurable on wired.

`next_hop=158` (`NSS_ETH_RX_INTERFACE`) is not optional. Left unset, four runs
gave 413 up / 320 down with the downlink scattered from 246 to 373; set at
module load, four runs gave 448 up / 406 down with the downlink inside 383–418.
The reference implementation sends the same
`nss_wifi_vdev_base_set_next_hop(ctx, NSS_ETH_RX_INTERFACE)` at the end of
`ath11k_nss_setup()`.

A single stream gets 226–337 down and 121–197 up on 5 GHz at the same CPU.
That is the stream, not the offload. An earlier revision of this file quoted
those single-stream numbers under a 4-stream heading.

The CPU is not the limit at these rates; the air and the client are.

## The two things that had to be right

### 1. ath11k must not fill the Rx refill ring

`ath11k_dp_pdev_alloc()` fills it at core start, from
`ath11k_core_qmi_firmware_ready()` — long before anything sets
`nss_offload_mask` from userspace, and regardless of whether the radio is
disabled in uci. The RXDMA hardware caches sixteen of those entries.

NSS then replenishes the ring with its own buffers, and the ring reads back as
pure NSS — 1023 entries with RBM 3 and cookies 1..1023 — but the hardware still
spends its sixteen cached **host** buffers on the first sixteen frames it
receives. Those arrive in REO with RBM 6 and host cookies, NSS resolves the
cookie against its own descriptor pool, finds a slot that was never allocated,
and drops the frame.

That one fact was both symptoms:

- **Rx.** The sixteen dropped frames are the station's M2s. hostapd retries M1
  four times per association and then deauthenticates, so four associations ×
  four retries = sixteen, and the fifth association is the first whose M2
  survives. Raising `wpa_pairwise_update_count` moved it to three associations
  with the same sixteen frames, which is what showed it was buffers and not
  rounds.
- **Tx.** The drop path frees the buffer it could not resolve, pushing sixteen
  foreign buffers onto NSS's own free list. What later comes out of that list is
  not a buffer NSS allocated, and the core traps dereferencing it: Thread 1,
  reason 0x1000, PC 3F006C18, A4 = 0, and the board reboots after a few
  thousand frames.

`nss_refill_hold` (added by the 991 patch) holds the ring empty from module
load, so the hardware has nothing to cache.

The check that this is still right: associate once, dump REO ring 0, look at the
RBM of entry 0. 3 is fixed, 6 is not.

```
echo '<reo_dest0 paddr from the probe log> 256' \
    > /sys/module/nss_wifili_probe/parameters/phys_dump
dmesg | tail
```

Word 1 of each 64-byte entry: `RET_BUF_MGR` is bits 10:8 (3 = NSS's SW0_BM,
6 = ath11k's SW3_BM), `SW_COOKIE` is bits 31:11. `peer_meta_data` is at offset
12 and carries the peer id, which is how the sixteen buffers were tied to the
four failing associations.

### 2. The frame format NSS is told must be the one it gets

The probe tells NSS `DECAP_TYPE = 2` (Ethernet). With ath11k's default
`frame_mode = 1` the frames it received were 802.11 + RFC1042 SNAP, which it
cannot parse, so it matched no rule and excepted every one of them to Linux —
the probe then converted them in software. `frame_mode=2` makes the hardware
decap to Ethernet and the uplink is forwarded inside NSS:

| | before | after |
|---|---|---|
| uplink | 138 Mbit/s at 41 % CPU | 121–152 Mbit/s at 2.8 % |
| host deliveries per 268 MB | 131343 | 11 |

This is also why the three NSS next-hop calls looked like they were being
ignored: `nss_wifi_vdev_base_set_next_hop`, `nss_wifi_vdev_set_next_hop` per VAP
and `nss_wifi_vdev_set_peer_next_hop` per peer are all accepted and all made no
difference, because there was nothing NSS could do with a frame it could not
read.

## Inside the driver

`nss.c` and `nss.h` in ath11k do what `nss_wifili_probe.ko` did, from where
Qualcomm's own driver does it. `ath11k_nss_setup()` sends INIT, PDEV_INIT and
START from `ath11k_core_pdev_create()`, between `ath11k_dp_pdev_alloc()` and
`ath11k_mac_register()`; the vdev is built from `ath11k_mac_op_add_interface()`,
peers follow `ath11k_mac_op_sta_state()`, and `ath11k_dp_tx()` hands its frames
straight to `nss_wifi_vdev_tx_buf()`. State lives on `struct ath11k_base`
instead of in a slot-indexed array, and nothing is reached through a function
pointer.

The two ran side by side on one build for as long as it took to show they
were equivalent, picked by a module parameter:

| 5 GHz | uplink | downlink |
|---|---|---|
| probe, 4 runs | 529 Mbit/s | 412 (373-431) |
| in-driver, 3 runs | 544 | 390 (332-421) |

The uplink is a little higher and the downlink a little lower, both inside the
spread of either path on its own - the probe's own downlink covers 373 to 431.
So the switch and the four hooks it chose between are gone, and the driver is
the only implementation.

Measured without them: 521 / 411 Mbit/s on 5 GHz over three runs, 53/36 on
2.4 GHz, no traps, and 4006 frames reached Linux out of roughly 1.5 GB of
traffic. That last number is the offload working - everything else was
forwarded inside NSS.

### What the port got wrong, and how it showed

Every vdev command reported the same status as the probe's, both peers were
created and authorised, and the four-way handshake completed - and a client
still could not get an address. `reo_reaped` climbed to 239 while
`rx_deliverd` sat at 2.

The two were the EAPOL pair. Everything else arrived on the *extended*
callback with `pkt_type = 14`, `NSS_WIFI_VDEV_EXT_DATA_PKT_TYPE_MCBC_RX`, and
the switch there only accepted NONE, IGMP and WDS_LEARN - so every broadcast a
station sent was freed as if it were a notification. No DHCP discover, no ARP.

That callback carries both frames NSS is excepting and notifications wearing an
skb, so it does need a type check; it just needs the right one. MCBC_RX is
what `MCBC_EXC_TO_HOST` produces, and 4ADDR and EAPOL are frames too.

Finding it took counters rather than reasoning, because `ath11k_dbg()` compiles
to nothing without `CPTCFG_ATH11K_DEBUG` and this build does not set it - the
whole peer and vdev path was silent. `nss_rxstats` is what those counters
became and is worth keeping:

```
cat /sys/module/ath11k/parameters/nss_rxstats
data=58 ext=226 noab=0 short=0 eapol=2 up=282 extdrop=0
```

`extdrop` non-zero with traffic flowing is this bug returning.

### What is left of the probe

`nss_export.c` stays: it reads ath11k's ring layout into the flat form the NSS
messages want, and that is as useful to an instrument as to the driver.
`nss_wifili_probe` keeps it and can still drive the whole sequence by hand
through its debugfs interface, which is how most of what is known about this
firmware was established. What it can no longer do is attach itself to ath11k's
events and own the data path - the four function pointers that let it are gone,
and so is the 90 s wait for a module to turn up and claim them.

It is not in the default image. It is kept because its comments record several
dozen hypotheses that were tested and refuted, and that is worth more than the
code around them.

## The Rx descriptor pool

`PROBE_RX_SW_DESC_NUM` is what the probe puts in `num_rx_swdesc` in PDEV_INIT,
and it sizes the firmware's Rx software descriptor pool. It sat at 1024 —
ath11k's own refill ring size — as a bisection, because the round that first
raised it to 4096 also changed `num_tx_desc` and `init_flags`, and that round
moved the crash from traffic time to association time. 1024 put the crash back
where it had been, so 4096 stayed under suspicion.

It was innocent. Both crashes were ath11k's sixteen cached refill buffers, and
with `nss_refill_hold` in place 4096 was re-run against 1024 on the 5 GHz radio,
six iperf3 `-P 4` runs per value, each value armed from a fresh boot twice:

| | uplink | downlink | `rx_desc_alloc_fail` |
|---|---|---|---|
| swdesc 1024 | 477 Mbit/s | 482 Mbit/s | 387k–461k per run |
| swdesc 4096 | 488 Mbit/s | 446 Mbit/s | **0** |

Zero traps either way, twelve runs over four boots.

**The throughput columns are noise.** Run to run the uplink spread is 110 Mbit/s
against a 10 Mbit/s difference in means, and the downlink mean moves the wrong
way. Pool size does not buy rate here.

**The counter goes to exactly zero,** on every run, and stays there over 1.7 M
cumulative frames. It never cost a frame in the first place — `rx_deliverd`
tracked `reo_reaped` to within ten packets at 1024 as well — so the exhaustion
is on the replenish side and the firmware recovers from it. 4096 is kept
because it is the value both vendor formulas give for a radio that is not the
internal 2.4 GHz one, because an error counter that is always nonzero cannot
warn about anything, and because it is free: the pool is firmware side and
MemAvailable is 21 MB either way.

So this is not where the gap to nwrt's ~624 Mbit/s is.

## Both radios at once

Everything per-radio in the probe is per slot: `probe_ss[PROBE_MAX_SOC]` holds
the wifili interface, radio interface, vdev, netdev, peer table and peer memory.
The control path picks a slot on entry and reaches it through `probe_cur`; the
data path cannot, because Rx and Tx run on both radios at once, so Tx takes its
slot from the hook context and Rx from the netdev NSS passes it.

Three things bit here, each with its own signature, and all three are the same
mistake — one number for the whole module where there should be one per slot:

- one netdev name, so slot 0 registered against the dummy netdev and every frame
  excepted on that radio went nowhere;
- `probe_peer_hook()` not selecting a slot, so the 2.4 GHz peers were created on
  the external wifili instance and the internal one reaped the station M2s and
  delivered none (`reo_reaped 20, rx_deliverd 0`);
- EAPOL handed to `ath11k_nss_dp_rx_eapol(probe_slot, ...)`, so the 2.4 GHz M2
  went into the 5 GHz control port — the probe counted `eapol=20` and hostapd on
  `phy0-ap0` saw nothing and deauthenticated.

Resolving the netdev by MAC instead of by name was tried and is **not** safe:
this runs from ath11k `add_interface`, `dev_getbyhwaddr_rcu()` can return a
netdev that is still being registered, and `dev_hold()` on it faults on
`pcpu_refcnt` at +0x528.

## How to run it

Nothing. It arms itself at boot, on both radios, and the radios come up
offloaded the first time — there is no window in which they run on the host
path and no `wifi down` / `wifi up` cycle.

Four files in `files/` do it, and the whole mechanism is load order plus
parameters:

```
etc/modules.d/32-qca-nss-drv       qca-nss-drv
etc/modules.d/34-ath11k-nss        ath11k nss_offload_mask=3 nss_refill_hold=3 frame_mode=2
etc/modules.d/35-nss-wifili-probe  nss_wifili_probe vdev_netdev0=phy0-ap0 vdev_netdev1=phy1-ap0 next_hop=158
etc/init.d/nss-failsafe            START=05
```

kmodloader walks `/etc/modules.d` in name order, so: `qca-nss-dp` (31, stock)
→ `qca-nss-drv` (32) → `ath11k` with its parameters (34) → the probe (35) →
`ath11k_ahb`, which has no numeric prefix and therefore sorts last. That last
one is what actually probes the hardware, so by the time ath11k's QMI worker
reaches `ath11k_core_pdev_create()` the NSS core has been up for three seconds
and the probe has already registered its hook.

Two of those positions are load-bearing rather than tidy. `qca-nss-drv` must
follow `qca-nss-dp`, because it takes over nss-dp's data plane and reopens
eth0/eth1 through NSS — that is the order the wired offload was brought up in
and the only one that has been tested. And ath11k must follow `qca-nss-drv`,
because ath11k now links against it: kmodloader would otherwise pull
qca-nss-drv in as a dependency at ath11k's position and put it *before*
nss-dp.

A measured boot:

```
21.33  NSS core 0 booted successfully
21.57  WIFILIPROBE: auto-start armed; ath11k drives the handover
24.74  nss: handing the data path of slot 1 over
25.82  nss: slot 1 data path is NSS-owned
26.89  nss: slot 0 data path is NSS-owned
35.03  slot 0 netdev 'phy0-ap0' resolved (held)
37.79  slot 1 netdev 'phy1-ap0' resolved (held)
```

`ath11k_nss_dp_ready()` will wait up to 90 s at that point if the probe is not
loaded yet, so the ordering is a latency optimisation rather than a
correctness requirement — but it is what keeps WiFi registration on time.

`scripts/arm-wifili-nss.sh` is still there for a box that booted disarmed. It
refuses to run if the probe is already loaded, which on a normal boot it is.

### If a boot goes wrong

`nss-failsafe` runs at START=05, before kmodloader, and arms
`/etc/nss-wifi-pending`. `nss-offload` clears it once `eth0` has carrier. A
boot that hangs, or that comes up with the LAN dead, never clears it — so the
next boot finds it still armed, rewrites `34-ath11k-nss` to `ath11k
frame_mode=2` and writes `/etc/nss-wifi-disabled`. One bad boot disarms the
WiFi handover without needing anything to work.

    rm /etc/nss-wifi-disabled && reboot        # re-arm

It is deliberately a separate pair of flags from the ones `nss-offload` keeps
for the ECM stack: the wired offload works and is not what changed, so a bad
WiFi boot must not switch it off.

## Known limitations

- A radio whose handover *fails* is worse off than one that was never offloaded:
  `nss_refill_hold` is a load-time parameter, so the ring stays empty even
  after `ath11k_nss_dp_ready()` clears the slot from the offload mask, and the
  radio cannot receive at all. It has not happened since the ordering above was
  put in place — the NSS core is up 3 s before ath11k asks — but the failure
  mode is real. The fix is for the failure path to refill the ring, not just
  clear the hold; re-announcing it over HTT does **not** flush the hardware
  cache, which was tested.
- `next_hop` must be set when the module loads. Writing 158 to it at runtime
  and recreating the vdevs crashed the board. Set at load time it is stable and
  worth 27 % of the downlink — see below.
- The two radios were tested one client at a time, not with a client on each
  simultaneously.
- `auth_early=1` authorises the peer at create time instead of after the
  handshake. It is a shortcut, not a fix.
- The probe is a diagnostic harness with a lot of knobs, several of which exist
  only to have refuted something. It is not a driver.
