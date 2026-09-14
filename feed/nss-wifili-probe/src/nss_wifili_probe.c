// SPDX-License-Identifier: GPL-2.0-only
/*
 * NSS wifili bring-up probe.
 *
 * Sends the wifili init sequence built from ath11k's real data-path rings and
 * reports what the NSS firmware makes of it.
 *
 * The first version of this got INIT accepted and then killed the firmware on
 * PDEV_INIT.  Reverse-engineering the vendor's own glue - wifi_3_0.ko from the
 * v1.6 image, whose osif_nss_wifili_soc_init and nss_wifili_pdev_attach are
 * intact apart from renamed locals - found five things wrong with it, none of
 * which the headers hint at:
 *
 *   - hwreg_base has to be the register's PHYSICAL address.  ath11k stores an
 *     offset from the ioremap()ed window; the vendor computes
 *     hwreg_virtual - dev_base_virtual + dev_base_physical.  Sending the bare
 *     offset points the firmware at 0x00a4xxxx instead of 0x0ca4xxxx, and it
 *     writes ring head/tail pointers there once it starts driving the rings -
 *     which is exactly when the firmware died.
 *   - target_type is Qualcomm's TARGET_TYPE (29 for IPQ5018), not ath11k's
 *     hw_rev (7).  The firmware indexes hardware tables by it.
 *   - rx_buf_len is the usable data length, not the buffer size: the vendor
 *     pairs tlv_size 256 with 1792 on a 2048-byte buffer.
 *   - soc_mem_profile is 1 on this exact board; this sent 0.
 *   - scheme_id comes from nss_wifili_thread_scheme_alloc(), which needs a
 *     radio dynamic interface allocated and registered first.  This sent 0.
 *
 * And one thing it should never have done: NSS_WIFILI_LINK_DESC_INFO_MSG is
 * firmware-to-host.  The vendor handles it in osif_nss_wifili_event_receive as
 * "wifili msdu link descriptor info received" and returns the descriptor to
 * hardware.  NSS never needs to be told where the link descriptor banks are,
 * so the earlier worry about ath11k's three banks was moot - and the error 99
 * that message answered with was the firmware having no host-side handler for
 * a type it only ever sends.
 *
 * A firmware crash is expensive to observe: qca-nss-drv panics the host on a
 * coredump unless dev.nss.general.coredump is set to something other than 0 or
 * 1, and NSS owns eth0/eth1, so a dead firmware means no network either way.
 * Hence the stage control - a run can stop one message short of the fatal one
 * and still report from a live box.
 *
 *   echo "<soc> <stage> [swdesc] [quiesce] [memprofile]" > \
 *        /sys/kernel/debug/nss_wifili_probe/run
 *
 *   stage 0  INIT with a deliberately poisoned ring description (control)
 *   stage 1  INIT only
 *   stage 2  + PDEV_INIT
 *   stage 3  + START
 *   stage 4  ONLY the vdev and peer, on a SoC stage 3 already started - it
 *            sends no reset and no init, because doing that to a running
 *            firmware is what broke the first attempt
 *
 *   swdesc      override num_rx_swdesc (0 = ath11k's own pool size)
 *   quiesce     1 = stop the ath11k DP NAPI before PDEV_INIT
 *   memprofile  soc_mem_profile, default 1 (what the vendor sends here)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/netdevice.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include <linux/atomic.h>
#include <linux/io.h>
#include <linux/crc32.h>
#include <linux/kthread.h>
#include <linux/soc/qcom/smem.h>

#include "ath11k_nss_export.h"
#include <nss_api_if.h>
#include <nss_wifili_if.h>
#include <nss_dynamic_interface.h>
#include <nss_wifi_vdev.h>

/*
 * nss_wifili_msg_init() is EXPORT_SYMBOL()ed by qca-nss-drv but never
 * declared in exports/nss_wifili_if.h - the same omission the upstream feed
 * patched for nss_wifili_get_context().  Declare it here rather than patch
 * the driver for a probe.
 */
extern void nss_wifili_msg_init(struct nss_wifili_msg *ncm, uint16_t if_num,
				uint32_t type, uint32_t len, void *cb,
				void *app_data);

#define PFX "WIFILIPROBE: "

static struct dentry *probe_dir;
static struct nss_ctx_instance *probe_ctx;

/*
 * Which ath11k SoC this instance offloads.  Slot 0 is the IPQ5018 internal
 * radio and goes to the internal wifili interface; slot 1 is the QCN6122 and
 * needs an external one - a different interface number and a different
 * dynamic-interface type for its radio, which is the same split the vendor
 * driver makes on target type.
 *
 * A bitmask, not an index: bit 0 is the internal IPQ5018 radio, bit 1 the
 * external QCN6122.  ath11k has already decided which slots are offloaded -
 * that is what its own nss_offload_mask is - and it calls probe_dp_ready()
 * once per slot.  This exists only to be able to take one of them back
 * without rebuilding, so the default is both.
 *
 * Refusing a slot here is not free: ath11k clears that slot from its offload
 * mask and falls back to the host data path, but nss_refill_hold is a
 * load-time parameter and still holds the refill ring empty.  A radio refused
 * here therefore cannot receive at all.  Refuse a slot only together with the
 * matching bit of nss_refill_hold.
 */
static unsigned int probe_slots = 3;
module_param_named(slot, probe_slots, uint, 0444);
MODULE_PARM_DESC(slot,
		 "bitmask of ath11k slots to offload: bit 0 the internal"
		 " radio, bit 1 the QCN6122");

/* moved into struct probe_slot_state */

/*
 * Per-SoC registration state.
 *
 * These were single statics, and probe_run() reused the first registration on
 * every later call, so only one wifili SoC could ever exist.  The stock nwrt
 * boot log on this board registers TWO, internal first:
 *
 *   33.3 s  register wifili function for soc 29  (QCA5018, internal)
 *   35.1 s  register wifili function for soc 30  (QCN6122, external)
 *
 * and with exactly one SoC registered the internal interface works while the
 * external one always traps, for either radio.  If the firmware's external-SoC
 * path indexes a per-SoC structure that assumes soc 0 exists, that is what one
 * would see.  probe_run() now swaps these in on entry and back out at the end,
 * which leaves every other use of the globals untouched - including the peer
 * hook, which runs outside any run and needs the last-run SoC's context.
 */
#define PROBE_MAX_SOC 2

#define PROBE_MAX_PEERS	128

/*
 * Per-slot state, so both radios can be accelerated at the same time.
 *
 * All of this used to be single globals, which limited the probe to one radio.
 * Arming both SoCs already worked - both wifili instances INIT, PDEV_INIT and
 * START cleanly and both show up in the stats file - but only one vdev could
 * exist, so the other radio had no Tx hook, ath11k_nss_dp_tx() returned
 * -ESHUTDOWN for it, and its clients could not complete a handshake.  The peer
 * ids collide as well: both radios number their peers from 3, so a single
 * peer-memory array cannot serve both.
 *
 * The control path - the vif and peer hooks, and probe_run - is serialised by
 * probe_lock and by ath11k, so it can pick a slot on entry and reach it
 * through probe_cur; the macros below keep every existing use of the old names
 * compiling unchanged.  The DATA path must not do that, because Rx callbacks
 * and Tx run concurrently on both radios: those functions take the slot from
 * the context pointer the hook was registered with, or from the netdev NSS
 * hands them, and index probe_ss[] directly.
 */
struct probe_slot_state {
	int			 wifili_if;
	int			 radio_ifnum;
	int			 vdev_ifnum;
	int			 self_peer_id;
	int			 peer_auth_sent;
	bool			 peer_created;
	u32			 peer_id;
	struct net_device	*vdev_ndev;
	struct device		*peer_dma_dev;
	struct nss_ctx_instance	*tx_ctx;
	void			*peer_mem_va[PROBE_MAX_PEERS];
	dma_addr_t		 peer_mem_pa[PROBE_MAX_PEERS];
};

static struct probe_slot_state probe_ss[PROBE_MAX_SOC] = {
	[0 ... PROBE_MAX_SOC - 1] = {
		.wifili_if = -1, .radio_ifnum = -1, .vdev_ifnum = -1,
		.self_peer_id = -1, .peer_auth_sent = -1,
	},
};
static unsigned int probe_cur;

#define probe_wifili_if		(probe_ss[probe_cur].wifili_if)
#define probe_radio_ifnum	(probe_ss[probe_cur].radio_ifnum)
#define probe_vdev_ifnum	(probe_ss[probe_cur].vdev_ifnum)
#define probe_self_peer_id	(probe_ss[probe_cur].self_peer_id)
#define probe_peer_auth_sent	(probe_ss[probe_cur].peer_auth_sent)
#define probe_peer_created	(probe_ss[probe_cur].peer_created)
#define probe_peer_id		(probe_ss[probe_cur].peer_id)
#define probe_vdev_ndev		(probe_ss[probe_cur].vdev_ndev)
#define probe_peer_dma_dev	(probe_ss[probe_cur].peer_dma_dev)
#define probe_tx_ctx		(probe_ss[probe_cur].tx_ctx)
#define probe_peer_mem_va	(probe_ss[probe_cur].peer_mem_va)
#define probe_peer_mem_pa	(probe_ss[probe_cur].peer_mem_pa)

static inline unsigned int probe_slot_of(void *ctx)
{
	unsigned int s = (unsigned int)(uintptr_t)ctx;

	return (s < PROBE_MAX_SOC) ? s : 0;
}

/*
 * Which slot owns this netdev?
 *
 * The Rx path runs in softirq context on both radios at once, so it cannot
 * use probe_cur.  NSS hands the callback the netdev the vdev was registered
 * with, and that is unique per slot, so map back through it.  Returns -1 for a
 * netdev no slot owns.
 */
static int probe_slot_by_dev(const struct net_device *dev)
{
	unsigned int i;

	for (i = 0; i < PROBE_MAX_SOC; i++)
		if (dev && probe_ss[i].vdev_ndev == dev)
			return (int)i;

	return -1;
}

static struct nss_ctx_instance *probe_ctx_soc[PROBE_MAX_SOC];
static nss_if_num_t probe_if_soc[PROBE_MAX_SOC] = {
	NSS_WIFILI_INTERNAL_INTERFACE, NSS_WIFILI_INTERNAL_INTERFACE };
static int probe_radio_ifnum_soc[PROBE_MAX_SOC] = { -1, -1 };
static u8 probe_scheme_id_soc[PROBE_MAX_SOC];
static struct net_device *probe_ndev;

/*
 * The vif's real netdev, held for the life of the vdev.  A reference is taken
 * because NSS keeps calling back into it after the run that created it ends.
 */
/* moved into struct probe_slot_state */

/*
 * If the AP netdev is destroyed while the vdev is still armed - a wifi down,
 * a reconfigure - drop the reference here rather than hold a pointer to an
 * unregistered device until teardown.
 */
static int probe_netdev_event(struct notifier_block *nb, unsigned long ev,
			      void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	unsigned int i;

	if (ev != NETDEV_UNREGISTER || !dev)
		return NOTIFY_DONE;

	for (i = 0; i < PROBE_MAX_SOC; i++) {
		if (cmpxchg(&probe_ss[i].vdev_ndev, dev,
			    (struct net_device *)NULL) != dev)
			continue;
		pr_info(PFX "slot %u vdev netdev %s unregistered - dropped\n",
			i, dev->name);
	}
	return NOTIFY_DONE;
}

static struct notifier_block probe_netdev_nb = {
	.notifier_call = probe_netdev_event,
};
static bool probe_netdev_nb_on;

static DEFINE_MUTEX(probe_lock);

static struct completion probe_done;
static int probe_response = -1;

/*
 * The radio interface, the vdev and the peer memory all have to outlive the
 * write() that created them: the firmware keeps running afterwards and traffic
 * only arrives once the client reassociates.  They are freed on rmmod.
 */
/* moved into struct probe_slot_state */
static u8 probe_scheme_id;
/* moved into struct probe_slot_state */

/*
 * The vdev's netdev comes from ath11k, in vif->netdev, and is used as it
 * arrives - no name, no reference.
 *
 * This replaces three module parameters that named it (vdev_netdev,
 * vdev_netdev0, vdev_netdev1) and a dev_get_by_name() to resolve them.  Two
 * things killed that approach.  phy numbering is not stable: one boot came up
 * with phy0 on the external radio and phy1 on the internal one, the reverse of
 * the boot before, so a hardcoded name silently pointed each slot at the other
 * radio.  And the reference counting was the source of its own crash - two
 * paths could dev_put() the same netdev, the refcount underflowed, the netdev
 * was freed early, and the next access faulted.
 *
 * The old note here said vif->netdev "is sometimes garbage
 * (ffffff3f9dcaf528)".  It is not.  That number is the faulting address, and
 * subtracting the 0x528 offset of pcpu_refcnt leaves a perfectly good netdev.
 * Measured at this point in add_interface: the pointer is valid and the
 * ifindex is already right; it is the name that is still empty and the
 * per-CPU refcount that does not exist yet.  So dev_hold() faults and reading
 * the name returns nothing, but the pointer itself is exactly what is wanted.
 *
 * It matters because registering the dummy instead costs the except path, and
 * EAPOL is excepted: with the dummy, the 4-way handshake never reaches hostapd
 * and the station associates then drops.
 */
MODULE_PARM_DESC(vdev_netdev,
		 "resolve the vdev netdev by this name rather than from the"
		 " vif pointer; empty registers the dummy");

/*
 * The context the Tx hook sends with.  probe_ctx is cleared when a run
 * unregisters the wifili SoC interface, but the hook outlives the run and the
 * NSS core context itself is long-lived - so keep a copy rather than have
 * every frame after the run fail with -ENODEV.
 */
/* moved into struct probe_slot_state */

/*
 * The type number for PEER_UPDATE_AUTH_FLAG.
 *
 * qca-nss-drv 14.0 puts it at 43, but both vendor drivers - the 11.x-era
 * wifi_3_0.ko from v1.6 and the 12.5-era one from nwrt - send 44, and they
 * agree with our header on PEER_SECURITY_TYPE_MSG being 21.  So one message
 * was dropped from the enum between 21 and 43 after the firmware we run was
 * built, and 44 is what a 12.2 firmware expects.  Left adjustable because
 * sending a wrong high-numbered type to the firmware is not a cheap mistake.
 */
static unsigned int probe_auth_msg_type = 44;
module_param_named(auth_msg_type, probe_auth_msg_type, uint, 0644);


/*
 * reoq: send NSS_WIFILI_TID_REOQ_SETUP_MSG for each of a peer's TIDs.
 *
 * This is the one per-peer message the probe has never sent.  ath11k builds
 * the REO queue descriptors itself and programs them into the WLAN firmware
 * over WMI, so the hardware reorders and REO produces descriptors - which is
 * exactly the measured shape of the 5 GHz failure: reo_reaped climbs one per
 * received frame, rxdma_buf_replenished follows it, and rx_deliverd stays at
 * zero with no error or drop counter moving anywhere in the stats file.  A
 * reaped MSDU whose (peer, tid) has no rx_tid in NSS has nowhere to go.
 *
 * reoq_tids is how many entries of the send order to use.  The order starts
 * with TID 16, ath11k's HAL_DESC_REO_NON_QOS_TID, because the 4-way handshake
 * frames are non-QoS data and that is the TID they arrive on; the QoS TIDs
 * follow.  1 therefore sends only the TID the handshake needs.
 */
static bool probe_reoq = false;
module_param_named(reoq, probe_reoq, bool, 0644);
static unsigned int probe_reoq_tids = 17;
module_param_named(reoq_tids, probe_reoq_tids, uint, 0644);


/*
 * warm_peers: create and delete N throwaway peers before any station exists.
 *
 * Measured on the QCN6122: the first FOUR station peer create/delete cycles
 * after PDEV_INIT have every received frame reaped from REO and then dropped
 * inside NSS with no counter moving; the fifth works and everything works from
 * then on.  Idling 90 s before associating changes nothing, so it is counted in
 * peer cycles, not time.  If the state that has to be worked through is per
 * wifili instance rather than per peer id or per real association, burning the
 * cycles on dummies at vdev-create time should make the station's first
 * association work - which is both the test and, if it holds, the workaround.
 *
 * warm_base is the first peer id to use.  The WLAN firmware hands the AP self
 * peer id 3 and the station 4,5,6,..., and it does not know about these, so
 * warm_base=4 reproduces exactly the ids the failing cycles use.
 * warm_ast is the AST index; the real station gets 19 on both radios.
 */
static unsigned int probe_warm_peers;
module_param_named(warm_peers, probe_warm_peers, uint, 0644);
static unsigned int probe_warm_base = 4;
module_param_named(warm_base, probe_warm_base, uint, 0644);
static unsigned int probe_warm_ast = 19;
module_param_named(warm_ast, probe_warm_ast, uint, 0644);
static unsigned int probe_warm_delay_ms = 20;
module_param_named(warm_delay, probe_warm_delay_ms, uint, 0644);

/*
 * Whether an excepted frame is handed to Linux or just counted.  Kept
 * switchable so the peer lifecycle and the host delivery path can be tested
 * one at a time: both were new in the same build, and the first run panicked
 * the host, which reboots too fast to leave a log anywhere but flash.
 */
static bool probe_deliver = true;
module_param_named(deliver, probe_deliver, bool, 0644);


/*
 * How many excepted frames to hex-dump on their way up.
 *
 * Seventeen frames reached the callback and none reached dnsmasq or the
 * bridge's forwarding table, which leaves two very different explanations -
 * the frames are not what eth_type_trans() assumes, or they are fine and
 * something further up drops them.  The first 32 bytes tell them apart.
 */
static unsigned int probe_dump_rx;
static unsigned int probe_dump_tx;
module_param_named(dump_tx, probe_dump_tx, uint, 0644);
module_param_named(dump_rx, probe_dump_rx, uint, 0644);

/*
 * nss_wifili_init_msg.flags.  The header documents it only as "Flags for SoC
 * initialization" and defines no values, but the vendor driver builds it as:
 *
 *	flags = 0;
 *	if (cfg[1124]) flags |= 0x10;
 *	flags |= 0x40;
 *	if (cfg[9])    flags |= 0x20;
 *
 * so 0x10 and 0x20 are configuration-dependent and 0x40 is unconditional.
 *
 * An unconditional bit is a premise of the protocol rather than an optional
 * feature, so it is set here too.
 *
 * It is not, as first guessed, tied to the additional-memory arrays.  Reading
 * the allocator settles that: one segment counter runs across both descriptor
 * kinds, ext_desc_page_num is set to its value between the two loops - the
 * index of the first segment holding extended descriptors, which is what this
 * probe already sends - and a third loop distributes the segments by position,
 * the first 32 into memory_addr[] and the overflow into the additional arrays.
 * NSS_WIFILI_MAX_NUMBER_OF_PAGE_MSG is 32 for exactly that reason.  With five
 * segments here the additional arrays are legitimately empty, so 0x40 cannot
 * be announcing them.
 */
