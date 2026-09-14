#!/bin/sh
# Arm the QSDK NSS wifili offload on the CMCC PZ-L8, by hand.
#
# NOT the normal path any more.  A stock boot arms both radios on its own -
# see docs/WIFILI.md, "How to run it".  This is for a box that booted with the
# handover disarmed, which is what /etc/nss-wifi-disabled means.  It refuses to
# run if the probe is already loaded, which on a normal boot it is.
#
# This is the sequence the measurements in docs/WIFILI.md were taken with,
# stripped of the diagnostic scaffolding (netconsole, log streaming, the NSS
# watchdog) that the development harness carried.  It is not an init script:
# it has to run after the NSS core and qca-nss-drv are up and it takes the
# radios down and back up, so it is a deliberate step, not something to put in
# /etc/rc.d yet.
#
# Prerequisites, both set before ath11k probes - see files/etc/modules.d/34-ath11k-nss:
#   ath11k nss_refill_hold=3 frame_mode=2
#
# Order matters.  The probe is inserted and both wifili SoCs are initialised
# while the radios are DOWN; `wifi up` then creates the vdevs, and the probe's
# vif hook hands each one to the slot that owns it.  Arming after the radios are
# already up leaves ath11k holding a data path NSS has taken over.
set +e

PROBE=${PROBE:-/lib/modules/$(uname -r)/nss_wifili_probe.ko}
[ -f "$PROBE" ] || PROBE=/root/nss_wifili_probe.ko

if ! [ -f "$PROBE" ]; then
	echo "nss_wifili_probe.ko not found" >&2
	exit 1
fi

if lsmod | grep -q '^nss_wifili_probe'; then
	echo "REFUSING: nss_wifili_probe is already loaded." >&2
	echo "  rmmod-ing it breaks the NSS data path and takes br-lan with it." >&2
	echo "  Reboot first, then run this on a clean boot." >&2
	exit 1
fi

# Both radios.  Bit 0 is the internal IPQ5018 (2.4 GHz, c000000.wifi), bit 1 the
# external QCN6122 (5 GHz, b00a040.wifi).
echo 3 > /sys/module/ath11k/parameters/nss_offload_mask

# A 512-entry firmware log ring; the default is too small for a trap dump to
# survive in it, and a trap with no dump is a trap you cannot read.
echo 512 > /proc/sys/dev/nss/general/logbuf
echo 2 > /proc/sys/dev/nss/general/coredump
sysctl -w kernel.panic_on_oops=0 >/dev/null 2>&1

wifi down
sleep 3

# Resolve each radio's AP netdev from the phy rather than hardcoding a name:
# phy numbering is not stable across boots, and a wrong name silently points a
# slot at the other radio.  This runs between `wifi down` and `wifi up`, so
# neither `iw dev` nor an SSID lookup would find anything - only the phy's
# device link is there.
AP24=
AP5=
for p in /sys/class/ieee80211/phy*; do
	[ -e "$p/device" ] || continue
	case "$(readlink -f "$p/device")" in
		*c000000*) AP24="$(basename "$p")-ap0" ;;
		*b00a040*) AP5="$(basename "$p")-ap0" ;;
	esac
done
echo "2.4 GHz AP netdev: ${AP24:-<none>}"
echo "5 GHz   AP netdev: ${AP5:-<none>}"

insmod "$PROBE" slot=1 init_flags=0 swdesc=0 auth_early=1 \
	vdev_netdev0="$AP24" vdev_netdev1="$AP5" next_hop=158 || exit 1

# "<soc> <stage> <swdesc> <quiesce> <memprofile>".  Stage 3 is INIT +
# PDEV_INIT + START.  swdesc 0 means "use the built-in value"; passing it on the
# insmod line instead is silently ignored, which invalidated two early rounds.
echo "0 3 0 0 1" > /sys/kernel/debug/nss_wifili_probe/run
sleep 2
echo "1 3 0 0 1" > /sys/kernel/debug/nss_wifili_probe/run
sleep 1

logread | grep -a WIFILIPROBE | sed 's/.*WIFILIPROBE: //' \
	| grep -aE 'wifili interface|INIT ->|PDEV_INIT radio|START ->' | tail -8

wifi up
sleep 14

logread | grep -a WIFILIPROBE | sed 's/.*WIFILIPROBE: //' \
	| grep -aE 'slot . netdev|vdev if=|tx hook armed' | tail -6
iw dev | awk '/Interface/{i=$2} /ssid/{print "  " i " " $2}'

echo
echo "armed.  Check with:"
echo "  cat /sys/module/nss_wifili_probe/parameters/rxstats"
echo "  grep -E 'reo_reaped|rx_deliverd |tx_sent_count' /sys/kernel/debug/qca-nss-drv/stats/wifili"
