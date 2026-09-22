#!/bin/bash
# Turn a clean OpenWrt v25.12.5 checkout into a buildable pz-l8 NSS tree.
#
# Usage:  scripts/setup.sh /path/to/openwrt
#
# Everything this project changes falls into five buckets:
#   1. edits to files OpenWrt already ships   -> openwrt/0001-cmcc-pz-l8-nss.patch
#   2. files OpenWrt does not have            -> openwrt/tree/, copied in place
#   3. our own packages                       -> feed/, wired in as a feed
#   4. patches to packages in other feeds     -> openwrt/feeds/, copied once the
#                                                feeds exist
#   5. rootfs overlay + config seed           -> files/, config/
#
# Idempotent: re-running on an already-prepared tree is a no-op for 1 and safe
# for the rest, including .config - an existing one is kept, so a menuconfig
# selection survives.  RESEED=1 goes back to the seed.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OW=${1:?usage: $0 /path/to/openwrt}
OW=$(cd "$OW" && pwd)

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

[ -f "$OW/rules.mk" ] || { echo "$OW does not look like an OpenWrt tree"; exit 1; }

# ---------------------------------------------------------------- 1. base version
if [ -d "$OW/.git" ]; then
    have=$(git -C "$OW" describe --tags --exact-match 2>/dev/null || echo "unknown")
    say "OpenWrt base: $have"
    if [ "$have" != "v25.12.5" ]; then
        echo "WARNING: this project is developed against v25.12.5."
        echo "         Patch 1 is a plain diff and will likely fail elsewhere."
    fi
fi

# ---------------------------------------------------------------- 2. tracked edits
say "Applying edits to upstream files"
PATCH="$HERE/openwrt/0001-cmcc-pz-l8-nss.patch"
if git -C "$OW" apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "already applied, skipping"
else
    git -C "$OW" apply --verbose "$PATCH"
fi

# ---------------------------------------------------------------- 3. new files
say "Copying files that do not exist upstream"
( cd "$HERE/openwrt/tree" && find . -type f -print0 |
  while IFS= read -r -d '' f; do
      install -Dm644 "$f" "$OW/${f#./}"
      echo "  ${f#./}"
  done )

# ---------------------------------------------------------------- 4. feed
say "Registering the pzl8 package feed"
FEEDS="$OW/feeds.conf"
[ -f "$FEEDS" ] || cp "$OW/feeds.conf.default" "$FEEDS"
grep -q '^src-link pzl8 ' "$FEEDS" ||
    echo "src-link pzl8 $HERE/feed" >> "$FEEDS"
grep '^src-link pzl8 ' "$FEEDS"

( cd "$OW" && ./scripts/feeds update -a >/dev/null && ./scripts/feeds install -a >/dev/null )
echo "feeds updated and installed"

# ---------------------------------------------------------------- 5. feed package patches
# These cannot live in openwrt/tree/: that is copied before the feeds exist, and
# scripts/feeds deletes a feed directory that is not yet a checkout before it
# clones into it.  After an update they are simply copied again.
say "Adding patches to packages from other feeds"
( cd "$HERE/openwrt/feeds" && find . -type f -print0 |
  while IFS= read -r -d '' f; do
      install -Dm644 "$f" "$OW/feeds/${f#./}"
      echo "  feeds/${f#./}"
  done )

# ---------------------------------------------------------------- 6. rootfs overlay
say "Installing the rootfs overlay"
mkdir -p "$OW/files"
cp -a "$HERE/files/." "$OW/files/"
find "$OW/files" -type f | sed "s|$OW/|  |"
# init scripts must stay executable through the image build
chmod +x "$OW"/files/etc/init.d/* 2>/dev/null || true
# rpcd runs these as programs; a 644 plugin never registers its ubus object
chmod +x "$OW"/files/usr/libexec/rpcd/* 2>/dev/null || true

# ---------------------------------------------------------------- 7. config
# An existing .config is someone's menuconfig session and is left alone.
# RESEED=1 throws it away and starts from the seed again.
if [ -f "$OW/.config" ] && [ "${RESEED:-0}" != 1 ]; then
    say "Keeping the existing .config  (RESEED=1 to replace it with the seed)"
else
    say "Seeding .config"
    cp "$HERE/config/cmcc_pz-l8.config" "$OW/.config"
fi

# Always run this: it fills in defaults for anything new since the config was
# written, which is the point of re-running setup.sh at all.
( cd "$OW" && make defconfig >/dev/null )
grep -E '^CONFIG_TARGET_(BOARD|SUBTARGET|PROFILE)' "$OW/.config" || true

# These are not packages, so nothing reminds you about them in menuconfig, and
# make defconfig will not put them back if they get turned off.  The workflow
# checks the same list.
missing=0
for sym in \
    CONFIG_NSS_DRV_WIFIOFFLOAD_ENABLE \
    CONFIG_NSS_FIRMWARE_VERSION_12_5 \
    CONFIG_PACKAGE_kmod-qca-nss-drv \
    CONFIG_PACKAGE_nss-firmware-ipq50xx
do
    grep -q "^${sym}=y" "$OW/.config" || { echo "  MISSING: $sym"; missing=1; }
done
if [ "$missing" != 0 ]; then
    echo
    echo "WARNING: the above are required by the NSS offload in this tree."
    echo "         Without WIFIOFFLOAD, qca-nss-drv is built with no wifili and"
    echo "         ath11k.ko fails to link.  Without the 12.5 firmware choice the"
    echo "         build succeeds and the wifili peer-stats ABI is silently wrong."
    echo "         Re-enable them in menuconfig, or run RESEED=1 $0 $OW"
fi

say "Ready.  Build with:  cd $OW && make -j\$(nproc)"
echo "        Pick packages with:  cd $OW && make menuconfig"
echo "        Keep your selection: cd $OW && ./scripts/diffconfig.sh > $HERE/config/cmcc_pz-l8.config"