/*
 * Default 0x40, per the analysis immediately above.
 *
 * A second comment block used to sit here claiming the opposite - that
 * osif_nss_wifili_soc_init() "never sets 0x40 ... assert nothing rather than
 * guess" - and it took the initialiser with it, leaving this at 0.  Both blocks
 * were in the file at once, adjacent and contradictory, with the correct one
 * orphaned above the wrong one.  Re-decompiling the vendor module confirms the
 * block above: `flags |= 0x40` is unconditional.  Do not "re-derive" this a
 * third time; read what is already here first.
 */
static unsigned int probe_init_flags = 0x40;

/*
 * The vdev's default next hop inside NSS, 0 to leave it unset.
 *
 * NSS_ETH_RX_INTERFACE (158) is the vendor's value and the accelerated path;
 * unset means every received frame is excepted to the AP netdev instead.
 */
static unsigned int probe_next_hop;
module_param_named(next_hop, probe_next_hop, uint, 0644);

/*
 * vdev_next_hop: the PER-VAP next hop.
 *
 * A different call from next_hop above.  nss_wifi_vdev_base_set_next_hop()
 * takes no interface number and sets the next hop for the wifi base
 * interface; nss_wifi_vdev_set_next_hop() sets it for one VAP.  Only the base
 * one was ever sent, and the uplink stayed fully excepted to the host: a
 * 268 MB client-to-WAN transfer put 131343 frames through the host vdev
 * callback - every frame - for 134 Mbit/s at 41% of the router's CPU, while
 * the downlink, which NSS forwards itself, ran 445 Mbit/s at 8.8%.
 *
 * Multicast and broadcast still reach the host because MCBC_EXC_TO_HOST is
 * set; the older note about DHCP and ARP vanishing down the fast path predates
 * that command being sent.
 */
static unsigned int probe_vdev_nh;
module_param_named(vdev_next_hop, probe_vdev_nh, uint, 0644);

/* Except broadcast and multicast to the host; forward station to station. */
static unsigned int probe_mcbc_to_host = 1;
module_param_named(mcbc_to_host, probe_mcbc_to_host, uint, 0644);

static unsigned int probe_ap_bridge = 1;
module_param_named(ap_bridge, probe_ap_bridge, uint, 0644);

/* htt_pkt_type_ethernet is 2 - the same value as ath11k's frame_mode. */
static unsigned int probe_encap_type = 2;
module_param_named(encap_type, probe_encap_type, uint, 0644);

static unsigned int probe_decap_type = 2;
module_param_named(decap_type, probe_decap_type, uint, 0644);

/*
 * Overrides for the Rx buffer geometry, 0 to use what ath11k reports.
 *
 * ath11k's own numbers are DP_RX_BUFFER_SIZE minus hal_desc_sz - 2048 - 384 =
 * 1664 - but ath11k's buffer size is the wrong base: NSS allocates the Rx
 * buffers, not ath11k, so what matters is NSS's payload size.  The QSDK 12.1
 * driver, the one that pairs with a firmware closest to the 12.2 in use here,
 * hardcodes rx_buf_len = 1856, which with this chip's 384-byte TLV block makes
 * the buffer 2240 rather than 2048.
 *
 * tlv_size is left alone by default: ath11k programs the hardware's packet
 * offset from hal_desc_sz over HTT, its own Rx path works on this board, so
 * 384 is what the hardware is actually doing.  It is adjustable only to be
 * able to try the pair together.
 */
static unsigned int probe_tlv_size;
module_param_named(tlv_size, probe_tlv_size, uint, 0644);

static unsigned int probe_rx_buf_len;
module_param_named(rx_buf_len, probe_rx_buf_len, uint, 0644);

/*
 * The peer's cipher, numbered as QSDK's cdp_sec_type does it: 0 none, 4 TKIP,
 * 6 AES-CCMP.  This used to be hardcoded 0, which is right for an open AP and
 * wrong for WPA2 - and a peer NSS believes is unencrypted is one whose frames
 * it handles on the wrong assumptions, which is where the WPA2 handshake was
 * dying with every message still ACKed.
 */
static unsigned int probe_sec_type = 6;
module_param_named(sec_type, probe_sec_type, uint, 0644);

/*
 * When to tell NSS the peer's cipher.
 *
 * This probe has always sent PEER_SECURITY straight after PEER_CREATE, which
 * is before the 4-way handshake has produced a PTK.  Both the vendor glue and
 * the ath11k reference port send it from the key-install path instead, i.e.
 * once the key exists.  sec_early=0 defers it to the authorise transition,
 * which is the closest thing this probe has to that moment.
 */
/* Whether to send the per-vdev cipher; the vendor does, this probe never has. */
/*
 * Whether to create the vdev's own BSS peer in NSS.
 *
 * ath11k creates the AP's self peer inside ath11k_peer_create() when the vdev
 * starts, not through sta_state, so the peer hook this probe installs never
 * sees it.  The vendor sends a PEER_CREATE for the vdev's own MAC the moment
 * the VAP is created, on both radios:
 *
 *   [nss-wifili]: peer create id 1 vap 0 ast 296 mac 00:03:7f:12:d5:d5
 *
 * Without it NSS has no source AST entry for anything the AP originates.
 */
static bool probe_self_peer = true;
module_param_named(self_peer, probe_self_peer, bool, 0644);
/* moved into struct probe_slot_state */

static bool probe_vdev_sec;
module_param_named(vdev_sec, probe_vdev_sec, bool, 0644);

static bool probe_sec_early = true;
module_param_named(sec_early, probe_sec_early, bool, 0644);


static unsigned int probe_drop_unenc;
module_param_named(drop_unenc, probe_drop_unenc, uint, 0644);
module_param_named(init_flags, probe_init_flags, uint, 0644);

static atomic_t probe_rx_drop_nodev = ATOMIC_INIT(0);
static atomic_t probe_rx_drop_short = ATOMIC_INIT(0);
static atomic_t probe_rx_drop_off = ATOMIC_INIT(0);
static atomic_t probe_rx_handed = ATOMIC_INIT(0);
static atomic_t probe_rx_eapol = ATOMIC_INIT(0);
static atomic_t probe_tx_eapol = ATOMIC_INIT(0);
static atomic_t probe_txf_nodev = ATOMIC_INIT(0);
static atomic_t probe_txf_short = ATOMIC_INIT(0);
static atomic_t probe_txf_nomem = ATOMIC_INIT(0);
static atomic_t probe_txf_nss = ATOMIC_INIT(0);

/*
 * Whether to convert an excepted native-wifi frame to 802.3.
 *
 * NSS hands back a frame it could not process itself exactly as the hardware
 * delivered it.  ath11k configures its AP vdevs for native wifi, so what
 * arrives is an 802.11 header plus RFC1042 SNAP - NOT Ethernet.  This path
 * used to assume Ethernet, so the EAPOL test below read the ethertype out of
 * the middle of addr3, never matched, and every EAPOL the client sent went to
 * the bridge instead of to hostapd.  The reference ath11k NSS port does the
 * same conversion in ath11k_nss_undecap_nwifi()/ath11k_nss_deliver_rx().
 */
static bool probe_undecap_nwifi = true;
module_param_named(undecap_nwifi, probe_undecap_nwifi, bool, 0644);
static atomic_t probe_rx_nwifi = ATOMIC_INIT(0);
static atomic_t probe_rx_nwifi_fail = ATOMIC_INIT(0);

/* Whether EAPOL is routed through mac80211 rather than the bridge. */
static bool probe_eapol_to_mac80211 = true;
module_param_named(eapol_mac80211, probe_eapol_to_mac80211, bool, 0644);

/* Which extended packet types NSS actually sends up, by pkt_type. */
static unsigned int probe_ext_type_count[32];

/*
 * Whether to narrow the REO hash map to destination ring 0, and how many REO
 * destination rings to declare to the firmware.
 *
 * These belong together.  NSS was seen to reap nothing while the hardware
 * heads of rings 1..3 climbed, which says it services ring 0 - but narrowing
 * the map while still declaring four rings tells the firmware one thing and
 * the hardware another, and every run in that state took the firmware down at
 * association while the one run without narrowing survived and reaped.  So
 * both are adjustable, to separate "narrowing is harmful" from "narrowing
 * without matching the declaration is harmful".
 *
 * reo_rings 0 means "declare every ring ath11k allocated".
 */
static bool probe_narrow_reo = true;
module_param_named(narrow_reo, probe_narrow_reo, bool, 0644);

/*
 * Which srng flag bits to keep when describing a ring to NSS.
 *
 * ath11k's flags are its own HAL's, and NSS's struct has no MSI concept at
 * all - no address, no data, nothing to program.  The QCN6122 is an MSI
 * device, so every one of its rings carries HAL_SRNG_FLAGS_MSI_INTR (0x20000)
 * where the internal radio's carry none: 0x80030000 against 0x80010000, the
 * only field that differs between the two pdev descriptions.  Passing a bit
 * NSS cannot act on through to firmware that may act on it anyway is not
 * something to leave to chance.
 */
#define PROBE_SRNG_FLAG_MSI_INTR	0x00020000

/*
 * Override the target type sent in INIT.
 *
 * The firmware dispatches on it: the vendor driver accepts {20,24,25,26,29,30},
 * routes {20,24,25,29} to the internal wifili interface and {26,30} to an
 * external one, and takes a QCA5018-specific branch on 29.  With the ring
 * descriptions of the two radios now known to be identical in every field, the
 * branch the firmware takes is the largest remaining difference between a slot
 * that receives and one that does not.
 */
/* Re-announce the RXDMA refill ring to the WLAN firmware after PDEV_INIT. */
static bool probe_htt_resetup;
module_param_named(htt_resetup, probe_htt_resetup, bool, 0644);

/*
 * The two INIT fields where the probe still differs from the vendor.
 *
 * osif_nss_wifili_soc_init() fills both from wlan_cfg: tx_sw_internode_queue
 * (host-cmn WLAN_CFG_TX_SW_INTERNODE_QUEUE, default 1024, range 128..1024) and
 * tx_device_limit (WLAN_CFG_TX_DEVICE_LIMIT, default 65536, range 16384..65536).
 * Neither QCA5018_i.ini nor QCN6122_i.ini overrides them in any of the three
 * vendor firmwares, so the defaults are what the NSS firmware sees there.  The
 * probe used to send 0 and 4096; slot 0 works either way, slot 1 is the open
 * question.
 */
static unsigned int probe_internode_q = 1024;
module_param_named(internode_q, probe_internode_q, uint, 0644);

static unsigned int probe_tx_device_limit = 65536;
module_param_named(tx_device_limit, probe_tx_device_limit, uint, 0644);

/*
 * Read back, from the CPU side, the register NSS is told a ring lives at.
 * R0 of every ring is RING_BASE_LSB, so the value there must equal the
 * ring's own physical base: a direct check that the windowed translation
 * handed to NSS points at the real register, with no NSS message involved.
 */
static bool probe_verify_reg = true;
module_param_named(verify_reg, probe_verify_reg, bool, 0644);

static void probe_readback(const char *name, const struct ath11k_nss_srng *r)
{
	void __iomem *v;
	u32 lsb;

	if (!probe_verify_reg || !r->valid || !r->hwreg_base[0])
		return;

	v = ioremap(r->hwreg_base[0], 8);
	if (!v) {
		pr_info(PFX "  RB %-14s ioremap %08x failed\n", name, r->hwreg_base[0]);
		return;
	}
	lsb = ioread32(v);
	iounmap(v);

	pr_info(PFX "  RB %-14s reg %08x reads %08x, ring paddr %08x -> %s\n",
		name, r->hwreg_base[0], lsb, r->ring_base_paddr,
		lsb == r->ring_base_paddr ? "MATCH" : "differs");
}

static unsigned int probe_target_type;
module_param_named(target_type, probe_target_type, uint, 0644);

/*
 * Which wifili interface to register on, independently of which radio runs.
 *
 * The slot being run decides both: slot 0 takes the internal interface (203)
 * and slot 1 an external one (215).  So "which radio" and "which firmware code
 * path" have never been varied separately, and every slot-0-is-clean result is
 * ambiguous between "the QCN6122 is the problem" and "the external-interface
 * path is the problem".
 *
 * 0 keeps the coupling, 1 forces external, 2 forces internal.  The isolating
 * run is slot=0 target_type=30 force_if=1: the known-good internal radio over
 * the external path.  A trap at 3F006C18 there would mean the fault is in the
 * external path and has nothing to do with the QCN6122; a clean run would put
 * it back on the radio.
 */
static unsigned int probe_force_if;
module_param_named(force_if, probe_force_if, uint, 0644);

/*
 * Thread-scheme priority to request for this radio.
 *
 * This was hardcoded LOW for every radio, on the reasoning that the vendor's
 * per-radio priority map (CFG_NSS_WIFILI_RADIO_PRI_MAP / MAP_BITS_PER_RADIO)
 * defaults to 0.  That reasoning was a guess, and the stock nwrt boot log on
 * this exact board disproves it:
 *
 *   soc 29 (QCA5018, 2.4G): radio_ifnum:28 scheme_id:1 radio_priority:0
 *   soc 30 (QCN6122, 5G):   radio_ifnum:29 scheme_id:0 radio_priority:1
 *
 * The vendor gives the 5 GHz radio HIGH priority, which the allocator's table
 * maps to THREAD_SCHEME_ID_0, not 1.  Worth noting alongside: slot 1 has
 * trapped on Thread 1 every time, while sending scheme_id 1.
 *
 * 0 = LOW (scheme 1), 1 = HIGH (scheme 0).
 */
static unsigned int probe_scheme_prio;
module_param_named(scheme_prio, probe_scheme_prio, uint, 0644);

static unsigned int probe_hwreg_delta;
module_param_named(hwreg_delta, probe_hwreg_delta, uint, 0644);
MODULE_PARM_DESC(hwreg_delta,
		 "added to every ring hwreg_base; 0x100000 selects the vendor"
		 " QCN6122 UMAC window (BAR+0x180000) instead of ath11k's 0x80000");

/*
 * Measured 2026-09-13: ath11k sets flags 0x20000 on tx_comp, reo_dest,
 * rx_rel and reo_exception and 0 on tcl_data and reo_reinject - bit for bit
 * what the vendor sends.  Masking MSI_INTR off was therefore removing the
 * only ring flag there is.  Default is now pass-through.
 */
static unsigned int probe_flag_mask = 0xffffffff;
module_param_named(flag_mask, probe_flag_mask, uint, 0644);

/*
 * Bits to force ON, on destination rings only.
 *
 * The vendor sends flags 0x20000 on tx_comp, reo_dest, rx_rel and
 * reo_exception - every destination ring - and 0 on tcl_data and
 * reo_reinject, the two source rings.  This probe can only ever pass through
 * what ath11k already set, and masks 0x20000 off on top of that, so if ath11k
 * never sets it the vendor's value is unreachable.  flag_set makes it
 * reachable; the dst-only restriction reproduces the vendor's pattern.
 */
static unsigned int probe_flag_set;
module_param_named(flag_set, probe_flag_set, uint, 0644);


static unsigned int probe_reo_rings = 1;
module_param_named(reo_rings, probe_reo_rings, uint, 0644);

/*
 * Overrides for the two PDEV_INIT fields that name which radio inside the SoC
 * this pdev is.  ath11k reports lmac_id 0 and target_pdev_id 1 for both the
 * internal radio and the QCN6122, because each has its own firmware instance
 * numbering from zero - but NSS drives the LMAC rings itself, and if it picks
 * the wrong one the refill ring's head pointer is never written: measured as
 * sw_head frozen at ath11k's fill value with the hardware tail at zero, while
 * the same peek on the working internal radio shows NSS driving the head and
 * the hardware two entries behind it.
 *
 * -1 means "use what ath11k reports".
 */
static int probe_lmac = -1;
module_param_named(lmac, probe_lmac, int, 0644);

static int probe_target_pdev = -1;
module_param_named(target_pdev, probe_target_pdev, int, 0644);

/*
 * Whether the peer lifecycle follows ath11k's sta_state or the stage-4 poll.
 *
 * Five runs, one survivor, and the survivor is the one that created its peer
 * from the poll a couple of seconds after association rather than
 * synchronously inside sta_state.  Keep both paths selectable until it is
 * clear which of the two differences that run had actually matters.
 */

#define PROBE_PEER_MEM_SZ	1600
/* PROBE_MAX_PEERS moved up, next to probe_slot_state */
/* moved into struct probe_slot_state */
/* moved into struct probe_slot_state */
/* moved into struct probe_slot_state */

/*
 * Release one peer's NSS memory block.
 *
 * The vendor glue keeps a pool indexed by peer id and the ath11k reference
 * port keeps peer->nss.vaddr - both give every peer its own block.  This
 * probe used to hand the same physical address to every PEER_CREATE, so a
 * delete cleared the object a later peer was still using.
 */
static void probe_peer_mem_free_one(u32 pidx)
{
	if (pidx >= PROBE_MAX_PEERS || !probe_peer_mem_va[pidx])
		return;

	if (probe_peer_dma_dev)
		dma_unmap_single(probe_peer_dma_dev, probe_peer_mem_pa[pidx],
				 PROBE_PEER_MEM_SZ, DMA_TO_DEVICE);
	kfree(probe_peer_mem_va[pidx]);
	probe_peer_mem_va[pidx] = NULL;
	probe_peer_mem_pa[pidx] = 0;
}


/* NSS keeps its per-peer state in a block the host allocates; the vendor
 * driver allocates exactly this much in osif_nss_wifili_pmem_alloc().
 */

/* frames NSS delivered up through the vdev interface */
static atomic_t probe_vdev_rx = ATOMIC_INIT(0);
static atomic_t probe_vdev_rx_ext = ATOMIC_INIT(0);

/* frames ath11k handed down to NSS for transmission */
static atomic_t probe_vdev_tx_ok = ATOMIC_INIT(0);
static atomic_t probe_vdev_tx_fail = ATOMIC_INIT(0);

/*
 * The peer handed over, by ath11k's peer id.  A station that re-associates
 * gets a new id, and an auth flag sent for an id NSS has never seen comes
 * back EMSG - so track which one was created and replace it when it changes.
 */
/* moved into struct probe_slot_state */
/* moved into struct probe_slot_state */

/*
 * The auth flag as NSS last accepted it, -1 for "never sent".
 *
 * The firmware takes an auth-flag update once and answers ELENGTH to every
 * repeat of the same value, so a poller that resends it is not merely noisy:
 * the run that coredumped had sent ~30 duplicates first.  A real driver sends
 * this from ath11k's peer-authorize callback, i.e. exactly on the transition,
 * and this is how the polled probe imitates that.
 */
