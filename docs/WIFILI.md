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

```
# once, before ath11k probes - shipped as files/etc/modules.d/29-ath11k-nss
ath11k nss_refill_hold=3 frame_mode=2

# after the NSS core and qca-nss-drv are up
sh scripts/arm-wifili-nss.sh
```

## Known limitations

- A held radio cannot receive until NSS takes the ring over, so its boot-time AP
  fails with `failed to vdev 0 create peer for AP: -110` and only comes up when
  the arm script runs `wifi up`. A production form should either clear the hold
  if NSS has not claimed the slot, or drain ath11k's buffers and flush the
  hardware cache at handover instead. Re-announcing the ring over HTT does
  **not** flush it — tested.
- `rx_desc_alloc_fail` climbs under load. `PROBE_RX_SW_DESC_NUM` is 1024 where
  the vendor reference runs 4096; raising it via the run line used to trap
  immediately, which is worth re-testing now that the buffer-ownership bug is
  gone.
- The two radios were tested one client at a time, not with a client on each
  simultaneously.
- `auth_early=1` authorises the peer at create time instead of after the
  handshake. It is a shortcut, not a fix.
- The probe is a diagnostic harness with a lot of knobs, several of which exist
  only to have refuted something. It is not a driver.