/* moved into struct probe_slot_state */

/*
 * Authorise the peer as soon as it is created rather than after the 4-way
 * handshake.  Diagnostic, not a fix - it is semantically wrong, since an
 * unauthorised peer is exactly what the handshake exists to resolve.  It
 * tests one thing: whether NSS withholds transmit until the peer is
 * authorised.  On the external SoC 18 of 22 EAPOL M1 submissions go
 * unanswered while the internal SoC answers all four, with identical peer
 * parameters and identical vdev configuration.
 */
static bool probe_auth_early;
module_param_named(auth_early, probe_auth_early, bool, 0644);
static int probe_peer_auth(u32 peer_id, bool authorized);

/*
 * The firmware error codes, so the log does not have to be decoded by hand
 * against a header afterwards.
 *
 * Transcribed entry-for-entry from THIS build's
 * qca-nss-drv-14.0/exports/nss_wifili_if.h enum nss_wifili_error_types, which
 * has exactly 93 members.  Keep it that way: the previous table came from a
 * different driver version and was MISALIGNED, not merely short - it carried an
 * extra "GROUP0_TIMER_ALLOC_FAIL" at 26 and omitted AST_ADD/AST_REMOVE/WDS_ADD/
 * WDS_REMOVE/WDS_MAP at 50..54, so every label from 26 on could be wrong.  The
 * old comment here claimed the only failure mode was "the firmware has more
 * codes than this driver's enum declares", i.e. a table that is too SHORT.
 * That fails safe - you get a bare number.  A misaligned table instead hands
 * back a confident wrong name: error=51 was reported as
 * "WDS_INVALID_PEERID_FAIL" when it is really AST_REMOVE_FAIL, and that nearly
 * sent me looking for WDS messages this probe never sends.
 *
 * If the driver version changes, re-derive this list; do not patch entries.
 */
static const char * const probe_emsg[] = {
	/*  0 */ "NONE", "INIT_FAIL_IMPROPER_STATE", "RINGS_INIT_FAIL",
	/*  3 */ "PDEV_INIT_IMPROPER_STATE_FAIL", "PDEV_INIT_INVALID_RADIOID_FAIL",
	/*  5 */ "PDEV_TX_IRQ_ALLOC_FAIL", "PDEV_RESET_INVALID_RADIOID_FAIL",
	/*  7 */ "PDEV_RESET_PDEV_NULL_FAIL", "PDEV_RESET_IMPROPER_STATE_FAIL",
	/*  9 */ "START_IMPROPER_STATE_FAIL", "PEER_CREATE_FAIL",
	/* 11 */ "PEER_DELETE_FAIL", "HASHMEM_INIT_FAIL",
	/* 13 */ "PEER_FREELIST_APPEND_FAIL", "PEER_CREATE_INVALID_VDEVID_FAIL",
	/* 15 */ "PEER_CREATE_INVALID_PEER_ID_FAIL", "PEER_CREATE_VDEV_NULL_FAIL",
	/* 17 */ "PEER_CREATE_PDEV_NULL_FAIL", "PEER_CREATE_ALLOC_FAIL",
	/* 19 */ "PEER_DELETE_VAPID_INVALID_FAIL", "PEER_DELETE_INVALID_PEERID_FAIL",
	/* 21 */ "PEER_DELETE_VDEV_NULL_FAIL", "PEER_DELETE_PDEV_NULL_FAIL",
	/* 23 */ "PEER_DELETE_PEER_NULL_FAIL", "PEER_DELETE_PEER_CORRUPTED_FAIL",
	/* 25 */ "PEER_DUPLICATE_AST_INDEX_PEER_ID_FAIL", "INSUFFICIENT_WT_FAIL",
	/* 27 */ "INVALID_NUM_TCL_RING_FAIL", "INVALID_NUM_REO_DST_RING_FAIL",
	/* 29 */ "HAL_SRNG_SOC_ALLOC_FAIL", "HAL_SRNG_INVALID_RING_INFO_FAIL",
	/* 31 */ "HAL_SRNG_TCL_ALLOC_FAIL", "HAL_SRNG_TXCOMP_ALLOC_FAIL",
	/* 33 */ "HAL_SRNG_REODST_ALLOC_FAIL", "HAL_SRNG_REOREINJECT_ALLOC_FAIL",
	/* 35 */ "HAL_SRNG_RXRELEASE_ALLOC_FAIL", "HAL_SRNG_RXEXCP_ALLOC_FAIL",
	/* 37 */ "HAL_TX_MEMALLOC_FAIL", "HAL_TX_INVLID_POOL_NUM_FAIL",
	/* 39 */ "HAL_TX_INVALID_PAGE_NUM_FAIL", "HAL_TX_DESC_MEM_ALLOC_FAIL",
	/* 41 */ "HAL_RX_MEMALLOC_FAIL", "PDEV_RXDMA_RING_ALLOC_FAIL",
	/* 43 */ "NAWDSEN_PEERID_INVALID", "NAWDSEN_PEER_NULL",
	/* 45 */ "NAWDSEN_PEER_CORRUPTED", "WDS_PEER_CFG_FAIL",
	/* 47 */ "RESET_NO_STOP", "HAL_SRNG_INVALID_RING_BASE_FAIL",
	/* 49 */ "PDEV_RX_INIT_FAIL", "AST_ADD_FAIL",
	/* 51 */ "AST_REMOVE_FAIL", "WDS_ADD_FAIL",
	/* 53 */ "WDS_REMOVE_FAIL", "WDS_MAP_FAIL",
	/* 55 */ "WDS_INVALID_PEERID_FAIL", "WDS_DUPLICATE_AST_INDEX_PEER_ID_FAIL",
	/* 57 */ "INVALID_RADIO_CMD", "INVALID_RADIO_IFNUM",
	/* 59 */ "PEER_SECURITY_PEER_NULL_FAIL", "PEER_SECURITY_PEER_CORRUPTED_FAIL",
	/* 61 */ "RADIO_INVALID_BUF_CFG", "INIT_FAIL_INVALID_TARGET",
	/* 63 */ "PDEV_INIT_FAIL_INVALID_LMAC_ID", "STATE_PDEV_NOT_INITIALIZED",
	/* 65 */ "RX_TLV_INVALID", "RX_BUF_LEN_INVALID",
	/* 67 */ "INVALID_PDEV_ID", "NO_PDEV_PRESENT",
	/* 69 */ "WDS_UPDATE_FAIL", "VLAN_ID_SET_FAIL",
	/* 71 */ "PDEV_UPDATE_INVALID_RADIOID_FAIL", "PDEV_UPDATE_INVALID_LMACID_FAIL",
	/* 73 */ "PDEV_UPDATE_INVALID_TARGETPDEVID_FAIL",
	/* 74 */ "PEER_AST_FLOWID_MAP_VAPID_INVALID_FAIL",
	/* 75 */ "PEER_AST_FLOWID_MAP_VDEV_NULL_FAIL",
	/* 76 */ "PEER_AST_FLOWID_MAP_PEERID_INVALID_FAIL",
	/* 77 */ "PEER_AST_FLOWID_MAP_STA_VAP_FAIL",
	/* 78 */ "PEER_AST_FLOWID_MAP_PEERID_MISMATCH_FAIL",
	/* 79 */ "PEER_AST_FLOWID_MAP_PEER_NULL_FAIL",
	/* 80 */ "PEER_AST_FLOWID_MAP_AST_MISMATCH_FAIL", "ISOLATION_SET_FAIL",
	/* 82 */ "WDS_ALREADY_PRESENT", "STATS_CLR_VDEV_NULL_FAIL",
	/* 84 */ "INVALID_VDEV_ID", "PDEV_INIT_FAIL_INVALID_THREAD_SCHEME_ID",
	/* 86 */ "PEER_WDS_INVALID_IFNUM", "PEER_AUTH_FLAG_UPDATE_FAIL",
	/* 88 */ "PEER_TEARDOWN_ALLOC_FAIL", "TX_CAPTURE_MODE_UPDATE_FAIL",
	/* 90 */ "PEER_MEMORY_INSUFFICIENT_FROM_HOST", "DUPLICATE_MPASS_ID_SET",
	/* 92 */ "UNKNOWN",
};

static const char *emsg_name(u32 e)
{
	return e < ARRAY_SIZE(probe_emsg) ? probe_emsg[e]
					  : "beyond the enum in this driver";
}

/*
 * nss_wifili_tx_msg_sync() overloads its return value, and the two enums it
 * can return collide on every interesting value.  On a local rejection it is
 * the nss_tx_status_t from nss_wifili_tx_msg(); after a completed round trip it
 * is wifili_pvt.response, which nss_wifili_callback() fills from
 * nss_cmn_msg.response.  The only way to read a value is to check whether
 * probe_event_cb() printed a reply line for that message:
 *
 *   reply printed -> nss_cmn_response:  0 ACK, 1 EVERSION, 2 EINTERFACE,
 *                                       3 ELENGTH, 4 EMSG, 5 NOTIFY
 *   no reply      -> nss_tx_status_t:   1 FAILURE, 2 QUEUE, 3 NOT_READY,
 *                                       4 TOO_LARGE, 5 TOO_SHORT, 7 BAD_PARAM
 *
 * Printing 3 as a bare "ELENGTH" cost a whole session chasing a message-length
 * bug that never existed: PEER_CREATE returning 3 with no reply is NOT_READY,
 * i.e. the NSS core had already trapped and coredumped.
 */
static const char *sync_status_name(int s)
{
	switch (s) {
	case 0:	 return "ACK";
	case 1:	 return "EVERSION (replied) / TX_FAILURE";
	case 2:	 return "EINTERFACE (replied) / TX_QUEUE";
	case 3:	 return "ELENGTH (replied) / NOT_READY - NSS core is down";
	case 4:	 return "EMSG - see error= (replied) / TX_TOO_LARGE";
	case 5:	 return "NOTIFY (replied) / TX_TOO_SHORT";
	case 7:	 return "TX_BAD_PARAM";
	default: return "?";
	}
}

/*
 * A started SoC pushes NSS_WIFILI_STATS_MSG at about 50 a second and the vdev
 * pushes its own as fast, all with response NOTIFY.  Logging those buries the
 * answers that matter and evicts the firmware trap dump from the kernel ring -
 * which is the one thing a crashed run needs.  So count them instead, and log
 * only what a message actually got answered.
 */
static atomic_t probe_notify_count = ATOMIC_INIT(0);
static atomic_t probe_vdev_notify_count = ATOMIC_INIT(0);

/*
 * Solicited replies complete the pending wait; unsolicited notifications must
 * not.
 *
 * The previous version special-cased exactly one pair - response NOTIFY *and*
 * type NSS_WIFILI_STATS_MSG - and let everything else fall through to
 * complete(&probe_done).  STATS_MSG is 10, but the notification this firmware
 * actually emits at ~1 Hz is type 16, NSS_WIFILI_WDS_ACTIVE_INFO_MSG, so the
 * filter never matched: in the last trapping round nine of them fell through,
 * and each one overwrote probe_response and completed the completion that
 * nss_wifili_tx_msg_sync() was waiting on.  Any ACK/failure verdict taken from
 * that path while traffic was running is unreliable.
 *
 * The robust test is the response field, not the type: NSS_CMN_RESPONSE_NOTIFY
 * means "this is an event, not an answer", whatever its type number.  That
 * alone would have caught type 16 without knowing what 16 was.  The type list
 * below is kept as a belt-and-braces for firmware that sends an event with a
 * response of 0, and is derived from this build's enum:
 *   10 STATS, 12 PEER_STATS, 16 WDS_ACTIVE_INFO, 41 PEER_4ADDR_EVENT,
 *   53 ASTENTRY_SYNC, 54 MECENTRY_SYNC.
 *
 * This does NOT explain the trap - the crash is in the NSS firmware and a
 * corrupted host-side completion cannot cause it - but it was corrupting the
 * instrument at 1 Hz in exactly the window the trap happens in.
 */
static bool probe_msg_is_unsolicited(const struct nss_wifili_msg *msg)
{
	switch (msg->cm.type) {
	case 10:	/* NSS_WIFILI_STATS_MSG */
	case 12:	/* NSS_WIFILI_PEER_STATS_MSG */
	case 16:	/* NSS_WIFILI_WDS_ACTIVE_INFO_MSG */
	case 41:	/* NSS_WIFILI_PEER_4ADDR_EVENT_MSG */
	case 53:	/* NSS_WIFILI_ASTENTRY_SYNC_MSG */
	case 54:	/* NSS_WIFILI_MECENTRY_SYNC_MSG */
		return true;
	default:
		return msg->cm.response == NSS_CMN_RESPONSE_NOTIFY;
	}
}

static void probe_event_cb(void *app_data, struct nss_wifili_msg *msg)
{
	if (!msg)
		return;

	if (probe_msg_is_unsolicited(msg)) {
		atomic_inc(&probe_notify_count);
		/*
		 * Print once per type so a new event kind is still visible,
		 * without a 1 Hz stream drowning the log during a run.
		 */
		pr_info_once(PFX "  unsolicited event type=%u response=%u"
			     " error=%u (not completing a wait)\n",
			     msg->cm.type, msg->cm.response, msg->cm.error);
		return;
	}

	pr_info(PFX "  reply type=%u response=%u error=%u (%s)\n",
		msg->cm.type, msg->cm.response, msg->cm.error,
		emsg_name(msg->cm.error));

	probe_response = msg->cm.response;
	complete(&probe_done);
}

/*
 * probe_data_cb()
 *	Data path callback.  Registered because the register calls require one;
 *	nothing should arrive here while the probe only sends control messages.
 */
static void probe_data_cb(struct net_device *dev, struct sk_buff *skb,
			  struct napi_struct *napi)
{
	pr_warn_once(PFX "unexpected data callback\n");
	dev_kfree_skb_any(skb);
}

/*
 * probe_vdev_deliver()
 *	Hand an excepted frame to Linux on the interface it arrived on.
 *
 * This is the far end of the Rx path: NSS reaped it from REO, resolved the
 * peer, and had nowhere better to send it - a broadcast, an ARP, a DHCP
 * discover, an EAPOL, or any flow ECM has not accelerated yet.
 *
 * It has to go in on the vif's own netdev, which is why the vdev is registered
 * with that netdev rather than a dummy: the bridge, dnsmasq and hostapd all
 * see traffic by interface, so a frame delivered on anything else is a frame
 * that silently never arrives.  NSS has already done the 802.11 decap, so what
 * lands here is an Ethernet frame and eth_type_trans() is the whole of the
 * remaining work.  Mirrors osif_nss_ol_vap_data_receive().
 */
static void probe_vdev_deliver(struct net_device *dev, struct sk_buff *skb)
{
	if (probe_dump_rx) {
		probe_dump_rx--;
		print_hex_dump(KERN_INFO, PFX "rx: ", DUMP_PREFIX_OFFSET,
			       16, 1, skb->data,
			       min_t(unsigned int, skb->len, 32u), false);
		pr_info(PFX "rx: len=%u headroom=%d dev=%s\n", skb->len,
			skb_headroom(skb), dev ? dev->name : "(null)");
	}

	if (!dev) {
		atomic_inc(&probe_rx_drop_nodev);
		dev_kfree_skb_any(skb);
		return;
	}

	if (!probe_deliver) {
		atomic_inc(&probe_rx_drop_off);
		dev_kfree_skb_any(skb);
		return;
	}

	if (skb->len < ETH_HLEN) {
		atomic_inc(&probe_rx_drop_short);
		dev_kfree_skb_any(skb);
		return;
	}

	if (probe_undecap_nwifi &&
	    skb->len >= sizeof(struct ieee80211_hdr_3addr)) {
		struct ieee80211_hdr *h11 = (struct ieee80211_hdr *)skb->data;
		static const u8 snap[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
		unsigned int hl = ieee80211_hdrlen(h11->frame_control);

		/*
		 * ieee80211_is_data() alone cannot tell an 802.11 frame from an
		 * Ethernet one: it reads the first two bytes as a frame control
		 * field, and for an Ethernet frame those are the first two bytes of
		 * the destination MAC.  A DA beginning 28:d0 gives fc = 0xd028,
		 * whose type field reads as DATA, and the frame would be mangled.
		 * That could not happen while ath11k ran in native-wifi frame mode
		 * and every excepted frame really was 802.11; with frame_mode=2 the
		 * excepted frames are Ethernet.  Require the RFC1042 SNAP header at
		 * the 802.11 payload offset too - no Ethernet frame carries it there.
		 */
		if (ieee80211_is_data(h11->frame_control) &&
		    skb->len >= hl + sizeof(snap) &&
		    !memcmp(skb->data + hl, snap, sizeof(snap))) {
			const u8 *me = dev->dev_addr;

			if (ieee80211_data_to_8023_exthdr(skb, NULL, me,
							  NL80211_IFTYPE_AP,
							  0, false)) {
				atomic_inc(&probe_rx_nwifi_fail);
				dev_kfree_skb_any(skb);
				return;
			}
			atomic_inc(&probe_rx_nwifi);
		}
	}

	/*
	 * EAPOL goes to mac80211, not to the bridge: hostapd listens on the
	 * nl80211 control port, which only mac80211's receive path feeds.
	 */
	/*
	 * Hand EAPOL to the mac80211 control port of the radio it arrived on.
	 *
	 * This used to pass the module parameter, which is one
	 * number for the whole module.  With both radios armed that injected
	 * the 2.4 GHz station M2 into the 5 GHz control port: the probe counted
	 * eapol=20 and hostapd on phy0-ap0 still saw nothing and deauthenticated
	 * after its retries.
	 */
	if (probe_eapol_to_mac80211 && probe_slot_by_dev(dev) >= 0 &&
	    !ath11k_nss_dp_rx_eapol(probe_slot_by_dev(dev), 0, skb)) {
		atomic_inc(&probe_rx_eapol);
		return;
	}

	skb->dev = dev;
	skb->protocol = eth_type_trans(skb, dev);
	skb_reset_network_header(skb);
	skb->pkt_type = PACKET_HOST;
	skb->ip_summed = CHECKSUM_NONE;
	skb->next = NULL;

	/*
	 * No dev_sw_netstats_rx_add() here.  It writes through dev->tstats,
	 * the per-cpu block a driver opts into; a mac80211 netdev keeps its
	 * own counters and leaves that pointer unset, so the call panics the
	 * host on the first excepted frame.  mac80211 accounts for what it
	 * receives itself once the frame is in.
	 */
	atomic_inc(&probe_rx_handed);

	/* mac80211 does allocate dev->tstats, so this is safe and is what
	 * makes the frames visible in /proc/net/dev. */
	dev_sw_netstats_rx_add(dev, skb->len);
	netif_receive_skb(skb);
}

static void probe_vdev_data_cb(struct net_device *dev, struct sk_buff *skb,
			       struct napi_struct *napi)
{
	atomic_inc(&probe_vdev_rx);
	probe_vdev_deliver(dev, skb);
}

/*
 * The extended callback carries the frames NSS marks as special.  The vendor
 * demultiplexes them on a per-type header; every type it does not special-case
 * ends up delivered to the stack, so deliver them all until a type shows up
 * that needs more.
 */
static void probe_vdev_ext_cb(struct net_device *dev, struct sk_buff *skb,
			      struct napi_struct *napi)
{
	struct nss_wifi_vdev_per_packet_metadata *md;
	u32 type;

	atomic_inc(&probe_vdev_rx_ext);

	/*
	 * The per-packet metadata is in the headroom, not in skb->data: the
	 * QSDK 10.4 driver reads it at
	 * skb->head + NSS_WIFI_VDEV_PER_PACKET_METADATA_OFFSET and switches on
	 * pkt_type.  Delivering the skb without looking at that was delivering
	 * frames of types that are not frames at all - Tx completions, Tx
	 * status, Rx errors - straight to the bridge.
	 */
	if (skb->head + NSS_WIFI_VDEV_PER_PACKET_METADATA_OFFSET +
	    sizeof(*md) > skb->data) {
		md = (void *)(skb->head +
			      NSS_WIFI_VDEV_PER_PACKET_METADATA_OFFSET);
		type = md->pkt_type;
	} else {
		type = NSS_WIFI_VDEV_EXT_DATA_PKT_TYPE_NONE;
	}

	if (type < 32)
		probe_ext_type_count[type]++;

	switch (type) {
	case NSS_WIFI_VDEV_EXT_DATA_PKT_TYPE_NONE:
	case NSS_WIFI_VDEV_EXT_DATA_PKT_TYPE_IGMP:
	case NSS_WIFI_VDEV_EXT_DATA_PKT_TYPE_WDS_LEARN:
		/* A real frame the host is expected to receive. */
		probe_vdev_deliver(dev, skb);
		return;
	default:
		/*
		 * Everything else is a notification wearing an skb: Tx
		 * completion, Tx status, an Rx error, a mesh or proxy-STA
		 * hand-off.  The vendor has a handler per type; none of them
		 * ends in the bridge.
		 */
		dev_kfree_skb_any(skb);
		return;
	}
}

/*
 * probe_vdev_tx()
 *	ath11k's Tx, routed through NSS.
 *
 * Follows osif_nss_ol_vap_xmit(): the frame has to be at least an Ethernet
 * header, needs 24 bytes of headroom once it is over 1514 bytes, and travels
 * with skb->next cleared.  On success NSS owns the skb, so return 0 and let
 * ath11k treat it exactly as a frame queued to TCL.
 */
static void probe_vdev_msg_cb(void *app_data, struct nss_cmn_msg *msg)
{
	if (!msg)
		return;

	if (msg->response == NSS_CMN_RESPONSE_NOTIFY) {
		atomic_inc(&probe_vdev_notify_count);
		return;
	}

	pr_info(PFX "  vdev reply type=%u response=%u error=%u\n",
		msg->type, msg->response, msg->error);
}

static void dump_srng(const char *name, const struct ath11k_nss_srng *s,
		      u32 dev_base)
{
	if (!s->valid) {
		pr_info(PFX "  %-16s <not allocated>\n", name);
		return;
	}
	pr_info(PFX "  %-16s id=%3u dir=%s entries=%5u esz=%2u paddr=%08x hwreg=%08x,%08x flags=%08x\n",
		name, s->ring_id, s->ring_dir ? "dst" : "src",
		s->num_entries, s->entry_size, s->ring_base_paddr,
		s->hwreg_base[0],
		s->hwreg_base[1], s->flags);
}

/*
 * fill_srng_msg()
 *	Translate one ring.
 *
 * The only computation is on hwreg_base: ath11k's value is an offset into the
 * WLAN register window and the firmware wants a physical address.  A zero is
 * left alone - the LMAC rings genuinely have no register base, and turning
 * that into dev_base_paddr would name the wrong register rather than none.
 *
 * mac_id and low_threshold stay zero, which is what the vendor sends.
 */
static void fill_srng_msg(struct nss_wifili_hal_srng_info *dst,
			  const struct ath11k_nss_srng *src,
			  u32 dev_base_paddr)
{
	int i;

	memset(dst, 0, sizeof(*dst));
	if (!src->valid)
		return;

	dst->ring_id		= src->ring_id;
	dst->ring_base_paddr	= src->ring_base_paddr;
	dst->num_entries	= src->num_entries;
	dst->flags		= (src->flags & probe_flag_mask) |
				  (src->ring_dir ? probe_flag_set : 0);
	dst->ring_dir		= src->ring_dir;
	dst->entry_size		= src->entry_size;

	/*
	 * hwreg_delta shifts every ring register NSS is given.
	 *
	 * ath11k_nss_export.c reaches the QCN6122's UMAC registers through
	 * ath11k's own static window at ATH11K_PCI_WINDOW_START (0x80000),
	 * and its comment records that the vendor's
	 * hal_get_window_address_6122() instead maps UMAC to BAR + 0x180000 -
	 * the third 0x80000 slot - because the co-processor is a different
	 * master from the host CPU.  Observed values agree: slot 1 sends
	 * 81ec4694 = 0x81E00000 + 0x80000 + 0x44694.
	 *
	 * That comment concludes "the blocker is not the mapping", but it was
	 * written about the WLAN Q6 dying with err_smem_ver.2.1 - a different
	 * processor from the NSS core, which is what traps today with a dst
	 * range error.  So the window is untested against THIS failure.
	 *
	 * A wrong register address makes NSS read a garbage ring head/tail,
	 * use it as an index, and store to a computed address - which is what
	 * a dst range error is.  Delta 0x100000 selects the vendor layout.
	 */
	for (i = 0; i < NSS_WIFILI_MAX_SRNG_REG_GROUPS_MSG &&
		    i < ATH11K_NSS_NUM_REG_GRP; i++)
		dst->hwreg_base[i] = src->hwreg_base[i] ?
			src->hwreg_base[i] + probe_hwreg_delta : 0;
}

/*
 * A control run: the same message with the ring description deliberately
 * poisoned.  Without it a plain ACK would prove nothing about compatibility -
 * it is what showed the firmware really does consume these addresses, by
 * failing with HAL_SRNG_REODST_ALLOC_FAIL on 0xdeadbeef.
 */
static void poison_init(struct nss_wifili_init_msg *init)
{
	int i;

	for (i = 0; i < NSS_WIFILI_MAX_TCL_DATA_RINGS_MSG; i++) {
		init->tcl_ring_info[i].ring_id		= 0xfe;
		init->tcl_ring_info[i].ring_base_paddr	= 0xdeadbeef;
		init->tcl_ring_info[i].num_entries	= 0x7fffffff;
		init->tcl_ring_info[i].entry_size	= 0xffff;
		init->tcl_ring_info[i].ring_dir		= 0xff;
		init->tcl_ring_info[i].hwreg_base[0]	= 0xdeadbeef;
		init->tcl_ring_info[i].hwreg_base[1]	= 0xdeadbeef;
	}
	init->hssm.dev_base_addr	 = 0xdeadbeef;
	init->hssm.shadow_rdptr_mem_addr = 0xdeadbeef;
	init->hssm.shadow_wrptr_mem_addr = 0xdeadbeef;
	init->hssm.lmac_rings_start_id	 = 0xff;
	init->num_tcl_data_rings	 = NSS_WIFILI_MAX_TCL_DATA_RINGS_MSG;
	init->num_reo_dest_rings	 = NSS_WIFILI_MAX_REO_DATA_RINGS_MSG;
	init->target_type	 = 0xffffffff;
	init->wrip.tlv_size		 = 0xffff;
	init->wrip.rx_buf_len		 = 0xffff;
}

static int probe_send_simple(struct nss_ctx_instance *ctx, uint32_t type,
			     const char *what)
{
	struct nss_wifili_msg *msg;
	nss_tx_status_t status;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	nss_wifili_msg_init(msg, probe_wifili_if, type, 0,
			    probe_event_cb, NULL);
	status = nss_wifili_tx_msg_sync(ctx, msg);
	pr_info(PFX "%s -> status=%d (%s)\n", what, status,
		sync_status_name(status));

	kfree(msg);
	return (status == NSS_TX_SUCCESS) ? 0 : -EIO;
}

static int probe_send_reset(struct nss_ctx_instance *ctx)
{
	/*
	 * The firmware refuses a reset that is not preceded by a stop -
	 * NSS_WIFILI_EMSG_RESET_NO_STOP exists precisely for that.  Send both
	 * and let each report its own status; a stop on an already-stopped SoC
	 * failing is expected and harmless.
	 */
	probe_send_simple(ctx, NSS_WIFILI_STOP_MSG, "STOP");
	msleep(100);
	return probe_send_simple(ctx, NSS_WIFILI_SOC_RESET_MSG, "SOC_RESET");
}

/*
 * NSS builds its Tx descriptor pool out of memory the host hands it, and the
 * descriptor sizes are fixed by the firmware: the vendor asks for
 * 80 * (num_tx_desc + 1) bytes of regular descriptors followed by
 * 160 * (num_tx_desc_ext + 1) bytes of extended ones, and tells NSS where the
 * extended run starts with ext_desc_page_num.
 *
 * The earlier version handed over four pages and left ext_desc_page_num at 0,
 * which puts both arrays at page 0 - two descriptor tables over the same
 * memory.
 */
#define PROBE_TX_DESC_SZ	80
#define PROBE_TX_EXT_DESC_SZ	160
/*
 * Descriptor count and page size, both taken from the vendor.
 *
 * For a single-pool target - which this is, IPQ5018 having one TCL data ring -
 * osif_nss_wifili_soc_init() sends num_tx_desc = num_tx_desc_ext = 4096 and
 * chunks the descriptor memory at 245760 bytes a page.
 *
 * That page size is not arbitrary: 245760 is 80 * 3072 and 160 * 1536, so a
 * descriptor of either size divides it exactly.  The 64 KiB pages this used
 * before divide neither - 65536/80 is 819.2 and 65536/160 is 409.6 - so if the
 * firmware addresses a descriptor as
 *
 *	page[idx / (page_size / desc_size)] + (idx % ...) * desc_size
 *
 * then the last descriptor of a page has only 16 of its 80 bytes inside it and
 * the write runs past the allocation.  Corruption whose consequences depend on
 * what happens to be next in memory is exactly the shape of the remaining
 * fault: everything ACKs, nothing reproduces, and it only bites once traffic
 * starts.
 */
/*
 * Back to 4096 to bisect.  8192 came from ATH11K_WIFILI_DBDC_NUM_TX_DESC in the
 * open ath11k NSS reference, but it was changed in the same round as
 * num_rx_swdesc 1024->4096 and init_flags 0x40->0, and that round moved the
 * failure EARLIER: the core now dies ~140 ms after the second PEER_SECURITY,
 * during association, with no PEER_AUTH and no data frames at all, where before
 * it survived association and trapped ~1.5 s into sustained traffic.
 *
 * Three values changed at once bought no attribution, which is the same mistake
 * as ranking parameters on single runs.  swdesc=4096 is the better-grounded of
 * the two (both vendor formulas agree on it for a non-QCA5018 radio), so it
 * stays and this one reverts.  The vendor's own tx desc counts are profile
 * dependent (WIFILI_NSS_DBDC_NUM_TX_DESC is 1024*4 under QCA_LOWMEM_CONFIG, not
 * 1024*8), so 4096 is also the profile-correct figure for this board.
 */
#define PROBE_NUM_TX_DESC	4096

/*
 * WLAN_CFG_RX_SW_DESC_NUM_SIZE from the vendor's cfg_dp.h, QCA_LOWMEM_CONFIG
 * branch - the profile this 256 MB IPQ5018 board builds as.  Both vendor
 * formulas land on this same number for a radio that is not the internal 2.4
 * GHz one; see the long comment at the swdesc computation in
 * probe_send_pdev_init() for the full derivation.
 *
 * PROBE_RX_DESC_POOL_WEIGHT (3) used to live here and is gone: the weight of 3
 * is the full-memory #else branch in cfg_dp.h, the low-memory default is 1, and
 * multiplying by 3 here was a misapplication of the open ath11k reference.
 */
/*
 * Was 1024, as a bisection: the round that first tried 4096 also changed
 * num_tx_desc and init_flags, and it moved the failure from traffic-time to
 * association-time - the core died ~123 ms after the second PEER_SECURITY,
 * with no PEER_AUTH and no data frames.  1024 restored the traffic-time
 * failure, which left 4096 under suspicion of being actively harmful here
 * despite matching both vendor formulas.
 *
 * It was not.  Both failures were ath11k's sixteen cached refill buffers
 * landing in NSS's descriptor pool - see nss_refill_hold in the 991 patch -
 * and with that fixed, 4096 was re-run A/B against 1024 on the 5 GHz radio,
 * six iperf3 -P 4 runs each over two fresh boots per value:
 *
 *	                 uplink       downlink   rx_desc_alloc_fail
 *	swdesc 1024   477 Mbit/s    482 Mbit/s   ~420000 per run
 *	swdesc 4096   488 Mbit/s    446 Mbit/s   0
 *
 * Zero traps either way.  The throughput columns are noise - the run-to-run
 * spread is 110 Mbit/s against a 10 Mbit/s difference in means - so the pool
 * size does not buy rate at these speeds.  What it does is take
 * rx_desc_alloc_fail from two thirds of reo_reaped to exactly zero, on every
 * run, cumulative over 1.7 M frames.
 *
 * That counter never cost a frame: rx_deliverd tracked reo_reaped to within
 * ten packets at 1024 too, so the exhaustion is on the replenish side and the
 * firmware recovers from it.  4096 is kept anyway - it is the value both
 * vendor formulas give for a radio that is not the internal 2.4 GHz one, it
 * silences an error counter that would otherwise mask a real one, and it costs
 * no host memory (MemAvailable is 21 MB either way; the pool is firmware side).
 */
#define PROBE_RX_SW_DESC_NUM	4096
#define PROBE_TX_PAGE_SZ	245760
#define PROBE_TX_MAX_PAGES	NSS_WIFILI_MAX_NUMBER_OF_PAGE_MSG

static void *probe_tx_page_va[PROBE_TX_MAX_PAGES];
static dma_addr_t probe_tx_page_pa[PROBE_TX_MAX_PAGES];
static int probe_tx_pages;
static int probe_tx_ext_first_page;
static struct device *probe_dma_dev;

static void probe_free_tx_pages(void)
{
	int i;

	for (i = 0; i < probe_tx_pages; i++)
		if (probe_tx_page_va[i])
			dma_free_coherent(probe_dma_dev, PROBE_TX_PAGE_SZ,
					  probe_tx_page_va[i],
					  probe_tx_page_pa[i]);
	memset(probe_tx_page_va, 0, sizeof(probe_tx_page_va));
	probe_tx_pages = 0;
}

static int probe_alloc_tx_pages(struct device *dev)
{
	u32 reg_bytes, ext_bytes;
	int reg_pages, ext_pages, total, i;

	if (probe_tx_pages)
		return 0;

	reg_bytes = PROBE_TX_DESC_SZ * (PROBE_NUM_TX_DESC + 1);
	ext_bytes = PROBE_TX_EXT_DESC_SZ * (PROBE_NUM_TX_DESC + 1);
	reg_pages = DIV_ROUND_UP(reg_bytes, PROBE_TX_PAGE_SZ);
	ext_pages = DIV_ROUND_UP(ext_bytes, PROBE_TX_PAGE_SZ);
	total = reg_pages + ext_pages;

	if (total > PROBE_TX_MAX_PAGES) {
		pr_err(PFX "tx pool needs %d pages, max %d\n",
		       total, PROBE_TX_MAX_PAGES);
		return -ENOMEM;
	}

	probe_dma_dev = dev;
	for (i = 0; i < total; i++) {
		probe_tx_page_va[i] = dma_alloc_coherent(dev, PROBE_TX_PAGE_SZ,
							 &probe_tx_page_pa[i],
							 GFP_KERNEL);
		if (!probe_tx_page_va[i]) {
			pr_err(PFX "tx pool page %d alloc failed\n", i);
			probe_tx_pages = i;
			probe_free_tx_pages();
			return -ENOMEM;
		}
	}

	probe_tx_pages = total;
	probe_tx_ext_first_page = reg_pages;
	pr_info(PFX "tx pool: %d desc -> %u B regular (%d pages) + %u B ext (%d pages), ext starts at page %d\n",
		PROBE_NUM_TX_DESC, reg_bytes, reg_pages, ext_bytes, ext_pages,
		probe_tx_ext_first_page);
	return 0;
}

/*
 * probe_radio_if_alloc()
 *	Allocate and register the radio interface a pdev needs.
 *
 * The vendor does this before every PDEV_INIT: allocate a dynamic interface of
 * the wifili radio type, register it, then ask for a thread scheme against it.
 * scheme_id is the only part of it that reaches the firmware, but the firmware
 * validates it - NSS_WIFILI_EMSG_PDEV_INIT_FAIL_INVALID_THREAD_SCHEME_ID
 * exists - and a hardcoded 0 is not the same as one the allocator handed out.
 */
/*
 * The radio interface type has to match the wifili interface it hangs off:
 * an external SoC's radio is EXTERNAL0/1, not INTERNAL, and allocating the
 * wrong one gets a node NSS will not associate with the pdev.
 */
/*
 * Gate on the wifili interface actually in use, not on the slot number.  The two
 * agreed only while one SoC was ever registered; running soc 0 with slot=1
 * asked for an EXTERNAL0 radio node on the INTERNAL interface, which is the
 * exact mismatch the comment above warns about.  The firmware took the
 * PDEV_INIT and then died between it and START.
 */
static enum nss_dynamic_interface_type probe_radio_di_type(void)
{
	if (probe_wifili_if == NSS_WIFILI_INTERNAL_INTERFACE)
		return NSS_DYNAMIC_INTERFACE_TYPE_WIFILI_INTERNAL;

	return probe_wifili_if == NSS_WIFILI_EXTERNAL_INTERFACE1 ?
		NSS_DYNAMIC_INTERFACE_TYPE_WIFILI_EXTERNAL1 :
		NSS_DYNAMIC_INTERFACE_TYPE_WIFILI_EXTERNAL0;
}

static int probe_radio_if_alloc(u8 *scheme_id)
{
	struct nss_ctx_instance *rctx;
	int ifnum;

	/*
	 * Reuse across runs: the interface has to stay for as long as the pdev
	 * is up inside NSS, and a second allocation would strand the first.
	 */
	if (probe_radio_ifnum >= 0) {
		*scheme_id = probe_scheme_id;
		pr_info(PFX "radio if=%d scheme_id=%u (reused)\n",
			probe_radio_ifnum, probe_scheme_id);
		return 0;
	}

	ifnum = nss_dynamic_interface_alloc_node(probe_radio_di_type());
	if (ifnum < 0) {
		pr_err(PFX "radio dynamic interface alloc failed (%d)\n", ifnum);
		return ifnum;
	}

	rctx = nss_register_wifili_radio_if(ifnum, probe_data_cb, probe_data_cb,
					    probe_event_cb, probe_ndev, 0);
	if (!rctx) {
		pr_err(PFX "nss_register_wifili_radio_if(%d) failed\n", ifnum);
		nss_dynamic_interface_dealloc_node(ifnum,
			probe_radio_di_type());
		return -ENODEV;
	}

	probe_radio_ifnum = ifnum;
	probe_scheme_id = nss_wifili_thread_scheme_alloc(probe_ctx, ifnum,
				probe_scheme_prio ? NSS_WIFILI_HIGH_PRIORITY_SCHEME :
						NSS_WIFILI_LOW_PRIORITY_SCHEME);
	*scheme_id = probe_scheme_id;
	pr_info(PFX "radio if=%d scheme_id=%u\n", ifnum, probe_scheme_id);
	return 0;
}

static void probe_radio_if_free(void)
{
	if (probe_radio_ifnum < 0)
		return;

	nss_wifili_thread_scheme_dealloc(probe_ctx, probe_radio_ifnum);
	nss_unregister_wifili_radio_if(probe_radio_ifnum);
	nss_dynamic_interface_dealloc_node(probe_radio_ifnum,
		probe_radio_di_type());
	probe_radio_ifnum = -1;
}

/*
 * probe_send_pdev_init()
 *	Bring one radio up inside NSS.
 *
 * Control messages all go to the SoC interface.  nss_wifili_tx_msg() rejects
 * anything else outright - only INTERNAL_INTERFACE and EXTERNAL_INTERFACE0/1
 * pass its check - and the vendor confirms it: nss_wifili_pdev_attach() calls
 * nss_cmn_msg_init() with the SoC interface number even though it has just
 * allocated a radio interface.  radio_id in the payload selects the radio.
 */
static int probe_send_pdev_init(struct ath11k_nss_dp_info *info, unsigned int r,
				u32 swdesc_override)
{
	const struct ath11k_nss_pdev *p = &info->pdev[r];
	struct nss_wifili_msg *msg;
	struct nss_wifili_pdev_init_msg *pi;
	nss_tx_status_t status;
	u8 scheme_id = 0;
	u32 swdesc;
	int ret = -EIO;

	if (!p->valid) {
		pr_info(PFX "pdev %u not present, skipping\n", r);
		return 0;
	}

	if (probe_radio_if_alloc(&scheme_id))
		return -ENODEV;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg) {
		probe_radio_if_free();
		return -ENOMEM;
	}

	/*
	 * WIFILI_RX_DESC_POOL_WEIGHT.  ath11k can have up to three times the
	 * refill ring's size outstanding at once - in the ring, in flight, and
	 * being processed - and it sizes its own idr accordingly:
	 *
	 *	dp_rx.c:401   idr_alloc(..., (rx_ring->bufs_max * 3) + 1, ...)
	 *
	 * so the firmware's Rx software descriptor pool has to cover 3x as
	 * well.  The open ath11k NSS reference does exactly that:
	 *
	 *	pdevmsg->num_rx_swdesc =
	 *		WIFILI_RX_DESC_POOL_WEIGHT * DP_RXDMA_BUF_RING_SIZE;
	 *	#define WIFILI_RX_DESC_POOL_WEIGHT 3
	 *	#define DP_RXDMA_BUF_RING_SIZE  1024      (dp.h:220, this tree)
	 *
	 * This probe was announcing 1024 - the bare ring size, with the weight
	 * dropped - so ath11k could hand out indices up to 3073 against a pool
	 * the firmware believed held 1024.  Indexing past the end only happens
	 * once sustained Rx traffic fills it, which is exactly when the NSS core
	 * traps.
	 */
	/*
	 * 4096, which is what BOTH vendor formulas produce for this board.
	 *
	 * The three "conflicting" sources were never in conflict - they describe
	 * different radios and different memory profiles, and the conflict was
	 * manufactured by comparing them as if they applied to the same one:
	 *
	 *   newer qca-wifi (osif_nss_wifili.c):
	 *	num_rx_swdesc = wlan_cfg_get_dp_soc_rx_sw_desc_num(..)   // 4096
	 *	if (target_type == TARGET_TYPE_QCA5018 && mem_profile == 1)
	 *		num_rx_swdesc = WIFILI_NSS_2G_NUM_RX_DESC;       // 2048
	 *   older qca-wifi:
	 *	num_rx_swdesc = rxdma_refill_ring_size * rx_sw_desc_weight
	 *			= 4096 * 1 = 4096
	 *
	 * cfg_dp.h, QCA_LOWMEM_CONFIG branch - this 256 MB IPQ5018 board, per the
	 * vendor's own "IPQ5018 has 2K rx buffer requirement for 256Mb profile":
	 *	WLAN_CFG_RX_SW_DESC_NUM_SIZE       4096   (min 1024, max 12288)
	 *	WLAN_CFG_RX_SW_DESC_WEIGHT_SIZE       1   (max 3)
	 *	   "For low memory AP cases using 1 will reduce the rx
	 *	    descriptors memory req."
	 *
	 * So the weight of 3 belongs to the full-memory #else branch, and that is
	 * where the open ath11k NSS reference's WIFILI_RX_DESC_POOL_WEIGHT 3 came
	 * from - it does not apply here.  An earlier version of this code
	 * multiplied by 3 on the strength of that reference; that was wrong.
	 *
	 * The 2048 override is gated on TARGET_TYPE_QCA5018 (29, the internal
	 * 2.4 GHz radio).  Slot 1 is QCN6122, target 30, so it is NOT covered -
	 * which also vindicates the old note that 2048 applies only to tgt 29,
	 * a note retracted earlier today because the stripped binary showed no
	 * branch.  That binary was built for a single target with the branch
	 * folded away: "no branch in the disassembly" is not "no branch in the
	 * source".
	 */
	swdesc = swdesc_override ? swdesc_override : PROBE_RX_SW_DESC_NUM;

	pr_info(PFX "--- PDEV_INIT radio=%u lmac=%d target_pdev=%d swdesc=%u%s scheme=%u\n",
		r, (probe_lmac >= 0) ? probe_lmac : (int)p->lmac_id,
		(probe_target_pdev >= 0) ? probe_target_pdev :
		(int)p->target_pdev_id, swdesc,
		swdesc_override ? " (overridden)" : "", scheme_id);
	dump_srng("rxdma", &p->rxdma_ring, info->dev_base_paddr);

	pi = &msg->msg.pdevmsg;
	fill_srng_msg(&pi->rxdma_ring, &p->rxdma_ring, info->dev_base_paddr);
	pr_info(PFX "  rxdma srng: id=%u dir=%u entries=%u esz=%u flags=%08x paddr=%08x hwreg=%08x,%08x\n",
		p->rxdma_ring.ring_id, p->rxdma_ring.ring_dir,
		p->rxdma_ring.num_entries, p->rxdma_ring.entry_size,
		p->rxdma_ring.flags, p->rxdma_ring.ring_base_paddr,
		p->rxdma_ring.hwreg_base[0], p->rxdma_ring.hwreg_base[1]);
	pi->radio_id	   = p->radio_id;
	pi->lmac_id			= (probe_lmac >= 0) ? probe_lmac : p->lmac_id;
	pi->target_pdev_id = (probe_target_pdev >= 0) ? probe_target_pdev :
				     p->target_pdev_id;
	pi->num_rx_swdesc  = swdesc;
	pi->hwmode	   = 0;
	pi->scheme_id	   = scheme_id;

	nss_wifili_msg_init(msg, probe_wifili_if,
			    NSS_WIFILI_PDEV_INIT_MSG,
			    sizeof(struct nss_wifili_pdev_init_msg),
			    probe_event_cb, NULL);

	status = nss_wifili_tx_msg_sync(probe_ctx, msg);
	pr_info(PFX "PDEV_INIT radio=%u -> status=%d (%s)\n", r, status,
		sync_status_name(status));
	if (status == NSS_TX_SUCCESS)
		ret = 0;
	else
		probe_radio_if_free();

	kfree(msg);
	return ret;
}

/*
 * probe_vdev_create()
 *	Give NSS a virtual device for the radio's AP interface.
 *
 * The vdev is not a wifili message: it is its own interface family
 * (nss_wifi_vdev_*) on a NSS_DYNAMIC_INTERFACE_TYPE_VAP node, which is why the
 * wifili message enum has nothing vdev-shaped in it.  radio_ifnum ties it back
 * to the radio interface PDEV_INIT allocated, and vdev_id has to be the one the
 * WLAN firmware knows - ath11k's arvif->vdev_id.
 *
 * epid, downloadlen and hdrcache are HTT Tx descriptor metadata.  They are left
 * zero: this is about seeing whether Rx reaches a vdev, and a zero Tx template
 * cannot be used by accident.
 */
/*
 * probe_vdev_cmd()
 *	Send one NSS_WIFI_VDEV_INTERFACE_CMD_MSG.
 *
 * The vdev command space is where the two settings a bridged AP needs live.
 * Without them a client associates, is authorised, has its unicast excepted
 * to the host - and never gets an address, because DHCP discover and ARP are
 * broadcast, and broadcast is handled by NSS's own intra-BSS path rather than
 * being excepted.  The counters name it: reo_reaped runs ahead of rx_deliverd
 * by about the number of frames missing, rx_intra_bss_mcast accounts for them,
 * and the frames that do arrive are all multicast or unicast - never a
 * broadcast.
 */
static int probe_vdev_cmd(int ifnum, u32 cmd, u32 value, const char *name)
{
	struct nss_wifi_vdev_msg *msg;
	nss_tx_status_t status;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	msg->msg.vdev_cmd.cmd	= cmd;
	msg->msg.vdev_cmd.value	= value;

	nss_wifi_vdev_msg_init(msg, ifnum, NSS_WIFI_VDEV_INTERFACE_CMD_MSG,
			       sizeof(struct nss_wifi_vdev_cmd_msg),
			       NULL, NULL);

	status = nss_wifi_vdev_tx_msg(probe_ctx, msg);
	pr_info(PFX "vdev cmd %s (%u) = %u -> status=%d\n",
		name, cmd, value, status);

	kfree(msg);
	return (status == NSS_TX_SUCCESS) ? 0 : -EIO;
}

static int probe_peer_create(const struct ath11k_nss_peer_info *peer,
			     struct device *dma_dev);
static int probe_peer_security(u32 peer_id);

static int probe_peer_delete(u32 peer_id);

/*
 * Find the vdev's own peer among the ones ath11k knows and hand it to NSS.
 */
static void probe_self_peer_create(unsigned int soc_idx, const u8 *mac,
				   struct device *dma_dev)
{
	struct ath11k_nss_peer_info peers[ATH11K_NSS_MAX_PEER];
	int np, i;

	if (!probe_self_peer || probe_self_peer_id >= 0 || !dma_dev)
		return;

	np = ath11k_nss_get_peers(soc_idx, peers, ARRAY_SIZE(peers));
	for (i = 0; i < np; i++) {
		if (!peers[i].valid || !ether_addr_equal(peers[i].mac_addr, mac))
			continue;

		pr_info(PFX "self peer: mac=%pM peer_id=%u ast_hash=%u hw_ast=%u\n",
			peers[i].mac_addr, peers[i].peer_id,
			peers[i].ast_hash, peers[i].hw_peer_id);
		if (!probe_peer_create(&peers[i], dma_dev)) {
			probe_self_peer_id = peers[i].peer_id;
			probe_peer_security(peers[i].peer_id);
		}
		return;
	}
	pr_info(PFX "self peer for %pM not found among %d peers\n", mac, np);
}


/*
 * probe_warm_peers_run()
 *	Burn probe_warm_peers peer create/delete cycles on throwaway peers.
 *
 * Each dummy is made to look like the station peer that follows: same vdev,
 * same AST index, consecutive peer ids starting at warm_base.  Only the MAC
 * differs, because two live peers may not share one.
 */
static void probe_warm_peers_run(struct device *dma_dev)
{
	struct ath11k_nss_peer_info d;
	unsigned int i;

	if (!probe_warm_peers || !dma_dev)
		return;

	pr_info(PFX "warm_peers: %u dummy cycles from id %u, ast %u\n",
		probe_warm_peers, probe_warm_base, probe_warm_ast);

	for (i = 0; i < probe_warm_peers; i++) {
		memset(&d, 0, sizeof(d));
		d.valid	     = 1;
		d.peer_id    = probe_warm_base + i;
		d.vdev_id    = 0;
		d.ast_hash   = probe_warm_ast;
		d.hw_peer_id = probe_warm_ast;
		d.mac_addr[0] = 0x02;
		d.mac_addr[5] = (u8)(0x10 + i);

		if (probe_peer_create(&d, dma_dev))
			continue;
		if (probe_warm_delay_ms)
			msleep(probe_warm_delay_ms);
		probe_peer_delete(d.peer_id);
		if (probe_warm_delay_ms)
			msleep(probe_warm_delay_ms);
	}

	pr_info(PFX "warm_peers: done\n");
}

static int probe_vdev_create(const struct ath11k_nss_vif *vif)
{
	struct nss_wifi_vdev_msg *msg;
	struct nss_wifi_vdev_config_msg *cfg;
	nss_tx_status_t status;
	uint32_t reg;
	int ifnum;

	if (probe_vdev_ifnum >= 0) {
		pr_info(PFX "vdev if=%d already created\n", probe_vdev_ifnum);
		return 0;
	}

	if (probe_radio_ifnum < 0) {
		pr_err(PFX "no radio interface yet - PDEV_INIT has to run first\n");
		return -EINVAL;
	}

	ifnum = nss_dynamic_interface_alloc_node(NSS_DYNAMIC_INTERFACE_TYPE_VAP);
	if (ifnum < 0) {
		pr_err(PFX "vdev dynamic interface alloc failed (%d)\n", ifnum);
		return ifnum;
	}

	/*
	 * Use vif->netdev as it arrives.  Do not take a reference to it, and do
	 * not read its name.
	 *
	 * This hook runs from ath11k's add_interface, inside drv_add_interface()
	 * with RTNL held, and at that point the netdev object exists and its
	 * ifindex is already correct - measured as 5 and 6, matching phy0-ap0
	 * and phy1-ap0 - but it is not fully registered: the name is still
	 * empty, and dev_hold() takes a level 0 translation fault on
	 * pcpu_refcnt.  RTNL is held, so the dying task never releases it and
	 * every later cfg80211 and netdev task piles up behind the lock.
	 *
	 * An older note here read the faulting address, ffffff3f9dcaf528, as
	 * the netdev pointer being garbage.  It is not: subtract the 0x528
	 * offset of pcpu_refcnt and what is left is the netdev itself.  The
	 * pointer was always fine; the refcount being asked for did not exist
	 * yet.  virt_addr_valid() returning true for it was not a false
	 * negative either - it really was a valid address.
	 *
	 * Storing it without a reference is safe, and is what the open
	 * reference implementation does: by the time a frame reaches the netdev
	 * it is registered, and mac80211 calls remove_interface - where the
	 * vdev is unregistered from NSS - before it unregisters the netdev.  It
	 * also removes the reference counting that underflowed in an earlier
	 * round, freeing a netdev early and faulting at the same +0x528.
	 */
	/*
	 * Resolve the netdev by name, per slot.
	 *
	 * vdev_netdev was one string for the whole module, so with both radios
	 * armed it named the right interface for one slot and left the other
	 * registered against the dummy netdev - and every frame NSS excepts on
	 * that radio, EAPOL included, would be delivered nowhere.  So there is
	 * one name per slot.
	 *
	 * Looking it up by the vif MAC instead was tried and is NOT safe: this
	 * runs from ath11k add_interface, dev_getbyhwaddr_rcu() can return a
	 * netdev that is still being registered, and dev_hold() on it faults on
	 * pcpu_refcnt at +0x528 - the same fault the note above records for
	 * vif->netdev.  By the time the name exists the netdev is registered,
	 * which is why the name lookup is the safe one.
	 */
	/*
	 * The name ath11k gave us, which is the one that is right whatever the
	 * phy numbering came out as this boot.  The module parameters stay as
	 * an override for a driver too old to fill it in.
	 */
	probe_vdev_ndev = vif->netdev;
	if (!probe_vdev_ndev)
		pr_warn(PFX "slot %u has no netdev - registering the dummy\n",
			probe_cur);
	reg = nss_register_wifi_vdev_if(probe_ctx, ifnum, probe_vdev_data_cb,
					probe_vdev_ext_cb, probe_vdev_msg_cb,
					probe_vdev_ndev ? : probe_ndev, 0);
	pr_info(PFX "vdev if=%d registered (ret=%u) for vdev_id=%u mac=%pM opmode=%u netdev=%s\n",
		ifnum, reg, vif->vdev_id, vif->mac_addr, vif->nss_opmode,
		probe_vdev_ndev ? probe_vdev_ndev->name : "(dummy)");

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg) {
		nss_unregister_wifi_vdev_if(ifnum);
		nss_dynamic_interface_dealloc_node(ifnum,
			NSS_DYNAMIC_INTERFACE_TYPE_VAP);
		return -ENOMEM;
	}

	cfg = &msg->msg.vdev_config;
	ether_addr_copy(cfg->mac_addr, vif->mac_addr);
	cfg->radio_ifnum = probe_radio_ifnum;
	cfg->vdev_id	 = vif->vdev_id;
	cfg->opmode	 = vif->nss_opmode;

	nss_wifi_vdev_msg_init(msg, ifnum,
			       NSS_WIFI_VDEV_INTERFACE_CONFIGURE_MSG,
			       sizeof(*cfg), NULL, NULL);
	status = nss_wifi_vdev_tx_msg(probe_ctx, msg);
	pr_info(PFX "VDEV_CONFIGURE -> status=%d\n", status);

	if (status != NSS_TX_SUCCESS) {
		kfree(msg);
		nss_unregister_wifi_vdev_if(ifnum);
		nss_dynamic_interface_dealloc_node(ifnum,
			NSS_DYNAMIC_INTERFACE_TYPE_VAP);
		return -EIO;
	}

	memset(msg, 0, sizeof(*msg));
	ether_addr_copy(msg->msg.vdev_enable.mac_addr, vif->mac_addr);
	nss_wifi_vdev_msg_init(msg, ifnum, NSS_WIFI_VDEV_INTERFACE_UP_MSG,
			       sizeof(struct nss_wifi_vdev_enable_msg),
			       NULL, NULL);
	status = nss_wifi_vdev_tx_msg(probe_ctx, msg);
	pr_info(PFX "VDEV_UP -> status=%d\n", status);

	kfree(msg);

	/*
	 * The vendor ends osif_nss_ol_vap_create() with nss_if_change_mtu().  An
	 * interface NSS believes has no MTU is not one it will hand a frame to,
	 * which is the shape of "reaped from REO but never delivered".
	 */
	status = nss_if_change_mtu(probe_ctx, ifnum, ETH_DATA_LEN);
	pr_info(PFX "vdev MTU %d -> status=%d\n", ETH_DATA_LEN, status);

	/*
	 * Broadcast and multicast to the host, and intra-BSS forwarding on.
	 *
	 * MCBC_EXC_TO_HOST is what lets a DHCP discover reach dnsmasq at all.
	 * AP_BRIDGE is the station-to-station forwarding an AP owes its clients;
	 * NSS does it without the host either way, but saying so explicitly
	 * means the setting is not left to whatever the firmware defaults to.
	 */
	/*
	 * Tell the vdev what its frames look like.  The QSDK 10.4 driver, whose
	 * source is public, sends all three of these from
	 * osif_nss_ol_vdev_set_cfg() - and nothing here was sending any of
	 * them, leaving the encapsulation and decapsulation types to whatever
	 * the firmware defaults to.  If it defaults to raw or native-wifi
	 * rather than Ethernet then the frames it hands up carry a rawmode
	 * metadata header this side never pulls, and its own handling of
	 * broadcast differs too.  htt_pkt_type_ethernet is 2, which is the same
	 * value ath11k uses for frame_mode and for HAL_TCL_ENCAP_TYPE_ETHERNET.
	 *
	 * drop_unenc matters for an open AP in particular: a vdev that drops
	 * unencrypted frames drops everything this test sends.
	 */
	probe_vdev_cmd(ifnum, NSS_WIFI_VDEV_ENCAP_TYPE_CMD,
		       probe_encap_type, "ENCAP_TYPE");
	probe_vdev_cmd(ifnum, NSS_WIFI_VDEV_DECAP_TYPE_CMD,
		       probe_decap_type, "DECAP_TYPE");
	probe_vdev_cmd(ifnum, NSS_WIFI_VDEV_DROP_UNENC_CMD,
		       probe_drop_unenc, "DROP_UNENC");

	probe_vdev_cmd(ifnum, NSS_WIFI_VDEV_CFG_MCBC_EXC_TO_HOST_CMD,
		       probe_mcbc_to_host, "MCBC_EXC_TO_HOST");
	if (probe_vdev_sec)
		probe_vdev_cmd(ifnum, NSS_WIFI_VDEV_SECURITY_TYPE_CMD,
			       probe_sec_type, "SECURITY_TYPE");

	probe_vdev_cmd(ifnum, NSS_WIFI_VDEV_CFG_AP_BRIDGE_CMD,
		       probe_ap_bridge, "AP_BRIDGE");

	/*
	 * Where a received frame goes.  The vendor's
	 * osif_nss_wifili_set_default_nexthop() points every wifi vdev at
	 * NSS_ETH_RX_INTERFACE (special interface 2), which is what hands the
	 * frame to NSS's own Ethernet Rx processing - the bridging and routing
	 * lookup, and from there to an accelerated flow.  It is also the reason
	 * a wifi frame can reach eth1 without Linux seeing it.
	 *
	 * That is the destination worth having, but it is a destination only
	 * once ECM has installed rules.  Until then NSS's Ethernet Rx path has
	 * nowhere to send a frame and consumes it, which is precisely why a
	 * client can associate, be authorised, have its unicast excepted to the
	 * host - and still never get an address, because the broadcast DHCP
	 * discover and ARP it needs went down this path instead.  The counters
	 * say so: reo_reaped runs ahead of rx_deliverd by about the number of
	 * frames missing, and rx_intra_bss_mcast accounts for them.
	 *
	 * So leave it unset by default.  A vdev with no next hop delivers to
	 * the netdev it was registered with, which is the AP interface, which
	 * is a bridge port - the whole AP works, at the cost of every frame
	 * crossing Linux.  Setting it to NSS_ETH_RX_INTERFACE is then the
	 * second step, once ECM is there to give it somewhere to go.
	 */
	if (probe_next_hop) {
		status = nss_wifi_vdev_base_set_next_hop(probe_ctx,
							 probe_next_hop);
		pr_info(PFX "vdev default next hop -> %u status=%d\n",
			probe_next_hop, status);
	} else {
		pr_info(PFX "vdev next hop left unset - frames come to %s\n",
			probe_vdev_ndev ? probe_vdev_ndev->name : "(dummy)");
	}

	if (probe_vdev_nh) {
		status = nss_wifi_vdev_set_next_hop(probe_ctx, ifnum,
						    probe_vdev_nh);
		pr_info(PFX "vdev %d per-VAP next hop -> %u status=%d\n",
			ifnum, probe_vdev_nh, status);
	}

	probe_vdev_ifnum = ifnum;

	/*
	 * Only now: the hook is what ath11k calls from its Tx path, and it must
	 * not fire before the interface it sends to is configured and up.
	 */
	probe_tx_ctx = probe_ctx;

	/*
	 * Only now: the peer hook needs a vdev to attach peers to, and arming
	 * it earlier would have it fire for a station that associates while
	 * the vdev is still being built.
	 */
	pr_info(PFX "vdev if=%d ready; peers come from the stage 4 poll\n",
		ifnum);

	/*
	 * probe_cur, not the module parameter.  This runs from the vif hook,
	 * which sets probe_cur to the slot that owns the vdev; passing the
	 * parameter put the second radio's BSS peer on the first radio's
	 * wifili instance.  Same mistake, and same signature, as the EAPOL
	 * routing bug recorded in probe_vdev_deliver().
	 */
	probe_self_peer_create(probe_cur, vif->mac_addr, probe_peer_dma_dev);

	probe_warm_peers_run(probe_peer_dma_dev);

	return 0;
}

/*
 * Tear the vdev down at most once.
 *
 * This runs both from the vif hook, in hostapd's context as mac80211 removes
 * the interface, and from module exit.  Nothing serialised the two, so both
 * could pass the ifnum test and both could dev_put() the same netdev.  That
 * underflows the refcount, the netdev is freed early, and the next dev_put or
 * dev_hold reads pcpu_refcnt through a reused pointer - the
 * 'Unable to handle kernel paging request at ffffff3f........528' Oops that
 * kept killing hostapd, which tore the AP down, which made the station
 * reassociate, which is what finally raced NSS into a freed peer entry.
 *
 * xchg() makes both claims atomic, so the loser of the race does nothing.
 */
static void probe_vdev_free(void)
{
	struct net_device *nd;
	int ifnum = xchg(&probe_vdev_ifnum, -1);

	if (ifnum < 0)
		return;

	probe_tx_ctx = NULL;

	nss_unregister_wifi_vdev_if(ifnum);
	nss_dynamic_interface_dealloc_node(ifnum,
		NSS_DYNAMIC_INTERFACE_TYPE_VAP);

	/*
	 * Only after the unregister: NSS must not be able to call back into a
	 * netdev we have already forgotten.  No dev_put - no reference was
	 * taken, which is also what stops two paths through here from
	 * underflowing the refcount and freeing the netdev early.
	 */
	nd = xchg(&probe_vdev_ndev, (struct net_device *)NULL);
	if (nd)
		pr_info(PFX "vdev netdev %s dropped\n", nd->name);
}


/*
 * probe_peer_reoq_setup()
 *	Tell NSS which of a peer's TIDs have an Rx reorder queue.
 *
 * struct nss_wifili_reo_tidq_msg is two fields, tid and peer_id: it is a
 * notification, not a handover of the descriptor, so the host does not have to
 * publish the queue address - NSS reads it from the peer the same way the
 * hardware does.  Sent straight after PEER_CREATE and before the authorise,
 * which is the order the vendor uses.
 *
 * TID 16 goes first on purpose.  Each sync round trip costs ~15 ms, so with
 * all 17 sent in numeric order the non-QoS TID would not be live until ~250 ms
 * after the peer exists - well after the first M2 arrives.
 */
static int probe_peer_reoq_setup(u32 peer_id)
{
	static const u8 order[] = {
		16, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
	};
	struct nss_wifili_msg *msg;
	nss_tx_status_t status;
	unsigned int i, n;
	int ok = 0, fail = 0;

	if (!probe_reoq)
		return 0;

	n = probe_reoq_tids;
	if (n > ARRAY_SIZE(order))
		n = ARRAY_SIZE(order);

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	for (i = 0; i < n; i++) {
		memset(msg, 0, sizeof(*msg));
		msg->msg.reotidqmsg.tid	    = order[i];
		msg->msg.reotidqmsg.peer_id = peer_id;

		nss_wifili_msg_init(msg, probe_wifili_if,
				    NSS_WIFILI_TID_REOQ_SETUP_MSG,
				    sizeof(struct nss_wifili_reo_tidq_msg),
				    probe_event_cb, NULL);

		status = nss_wifili_tx_msg_sync(probe_ctx, msg);
		if (status == NSS_TX_SUCCESS)
			ok++;
		else
			fail++;

		if (i == 0)
			pr_info(PFX "REOQ_SETUP peer=%u tid=%u -> status=%d (%s)\n",
				peer_id, order[i], status,
				sync_status_name(status));
	}

	pr_info(PFX "REOQ_SETUP peer=%u tids=%u ok=%d fail=%d\n",
		peer_id, n, ok, fail);

	kfree(msg);
	return fail ? -EIO : 0;
}

/*
 * probe_peer_create()
 *	Tell NSS about one peer.
 *
 * NSS keeps its per-peer state in memory the host allocates and DMA-maps -
 * osif_nss_wifili_pmem_alloc() in the vendor driver does exactly this, 1600
 * bytes per peer - and the physical address travels in the create message.  A
 * single block is enough here: the probe creates one peer.
 */
static int probe_peer_create(const struct ath11k_nss_peer_info *peer,
			     struct device *dma_dev)
{
	struct nss_wifili_msg *msg;
	struct nss_wifili_peer_msg *pm;
	nss_tx_status_t status;

	u32 pidx = peer->peer_id;

	if (pidx >= PROBE_MAX_PEERS) {
		pr_err(PFX "peer id %u is past the peer memory table\n", pidx);
		return -EINVAL;
	}

	probe_peer_mem_free_one(pidx);

	probe_peer_mem_va[pidx] = kzalloc(PROBE_PEER_MEM_SZ, GFP_KERNEL);
	if (!probe_peer_mem_va[pidx])
		return -ENOMEM;

	probe_peer_mem_pa[pidx] = dma_map_single(dma_dev,
						 probe_peer_mem_va[pidx],
						 PROBE_PEER_MEM_SZ,
						 DMA_TO_DEVICE);
	if (dma_mapping_error(dma_dev, probe_peer_mem_pa[pidx])) {
		pr_err(PFX "peer memory dma map failed\n");
		kfree(probe_peer_mem_va[pidx]);
		probe_peer_mem_va[pidx] = NULL;
		return -ENOMEM;
	}
	probe_peer_dma_dev = dma_dev;
	pr_info(PFX "peer %u memory %u B at paddr=%08x\n",
		pidx, PROBE_PEER_MEM_SZ, (u32)probe_peer_mem_pa[pidx]);

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	pm = &msg->msg.peermsg;
	ether_addr_copy(pm->peer_mac_addr, peer->mac_addr);
	pm->vdev_id	     = peer->vdev_id;
	pm->peer_id	     = peer->peer_id;
	pm->hw_ast_idx	     = peer->hw_peer_id;
	pm->tx_ast_hash	     = peer->ast_hash;
	pm->nss_peer_mem     = (u32)probe_peer_mem_pa[pidx];
	pm->psta_vdev_id     = peer->vdev_id;
	pm->peer_memory_size = PROBE_PEER_MEM_SZ;

	pr_info(PFX "--- PEER_CREATE mac=%pM peer_id=%u vdev_id=%u ast_hash=%u hw_ast=%u\n",
		peer->mac_addr, peer->peer_id, peer->vdev_id,
		peer->ast_hash, peer->hw_peer_id);

	nss_wifili_msg_init(msg, probe_wifili_if,
			    NSS_WIFILI_PEER_CREATE_MSG, sizeof(*pm),
			    probe_event_cb, NULL);

	status = nss_wifili_tx_msg_sync(probe_ctx, msg);
	pr_info(PFX "PEER_CREATE -> status=%d (%s)\n", status,
		sync_status_name(status));

	kfree(msg);

	if (status == NSS_TX_SUCCESS)
		probe_peer_reoq_setup(peer->peer_id);

	if (status == NSS_TX_SUCCESS && probe_auth_early) {
		int ae = probe_peer_auth(peer->peer_id, true);

		pr_info(PFX "auth_early peer=%u -> %d\n", peer->peer_id, ae);
	}

	return (status == NSS_TX_SUCCESS) ? 0 : -EIO;
}

/*
 * probe_peer_next_hop()
 *	Point one peer's next hop at the accelerated interface.
 *
 * The vendor does this per peer, keyed by MAC, in
 * osif_nss_li_vdev_set_peer_nexthop() (_1637):
 *	nss_wifi_vdev_set_peer_next_hop(ctx, vdev_ifnum, peer_mac, ifnum)
 * This probe never called it at all.  Setting only the vdev-wide next hop
 * already took the transfer from 9.6 MB / 9.3 s to 25.9 MB / 13.1 s, so the
 * per-peer half of the same mechanism is the obvious remaining gap.
 *
 * Which context to pass is not certain: _1637 reads its ctx from *(pdev+716)
 * while _1694 (the vdev-wide call) reads *(pdev+808), so those may be two
 * different contexts.  probe_ctx is used here because the vdev-wide call
 * already returns status=0 with it - if this one does not, that is the first
 * thing to change.
 *
 * Skipped when peer_next_hop is 0, which is the default.  It deliberately has
 * its OWN parameter rather than reusing next_hop: sharing one knob made the two
 * halves of the mechanism impossible to separate, and the first run with both
 * enabled went from 25.9 MB / 13.1 s down to 5.6 MB / 9.7 s without any way to
 * tell whether the per-peer message was harmful or simply being rejected.
 */
static unsigned int probe_peer_nh;
module_param_named(peer_next_hop, probe_peer_nh, uint, 0644);
MODULE_PARM_DESC(peer_next_hop,
		 "per-peer next hop interface; 0 disables the per-peer call"
		 " so next_hop can be tested on its own");

static void probe_peer_next_hop(const u8 *mac)
{
	nss_tx_status_t status;

	if (!probe_peer_nh || probe_vdev_ifnum < 0 || !probe_ctx)
		return;

	status = nss_wifi_vdev_set_peer_next_hop(probe_ctx,
						 (uint32_t)probe_vdev_ifnum,
						 mac, probe_peer_nh);
	pr_info(PFX "peer next hop %pM -> %u status=%d (%s)\n",
		mac, probe_peer_nh, status, sync_status_name(status));
}

/*
 * probe_peer_security()
 *	Tell NSS the peer's cipher.
 *
 * Sent once per packet type, which is what the vendor does.  security_type 0
 * is "none" in the WLAN driver's cipher numbering, which is what an open AP
 * has - and an open AP is the only kind that can associate at all while
 * ath11k's Tx is gated, because WPA2 needs the AP to send EAPOL.
 */
/*
 * probe_peer_delete()
 *	Drop a peer NSS still holds.
 *
 * The message carries the same struct as create; only the ids are read.  The
 * peer memory block is reused for the replacement, which is safe because the
 * firmware is done with it once the peer is gone.
 */
static int probe_peer_delete(u32 peer_id)
{
	struct nss_wifili_msg *msg;
	nss_tx_status_t status;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	msg->msg.peermsg.peer_id = peer_id;

	nss_wifili_msg_init(msg, probe_wifili_if,
			    NSS_WIFILI_PEER_DELETE_MSG,
			    sizeof(struct nss_wifili_peer_msg),
			    probe_event_cb, NULL);

	status = nss_wifili_tx_msg_sync(probe_ctx, msg);
	pr_info(PFX "PEER_DELETE peer=%u -> status=%d (%s)\n",
		peer_id, status, sync_status_name(status));

	probe_peer_mem_free_one(peer_id);

	kfree(msg);
	return (status == NSS_TX_SUCCESS) ? 0 : -EIO;
}

static int probe_peer_security(u32 peer_id)
{
	struct nss_wifili_msg *msg;
	nss_tx_status_t status;
	int pkt_type;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	for (pkt_type = 0; pkt_type < 2; pkt_type++) {
		memset(msg, 0, sizeof(*msg));
		msg->msg.securitymsg.peer_id	   = peer_id;
		msg->msg.securitymsg.pkt_type	   = pkt_type;
		msg->msg.securitymsg.security_type = probe_sec_type;

		nss_wifili_msg_init(msg, probe_wifili_if,
				    NSS_WIFILI_PEER_SECURITY_TYPE_MSG,
				    sizeof(struct nss_wifili_peer_security_type_msg),
				    probe_event_cb, NULL);

		status = nss_wifili_tx_msg_sync(probe_ctx, msg);
		pr_info(PFX "PEER_SECURITY peer=%u pkt_type=%d sec=%u -> status=%d (%s)\n",
			peer_id, pkt_type, probe_sec_type, status,
			sync_status_name(status));
		msleep(20);
	}

	kfree(msg);
	return 0;
}

/*
 * probe_peer_auth()
 *	Mark the peer authorised inside NSS.
 *
 * A peer NSS considers unauthorised is one whose frames it is supposed to
 * refuse to forward, which is the shape of "reo_reaped climbs and rx_deliverd
 * stays zero".  The vendor normalises the flag to 0 or 1 and sends 4 bytes.
 */
static int probe_peer_auth(u32 peer_id, bool authorized)
{
	struct nss_wifili_msg *msg;
	nss_tx_status_t status;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	msg->msg.peer_auth.peer_id   = peer_id;
	msg->msg.peer_auth.auth_flag = authorized ? 1 : 0;

	nss_wifili_msg_init(msg, probe_wifili_if,
			    probe_auth_msg_type,
			    sizeof(struct nss_wifili_peer_update_auth_flag),
			    probe_event_cb, NULL);

	status = nss_wifili_tx_msg_sync(probe_ctx, msg);
	pr_info(PFX "PEER_AUTH peer=%u auth=%d type=%u -> status=%d (%s)\n",
		peer_id, authorized ? 1 : 0, probe_auth_msg_type, status,
		sync_status_name(status));

	kfree(msg);
	return (status == NSS_TX_SUCCESS) ? 0 : -EIO;
}

/*
 * probe_peek_rings()
 *	Read what the hardware itself has published, from the host's shadow blocks.
 *
 * Neither side's counters can settle the question this answers.  NSS says it
 * posted 1023 buffers and reaped nothing; ath11k is gated off the rings and so
 * counts nothing at all.  Both are consistent with "the hardware never produced
 * a frame" and with "the hardware produced frames NSS is not reaping".
 *
 * The shadow blocks are written by the hardware and are plain host memory:
 *
 *   rdptr[ring_id]                        a dst ring's head - entries produced
 *   rdptr[ring_id]                        a src ring's tail - entries consumed
 *   wrptr[ring_id - lmac_rings_start_id]  an lmac src ring's head - published
 *
 * So a non-zero rxdma tail means the WLAN hardware is taking the buffers NSS
 * posted, and a non-zero reo head means it is delivering frames.  The values
 * are byte offsets into the ring, not entry counts.
 */
static void probe_peek_rings(struct ath11k_nss_dp_info *info)
{
	const u32 *rd = info->shadow_rdptr_vaddr;
	const u32 *wr = info->shadow_wrptr_vaddr;
	u32 id;
	int i;

	if (!rd || !wr) {
		pr_info(PFX "PEEK: shadow blocks not exported\n");
		return;
	}

	for (i = 0; i < info->num_reo_dest_rings; i++)
		if (info->reo_dest_ring[i].valid) {
			id = info->reo_dest_ring[i].ring_id;
			pr_info(PFX "PEEK reo_dest[%d] id=%u hw_head=%u (of %u bytes)\n",
				i, id, rd[id],
				info->reo_dest_ring[i].num_entries *
				info->reo_dest_ring[i].entry_size * 4);
		}

	if (info->reo_exception_ring.valid) {
		id = info->reo_exception_ring.ring_id;
		pr_info(PFX "PEEK reo_exception id=%u hw_head=%u\n", id, rd[id]);
	}

	if (info->rx_rel_ring.valid) {
		id = info->rx_rel_ring.ring_id;
		pr_info(PFX "PEEK rx_release id=%u hw_head=%u\n", id, rd[id]);
	}

	for (i = 0; i < info->num_tcl_data_rings; i++)
		if (info->tx_comp_ring[i].valid) {
			id = info->tx_comp_ring[i].ring_id;
			pr_info(PFX "PEEK tx_comp[%d] id=%u hw_head=%u\n",
				i, id, rd[id]);
		}

	for (i = 0; i < ATH11K_NSS_MAX_PDEV; i++) {
		const struct ath11k_nss_srng *rx = &info->pdev[i].rxdma_ring;

		if (!info->pdev[i].valid || !rx->valid)
			continue;

		id = rx->ring_id;
		pr_info(PFX "PEEK rxdma[%d] id=%u sw_head=%u hw_tail=%u (of %u bytes) swdesc=%u\n",
			i, id,
			(id >= info->lmac_rings_start_id) ?
				wr[id - info->lmac_rings_start_id] : 0,
			rd[id],
			rx->num_entries * rx->entry_size * 4,
			info->pdev[i].num_rx_swdesc);
	}

	for (i = 0; i < info->num_reo_dest_rings; i++)
		probe_readback("reo_dest", &info->reo_dest_ring[i]);
	for (i = 0; i < info->num_tcl_data_rings; i++) {
		probe_readback("tcl_data", &info->tcl_ring[i]);
		probe_readback("tx_comp", &info->tx_comp_ring[i]);
	}
	probe_readback("reo_exception", &info->reo_exception_ring);
	probe_readback("rx_release", &info->rx_rel_ring);
	probe_readback("reo_reinject", &info->reo_reinject_ring);
}

/*
 * probe_peer_hook()
 *	ath11k has a station; give NSS the same peer, now.
 *
 * Runs in process context from ath11k's sta_state handler, before the station
 * can send a data frame.  That timing is the whole point: a frame reaped for a
 * peer NSS has never seen traps the firmware within milliseconds, with nothing
 * in its log ring to say why.  Polling for the station cannot win that race.
 */
static int probe_run(unsigned int soc_idx, unsigned int stage,
		     u32 swdesc, bool quiesce, u8 mem_profile)
{
	struct ath11k_nss_dp_info *info;
	struct nss_wifili_msg *msg;
	struct nss_wifili_init_msg *init;
	nss_tx_status_t status;
	unsigned long left;
	bool poison = (stage == 0);

	/*
	 * Select the slot this run is for.  Everything below reaches the
	 * per-slot state through probe_cur, so arming SoC 1 without this would
	 * write its wifili interface and radio interface into slot 0.
	 */
	if (soc_idx < PROBE_MAX_SOC)
		probe_cur = soc_idx;
	u32 dev_base;
	int ret = 0, i;

	if (soc_idx >= PROBE_MAX_SOC) {
		pr_err(PFX "soc %u out of range\n", soc_idx);
		return -EINVAL;
	}
	probe_ctx         = probe_ctx_soc[soc_idx];
	probe_wifili_if   = probe_if_soc[soc_idx];
	probe_radio_ifnum = probe_radio_ifnum_soc[soc_idx];
	probe_scheme_id   = probe_scheme_id_soc[soc_idx];

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (!info || !msg) {
		ret = -ENOMEM;
		goto out;
	}

	ret = ath11k_nss_get_dp_info(soc_idx, info);
	if (ret) {
		pr_err(PFX "no ath11k soc %u (%d)\n", soc_idx, ret);
		goto out;
	}
	dev_base = info->dev_base_paddr;

	pr_info(PFX "=== soc %u stage %u swdesc=%u quiesce=%d memprofile=%u ===\n",
		soc_idx, stage, swdesc, quiesce, mem_profile);
	pr_info(PFX "  dev_base=%08x shadow_rd=%08x shadow_wr=%08x lmac_start=%u\n",
		dev_base, info->shadow_rdptr_paddr,
		info->shadow_wrptr_paddr, info->lmac_rings_start_id);
	pr_info(PFX "  tlv_size=%u rx_buf_len=%u target_type=%u radios=%u tcl=%u reo=%u\n",
		info->rx_tlv_size, info->rx_buf_len, info->nss_target_type,
		info->num_radios, info->num_tcl_data_rings,
		info->num_reo_dest_rings);
	pr_info(PFX "  (hwreg values below are already dev_base-relative -> physical)\n");

	/*
	 * Remember the DMA-capable device now.  The peer hook runs from
	 * ath11k's sta_state path with no access to this info, and it cannot
	 * ask for it: the getter needs ar->conf_mutex, which that path holds.
	 */
	if (info->dma_dev)
		probe_peer_dma_dev = info->dma_dev;

	/*
	 * Stage 5 is read-only: it sends nothing and registers nothing, so it is
	 * safe against a firmware that is mid-run or already wedged.
	 */
	if (stage == 5) {
		probe_peek_rings(info);
		ret = 0;
		goto out;
	}

	for (i = 0; i < info->num_tcl_data_rings; i++) {
		dump_srng("tcl_data", &info->tcl_ring[i], dev_base);
		dump_srng("tx_comp", &info->tx_comp_ring[i], dev_base);
	}
	for (i = 0; i < info->num_reo_dest_rings; i++)
		dump_srng("reo_dest", &info->reo_dest_ring[i], dev_base);
	dump_srng("reo_exception", &info->reo_exception_ring, dev_base);
	dump_srng("rx_release", &info->rx_rel_ring, dev_base);
	dump_srng("reo_reinject", &info->reo_reinject_ring, dev_base);

	/* ---- build the init message ---------------------------------- */
	init = &msg->msg.init;
	init->hssm.dev_base_addr	 = dev_base;
	init->hssm.shadow_rdptr_mem_addr = info->shadow_rdptr_paddr;
	init->hssm.shadow_wrptr_mem_addr = info->shadow_wrptr_paddr;
	init->hssm.lmac_rings_start_id	 = info->lmac_rings_start_id;

	/*
	 * Declare only rings that were actually allocated.  The first run
	 * claimed three TCL rings when this hardware has one, and the firmware
	 * did not object - so be exact here and let the poison run be the thing
	 * that tests its validation.
	 */
	init->num_tcl_data_rings = 0;
	for (i = 0; i < info->num_tcl_data_rings; i++)
		if (info->tcl_ring[i].valid)
			init->num_tcl_data_rings++;

	init->num_reo_dest_rings = 0;
	for (i = 0; i < info->num_reo_dest_rings; i++)
		if (info->reo_dest_ring[i].valid)
			init->num_reo_dest_rings++;

	/*
	 * Declaring fewer REO rings than ath11k allocated is legitimate as long
	 * as the hash map sends nothing to the ones left out - which is exactly
	 * what narrowing does.  Declaring four while routing to one is the
	 * mismatch worth being able to rule out.
	 */
	if (probe_reo_rings && probe_reo_rings < init->num_reo_dest_rings)
		init->num_reo_dest_rings = probe_reo_rings;

	init->target_type	 = probe_target_type ? : info->nss_target_type;
	init->wrip.tlv_size	 = probe_tlv_size ? : info->rx_tlv_size;
	init->wrip.rx_buf_len	 = probe_rx_buf_len ? : info->rx_buf_len;
	init->soc_mem_profile	 = mem_profile;
	init->flags		 = probe_init_flags;

	/*
	 * Print after the assignments, not before them: this used to sit above
	 * and so always reported tlv=0 rx_buf_len=0, which is not what was sent
	 * and wasted time during the QCN6122 hunt.
	 */
	pr_info(PFX "declaring tcl=%u reo=%u (narrow_reo=%d) tlv=%u rx_buf_len=%u\n",
		init->num_tcl_data_rings, init->num_reo_dest_rings,
		probe_narrow_reo, init->wrip.tlv_size, init->wrip.rx_buf_len);

	for (i = 0; i < info->num_tcl_data_rings; i++) {
		fill_srng_msg(&init->tcl_ring_info[i], &info->tcl_ring[i],
			      dev_base);
		fill_srng_msg(&init->tx_comp_ring[i], &info->tx_comp_ring[i],
			      dev_base);
	}
	for (i = 0; i < info->num_reo_dest_rings; i++)
		fill_srng_msg(&init->reo_dest_ring[i], &info->reo_dest_ring[i],
			      dev_base);

	fill_srng_msg(&init->reo_exception_ring, &info->reo_exception_ring,
		      dev_base);
	fill_srng_msg(&init->rx_rel_ring, &info->rx_rel_ring, dev_base);
	fill_srng_msg(&init->reo_reinject_ring, &info->reo_reinject_ring,
		      dev_base);

	if (info->dma_dev && !probe_alloc_tx_pages(info->dma_dev)) {
		init->wtdim.num_tx_desc	     = PROBE_NUM_TX_DESC;
		init->wtdim.num_tx_desc_ext  = PROBE_NUM_TX_DESC;
		init->wtdim.num_pool	     = info->num_radios ?: 1;
		init->wtdim.num_memaddr	     = probe_tx_pages;
		init->wtdim.ext_desc_page_num = probe_tx_ext_first_page;
		init->wtdim.num_tx_device_limit = probe_tx_device_limit;
		init->tx_sw_internode_queue_size = probe_internode_q;
		for (i = 0; i < probe_tx_pages; i++) {
			init->wtdim.memory_addr[i] = (u32)probe_tx_page_pa[i];
			init->wtdim.memory_size[i] = PROBE_TX_PAGE_SZ;
		}
	}

	if (poison) {
		pr_info(PFX "*** CONTROL RUN: poisoning the ring description ***\n");
		poison_init(init);
	}

	/* ---- register and send --------------------------------------- */
	/* Idempotent: a second run reuses the registration the first made. */
	if (!probe_ctx) {
		if (probe_force_if ? (probe_force_if == 1) : (soc_idx != 0)) {
			probe_wifili_if = nss_get_available_wifili_external_if();
			if (probe_wifili_if == -1) {
				pr_err(PFX "no external wifili interface free\n");
				ret = -ENODEV;
				goto out;
			}
			pr_info(PFX "slot %u -> external wifili interface %d\n",
				probe_cur, probe_wifili_if);
		}

		probe_ctx = nss_register_wifili_if(probe_wifili_if,
						   probe_data_cb, probe_data_cb,
						   probe_event_cb, probe_ndev, 0);
		if (!probe_ctx) {
			pr_err(PFX "nss_register_wifili_if failed\n");
			ret = -ENODEV;
			goto out;
		}
		pr_info(PFX "registered, ctx=%px\n", probe_ctx);
	}

	/*
	 * Stage 4 works on a SoC that is already initialised and started, so it
	 * must not send any of this.  A SOC_RESET while the Rx path is running
	 * leaves the firmware answering EVERSION and ELENGTH to everything
	 * afterwards, and the PDEV_INIT that then fails frees the radio
	 * interface the vdev is about to ask for.
	 */
	if (stage == 4) {
		pr_info(PFX "STAGE 4 only: vdev + peer on an already-started SoC\n");
		goto vdev_peer;
	}

	/*
	 * Narrow REO to destination ring 0 before the firmware is told about
	 * the rings.  NSS reaps ring 0 only; with ath11k's four-way hash split
	 * in place most frames land in rings nothing services.
	 */
	if (probe_narrow_reo) {
		ret = ath11k_nss_dp_reo_to_ring0(soc_idx);
		if (ret)
			pr_warn(PFX "could not narrow REO hash map (%d)\n", ret);
	} else {
		pr_info(PFX "leaving ath11k's REO hash map alone\n");
	}

	/* clear any state a previous run left behind */
	probe_send_reset(probe_ctx);
	msleep(200);

	nss_wifili_msg_init(msg, probe_wifili_if,
			    NSS_WIFILI_INIT_MSG,
			    sizeof(struct nss_wifili_init_msg),
			    probe_event_cb, NULL);

	reinit_completion(&probe_done);
	probe_response = -1;

	pr_info(PFX "STAGE 1: NSS_WIFILI_INIT_MSG (%zu bytes)\n",
		sizeof(struct nss_wifili_init_msg));

	status = nss_wifili_tx_msg_sync(probe_ctx, msg);
	pr_info(PFX "INIT -> status=%d (%s)\n", status,
		sync_status_name(status));

	if (status != NSS_TX_SUCCESS) {
		left = wait_for_completion_timeout(&probe_done,
						   msecs_to_jiffies(3000));
		pr_info(PFX "RESULT: INIT rejected (status=%d, async response=%d, waited=%lu)\n",
			status, probe_response, left);
		goto unreg;
	}

	if (poison) {
		pr_warn(PFX "RESULT: firmware ACCEPTED POISONED input - INIT validation is shallow\n");
		goto unreg;
	}

	pr_info(PFX "RESULT: firmware ACCEPTED the ring description\n");
	if (stage < 2)
		goto unreg;

	/*
	 * Beyond here the firmware starts driving hardware rather than being
	 * told about it, and a firmware crash from this point takes eth0 and
	 * eth1 with it - NSS owns them - so the box goes silent even though the
	 * host kernel is fine.  Everything above has already been logged.
	 */
	if (quiesce) {
		pr_info(PFX "quiescing the ath11k data path before PDEV_INIT\n");
		ret = ath11k_nss_dp_quiesce(soc_idx, true);
		pr_info(PFX "quiesce -> %d\n", ret);
		msleep(100);
	}

	pr_info(PFX "STAGE 2: PDEV_INIT\n");
	for (i = 0; i < ATH11K_NSS_MAX_PDEV; i++)
		probe_send_pdev_init(info, i, swdesc);

	if (probe_htt_resetup) {
		int hret = ath11k_nss_dp_htt_refill_setup(soc_idx);

		pr_info(PFX "HTT refill ring re-announce -> %d\n", hret);
	}
	msleep(200);

	if (stage >= 3) {
		pr_info(PFX "STAGE 3: START\n");
		probe_send_simple(probe_ctx, NSS_WIFILI_START_MSG, "START");

		msleep(200);
	}

vdev_peer:
	if (stage >= 4) {
		struct ath11k_nss_vif vifs[ATH11K_NSS_MAX_VIF];
		struct ath11k_nss_peer_info peers[ATH11K_NSS_MAX_PEER];
		int nv, np, pi;

		pr_info(PFX "STAGE 4: vdev + peer\n");

		nv = ath11k_nss_get_vifs(soc_idx, vifs, ARRAY_SIZE(vifs));
		np = ath11k_nss_get_peers(soc_idx, peers, ARRAY_SIZE(peers));
		pr_info(PFX "ath11k reports %d vif(s), %d peer(s)\n", nv, np);

		for (i = 0; i < nv; i++)
			pr_info(PFX "  vif  mac=%pM vdev_id=%u opmode=%u up=%u radio=%u\n",
				vifs[i].mac_addr, vifs[i].vdev_id,
				vifs[i].nss_opmode, vifs[i].is_up,
				vifs[i].radio_id);
		for (i = 0; i < np; i++)
			pr_info(PFX "  peer mac=%pM peer_id=%u vdev_id=%u ast_hash=%u auth=%u\n",
				peers[i].mac_addr, peers[i].peer_id,
				peers[i].vdev_id, peers[i].ast_hash,
				peers[i].is_authorized);

		if (nv > 0)
			probe_vdev_create(&vifs[0]);
		else
			pr_warn(PFX "no vif to hand over - bring an AP up first\n");

		/*
		 * Hand over the station, not the AP's own peer - ath11k lists
		 * both, and the AP's own has the vif's MAC.
		 *
		 * Do it as soon as the station exists, before it is authorised:
		 * that is the vendor's order, and it has to be.  An authorised
		 * station is the *result* of a WPA2 handshake, and the EAPOL of
		 * that handshake only reaches hostapd if NSS already knows the
		 * peer well enough to except its frames to the host.  Waiting
		 * for authorisation first is a deadlock.
		 *
		 * The auth flag is then resent on every run, so a later run
		 * promotes the peer once ath11k reports it authorised.
		 */
		pi = -1;
		for (i = 0; i < np; i++) {
			if (nv > 0 &&
			    ether_addr_equal(peers[i].mac_addr, vifs[0].mac_addr))
				continue;
			pi = i;
			break;
		}

		if (pi < 0) {
			pr_info(PFX "no station yet - vdev is up, run stage 4 again once one associates\n");
		} else {
			if (probe_peer_created &&
			    probe_peer_id != peers[pi].peer_id) {
				pr_info(PFX "station re-associated: peer id %u -> %u, replacing\n",
					probe_peer_id, peers[pi].peer_id);
				probe_peer_delete(probe_peer_id);
				probe_peer_created = false;
				probe_peer_auth_sent = -1;
			}

			if (!probe_peer_created && info->dma_dev &&
			    !probe_peer_create(&peers[pi], info->dma_dev)) {
				probe_peer_security(peers[pi].peer_id);
				probe_peer_next_hop(peers[pi].mac_addr);
				probe_peer_created = true;
				probe_peer_id = peers[pi].peer_id;
			}

			if (probe_peer_created &&
			    probe_peer_auth_sent != !!peers[pi].is_authorized) {
				if (!probe_peer_auth(peers[pi].peer_id,
						     peers[pi].is_authorized))
					probe_peer_auth_sent =
						!!peers[pi].is_authorized;
			} else if (probe_peer_created) {
				pr_info(PFX "peer %u already at auth=%d - nothing to do\n",
					peers[pi].peer_id, probe_peer_auth_sent);
			}
		}

		msleep(200);
	}

	{
		int t;

		for (t = 0; t < 32; t++)
			if (probe_ext_type_count[t])
				pr_info(PFX "  ext pkt_type %d: %u\n",
					t, probe_ext_type_count[t]);
	}
	pr_info(PFX "rx handed=%d eapol=%d nwifi=%d/%d | dropped: nodev=%d off=%d short=%d\n",
		atomic_read(&probe_rx_handed),
		atomic_read(&probe_rx_eapol),
		atomic_read(&probe_rx_nwifi),
		atomic_read(&probe_rx_nwifi_fail),
		atomic_read(&probe_rx_drop_nodev),
		atomic_read(&probe_rx_drop_off),
		atomic_read(&probe_rx_drop_short));
	pr_info(PFX "notify: wifili=%d vdev=%d\n",
		atomic_read(&probe_notify_count),
		atomic_read(&probe_vdev_notify_count));
	pr_info(PFX "vdev delivered rx=%d ext=%d | host tx via NSS ok=%d fail=%d\n",
		atomic_read(&probe_vdev_rx), atomic_read(&probe_vdev_rx_ext),
		atomic_read(&probe_vdev_tx_ok),
		atomic_read(&probe_vdev_tx_fail));

	if (quiesce) {
		pr_info(PFX "resuming the ath11k data path\n");
		ath11k_nss_dp_quiesce(soc_idx, false);
	}
	ret = 0;

unreg:
	/*
	 * The radio interface, the vdev, the peer memory and the host-side
	 * registration are all deliberately left in place: the firmware keeps
	 * running after this write() returns and traffic only arrives once a
	 * client associates.  rmmod frees them.
	 *
	 * The registration in particular must outlive the run.  The peer hook
	 * fires from ath11k's sta_state path, outside any run, and sends peer
	 * messages with this context - unregistering here left it NULL and the
	 * first station to associate took the host down with it.  Unregistering
	 * a live SoC would also leave NSS unable to deliver anything upward.
	 */
	pr_info(PFX "=== run complete ===\n");

out:
	probe_ctx_soc[soc_idx]         = probe_ctx;
	probe_if_soc[soc_idx]          = probe_wifili_if;
	probe_radio_ifnum_soc[soc_idx] = probe_radio_ifnum;
	probe_scheme_id_soc[soc_idx]   = probe_scheme_id;
	kfree(msg);
	kfree(info);
	return ret;
}

/*
 * ---------------------------------------------------------------------------
 * Driven by ath11k rather than by a debugfs write.
 *
 * The debugfs "run" file stays - it is how every stage of this was found, and
 * it is still the way to re-run one by hand - but nothing has to use it.
 * ath11k calls dp_ready once its rings exist and before anything can deliver
 * into them, calls the vif hook when a vdev appears, and calls the peer hook
 * when a station associates.  That is the whole handover, in the order the
 * hardware requires it, without a script that has to guess at timing.
 */
static bool probe_auto = true;
module_param_named(auto_start, probe_auto, bool, 0444);
MODULE_PARM_DESC(auto_start,
		 "hand the data path over from ath11k's own bring-up");

/*
 * Override for num_rx_swdesc; 0 means "send what the vendor sends for this
 * board", i.e. PROBE_RX_SW_DESC_NUM (4096).
 *
 * Both vendor formulas land on 4096 for a radio that is not the internal 2.4
 * GHz one, and the QCA_LOWMEM_CONFIG defaults in cfg_dp.h are what this 256 MB
 * IPQ5018 board builds with (rx_sw_desc_num 4096, rx_sw_desc_weight 1).  An
 * earlier version of this block said "3 x ath11k's rx_refill_buf_ring.bufs_max"
 * and the code multiplied by 3 to match; the weight of 3 is the full-memory
 * #else branch, so both the text and the code were wrong.  Full derivation is
 * at the swdesc computation in probe_send_pdev_init().
 *
 * The open ath11k NSS reference implementation
 * (openwrt-nss-edma, 199-002-ath11k_nss-add-nss-driver-interface.patch) sends
 *
 *	pdevmsg->num_rx_swdesc =
 *		WIFILI_RX_DESC_POOL_WEIGHT * DP_RXDMA_BUF_RING_SIZE;   // 3 * 1024
 *
 * and the weight of 3 is not arbitrary: ath11k can have three times the refill
 * ring outstanding at once and sizes its own idr to match,
 *	dp_rx.c:401  idr_alloc(..., (rx_ring->bufs_max * 3) + 1, ...)
 * so the firmware's pool has to cover the same index range.
 *
 * UNRESOLVED CONFLICT - do not quietly pick a side again.  The comment that
 * used to stand here said the opposite, and said it from experience:
 *
 *	"On the QCN6122 the real pool is 1024, and forcing 2048 there is what
 *	 crashed the NSS core ... it indexes past the end of a 1024-entry pool
 *	 and every thread ends up parked in the fault handler."
 *
 * That observation was real - swdesc=2048 did break peer creation, and 0 fixed
 * it - but those runs also carried tlv_size=256/rx_buf_len=1792, so the two
 * variables were confounded.  The vendor binary is a third voice: it writes
 * 2048 unconditionally, with no target-type branch (lmac_get_tgt_type()'s
 * result is discarded), which contradicts the "only for tgt_type==29" claim
 * that comment also made.
 *
 * So: three sources, three readings - under-declaring is the bug (reference),
 * over-declaring is the bug (earlier experiment), or it is a flat constant
 * (vendor).  The current code follows the reference because its weight has a
 * mechanism behind it and because the crash appears only once sustained Rx
 * traffic could exhaust the pool.  If 3x also crashes, that eliminates the
 * whole under-declaring hypothesis rather than leaving it open.
 */
static u32 probe_swdesc;
module_param_named(swdesc, probe_swdesc, uint, 0644);
MODULE_PARM_DESC(swdesc,
		 "Rx software descriptors per pdev; 0 computes"
		 " 3 x ath11k's pool size, as the open NSS reference does");

static unsigned int probe_mem_profile = 1;
module_param_named(mem_profile, probe_mem_profile, uint, 0644);

/*
 * The Q6 writes its fatal record into SMEM item 421 - WCSS_CRASH_REASON, which
 * qcom_q6v5_mpd.c hands to every protection domain, root and both user PDs
 * alike.  qcom_q6v5.c then prints that item with "%s", and the v2.1 record puts
 * a NUL immediately after its "err_smem_ver.2.1:" header, so the log only ever
 * shows the header and the actual fault text never escapes.  That driver also
 * registers no coredump segments at all, so /sys/class/devcoredump/ can never
 * produce a blob for this Q6 no matter which mode it is put in.  Reading the
 * item raw is the only way to see why the firmware died.
 */
#define PROBE_SMEM_HOST_ANY	(-1)

static unsigned int probe_smem_item = 421;
module_param_named(smem_item, probe_smem_item, uint, 0644);
MODULE_PARM_DESC(smem_item, "SMEM item holding the Q6 crash record (421)");

static unsigned int probe_smem_len = 1024;
module_param_named(smem_len, probe_smem_len, uint, 0644);
MODULE_PARM_DESC(smem_len, "bytes of the crash record to dump");

static unsigned int probe_smem_watch;
module_param_named(smem_watch, probe_smem_watch, uint, 0444);
MODULE_PARM_DESC(smem_watch,
		 "poll the crash record every N ms and dump it the moment it"
		 " changes; 0 disables the watcher");

/*
 * probe_ascii_runs()
 *	Print every printable run in a buffer.
 *
 * The fault text is a string inside a mostly binary record, so the runs are
 * what matters; the hexdump that follows is only there for the register values
 * around them.
 */
static void probe_ascii_runs(const u8 *p, size_t len, const char *tag)
{
	char run[160];
	size_t i, n = 0;

	for (i = 0; i <= len; i++) {
		int c = (i < len) ? p[i] : 0;

		if (i < len && c >= 0x20 && c < 0x7f && n < sizeof(run) - 1) {
			run[n++] = c;
			continue;
		}
		if (n >= 4) {
			run[n] = 0;
			pr_err(PFX "%s[%04zx] %s\n", tag, i - n, run);
		}
		n = 0;
	}
}

static void probe_dump_buf(const u8 *buf, size_t len, const char *tag,
			   const char *why)
{
	pr_err(PFX "==== %s: %zu bytes [%s] ====\n", tag, len, why);
	probe_ascii_runs(buf, len, tag);
	print_hex_dump(KERN_ERR, PFX, DUMP_PREFIX_OFFSET, 16, 1, buf, len,
		       true);
	pr_err(PFX "==== end %s ====\n", tag);
}

static void probe_smem_dump(const char *why)
{
	size_t len = 0;
	void *msg;

	msg = qcom_smem_get(PROBE_SMEM_HOST_ANY, probe_smem_item, &len);
	if (IS_ERR(msg)) {
		pr_err(PFX "smem item %u unavailable (%ld) [%s]\n",
		       probe_smem_item, PTR_ERR(msg), why);
		return;
	}
	if (!len) {
		pr_err(PFX "smem item %u is empty [%s]\n", probe_smem_item, why);
		return;
	}
	if (probe_smem_len && len > probe_smem_len)
		len = probe_smem_len;

	probe_dump_buf(msg, len, "q6crash", why);
}

static int probe_smem_now_set(const char *val, const struct kernel_param *kp)
{
	probe_smem_dump("on demand");
	return 0;
}

static const struct kernel_param_ops probe_smem_now_ops = {
	.set = probe_smem_now_set,
};
module_param_cb(smem_now, &probe_smem_now_ops, NULL, 0200);
MODULE_PARM_DESC(smem_now, "write anything to dump the crash record now");

/*
 * probe_phys_set()
 *	Dump an arbitrary physical range, for the Q6 M3 and ETR dump regions
 *	named in the device tree.  busybox has no devmem and these ranges are
 *	reserved rather than mapped, so ioremap is the only way to reach them.
 */
static int probe_phys_set(const char *val, const struct kernel_param *kp)
{
	unsigned long long pa = 0;
	unsigned int len = 0;
	void __iomem *io;
	void *mem;
	char *sep, kbuf[64];
	u8 *tmp;

	strscpy(kbuf, val, sizeof(kbuf));
	sep = strim(kbuf);
	if (sscanf(sep, "%llx %u", &pa, &len) < 1)
		return -EINVAL;
	if (!len)
		len = 256;
	if (len > 4096)
		len = 4096;

	tmp = kmalloc(len, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	/*
	 * What this is usually pointed at now is ring and descriptor memory,
	 * which is ordinary kernel RAM from dma_alloc_coherent().  ioremap()
	 * refuses System RAM on arm64, so every attempt to read a REO ring
	 * through here came back as "write error: Out of memory" with
	 * probe_phys_set in the trace.  memremap() is the accessor for RAM;
	 * ioremap stays as the fallback for the genuinely reserved regions
	 * (the Q6 M3 and ETR dumps) this was originally written for.
	 */
	mem = memremap(pa, len, MEMREMAP_WB);
	if (mem) {
		memcpy(tmp, mem, len);
		memunmap(mem);
	} else {
		io = ioremap(pa, len);
		if (!io) {
			pr_err(PFX "neither memremap nor ioremap %llx+%u\n",
			       pa, len);
			kfree(tmp);
			return -ENOMEM;
		}
		memcpy_fromio(tmp, io, len);
		iounmap(io);
	}

	probe_dump_buf(tmp, len, "phys", sep);
	kfree(tmp);
	return 0;
}

static const struct kernel_param_ops probe_phys_ops = {
	.set = probe_phys_set,
};
module_param_cb(phys_dump, &probe_phys_ops, NULL, 0200);

/*
 * Receive counters, readable at any time.
 *
 * They were only printed at the end of an arming run - when they are still
 * zero - so EAPOL delivery has never actually been measured.  It matters: the
 * 4-way handshake fails five times before it works on the external interface,
 * each failure creates and deletes a peer, and the freed peer-table entry is
 * what the RX reap later dereferences.  Reading this after a run says whether
 * EAPOL frames were excepted at all.
 */
static int probe_rxstats_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf,
		  "handed=%d eapol=%d nodev=%d off=%d short=%d\n"
		  "tx_ok=%d tx_fail=%d tx_eapol=%d vdev_rx=%d vdev_rx_ext=%d\n"
		  "txf: nodev=%d short=%d nomem=%d nss=%d\n",
		  atomic_read(&probe_rx_handed),
		  atomic_read(&probe_rx_eapol),
		  atomic_read(&probe_rx_drop_nodev),
		  atomic_read(&probe_rx_drop_off),
		  atomic_read(&probe_rx_drop_short),
		  atomic_read(&probe_vdev_tx_ok),
		  atomic_read(&probe_vdev_tx_fail),
		  atomic_read(&probe_tx_eapol),
		  atomic_read(&probe_vdev_rx),
		  atomic_read(&probe_vdev_rx_ext),
		  atomic_read(&probe_txf_nodev),
		  atomic_read(&probe_txf_short),
		  atomic_read(&probe_txf_nomem),
		  atomic_read(&probe_txf_nss));
}

static const struct kernel_param_ops probe_rxstats_ops = {
	.get = probe_rxstats_get,
};
module_param_cb(rxstats, &probe_rxstats_ops, NULL, 0444);

MODULE_PARM_DESC(phys_dump, "write '<hex-phys> [len]' to dump a physical range");

/*
 * probe_smem_thread()
 *	Watch the crash record so the capture does not depend on the box
 *	surviving long enough to be logged into afterwards.
 *
 * A user PD restart can overwrite the record, and with coredump=inline the
 * crash is recoverable, so polling is what guarantees the first version of it
 * is the one that reaches the log.
 */
static struct task_struct *probe_smem_task;

static int probe_smem_thread(void *unused)
{
	bool seen = false;
	u32 last = 0;

	while (!kthread_should_stop()) {
		size_t len = 0;
		void *msg = qcom_smem_get(PROBE_SMEM_HOST_ANY,
					  probe_smem_item, &len);

		if (!IS_ERR(msg) && len) {
			size_t n = min_t(size_t, len, probe_smem_len);
			u32 sum = crc32(0, msg, n);

			if (sum != last) {
				last = sum;
				probe_smem_dump(seen ? "CHANGED" : "baseline");
				seen = true;
			}
		}

		msleep(probe_smem_watch);
	}

	return 0;
}

/*
 * probe_dp_ready()
 *	ath11k has built this SoC's data path and nothing can reach it yet.
 *
 * Runs INIT, PDEV_INIT and START.  Called from ath11k's QMI worker with
 * ab->core_lock held, which is why probe_run() may sleep here but must not
 * call back into anything of ath11k's that takes that lock - it does not.
 */
/*
 * probe_vif_event()
 *	An ath11k vdev appeared or is about to go away.
 *
 * Only the AP vdev is handed over.  A monitor vdev has no NSS equivalent, and
 * ath11k creates one alongside the AP whenever monitoring is configured.
 *
 * Called with ar->conf_mutex held, so nothing here may ask ath11k for the vif
 * list: everything needed is in the struct, including the netdev that the
 * except path and ECM both key off.
 */
static ssize_t probe_write(struct file *f, const char __user *buf,
			   size_t len, loff_t *off)
{
	unsigned int soc = 0, stage = 1, swdesc = 0, q = 0, mp = 1;
	char kbuf[64] = {};
	int ret;

	if (!len || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;

	if (sscanf(strim(kbuf), "%u %u %u %u %u",
		   &soc, &stage, &swdesc, &q, &mp) < 1) {
		pr_err(PFX "expected: <soc> [stage] [swdesc] [quiesce] [memprofile]\n");
		return -EINVAL;
	}

	mutex_lock(&probe_lock);
	ret = probe_run(soc, stage, swdesc, q != 0, (u8)mp);
	mutex_unlock(&probe_lock);

	return ret ? ret : len;
}

static const struct file_operations probe_fops = {
	.owner = THIS_MODULE,
	.write = probe_write,
};

static int __init probe_init(void)
{

	init_completion(&probe_done);

	if (!register_netdevice_notifier(&probe_netdev_nb))
		probe_netdev_nb_on = true;
	else
		pr_warn(PFX "netdev notifier registration failed\n");

	/* the register calls want a netdev; a dummy is enough here. */
	probe_ndev = alloc_netdev_dummy(0);
	if (!probe_ndev)
		return -ENOMEM;

	probe_dir = debugfs_create_dir("nss_wifili_probe", NULL);
	debugfs_create_file("run", 0200, probe_dir, NULL, &probe_fops);

	pr_info(PFX "loaded - write '<soc> <stage> [swdesc] [quiesce] [memprofile]' to %s\n",
		"/sys/kernel/debug/nss_wifili_probe/run");

	if (probe_smem_watch) {
		probe_smem_task = kthread_run(probe_smem_thread, NULL,
					      "nss_probe_smem");
		if (IS_ERR(probe_smem_task)) {
			probe_smem_task = NULL;
			pr_err(PFX "could not start the crash-record watcher\n");
		} else {
			pr_info(PFX "watching SMEM item %u every %u ms\n",
				probe_smem_item, probe_smem_watch);
		}
	}

	return 0;
}

static void __exit probe_exit(void)
{

	if (probe_netdev_nb_on) {
		unregister_netdevice_notifier(&probe_netdev_nb);
		probe_netdev_nb_on = false;
	}
	if (probe_smem_task)
		kthread_stop(probe_smem_task);
	debugfs_remove_recursive(probe_dir);
	probe_vdev_free();
	{
		u32 pidx;

		for (pidx = 0; pidx < PROBE_MAX_PEERS; pidx++)
			probe_peer_mem_free_one(pidx);
	}
	probe_radio_if_free();
	if (probe_ctx)
		nss_unregister_wifili_if(probe_wifili_if);
	probe_free_tx_pages();
	if (probe_ndev)
		free_netdev(probe_ndev);
	pr_info(PFX "unloaded\n");
}

module_init(probe_init);
module_exit(probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Probe the NSS wifili init sequence against ath11k rings");
