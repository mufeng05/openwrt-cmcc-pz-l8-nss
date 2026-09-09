#!/bin/bash
# Turn a clean OpenWrt v25.12.5 checkout into a buildable pz-l8 NSS tree.
#
# Usage:  scripts/setup.sh /path/to/openwrt
#
# Everything this project changes falls into four buckets:
#   1. edits to files OpenWrt already ships   -> openwrt/0001-cmcc-pz-l8-nss.patch
#   2. files OpenWrt does not have            -> openwrt/tree/, copied in place
#   3. our own packages                       -> feed/, wired in as a feed
#   4. rootfs overlay + config seed           -> files/, config/
#
# Idempotent: re-running on an already-prepared tree is a no-op for 1 and safe
# for the rest.
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

# ---------------------------------------------------------------- 5. rootfs overlay
say "Installing the rootfs overlay"
mkdir -p "$OW/files"
cp -a "$HERE/files/." "$OW/files/"
find "$OW/files" -type f | sed "s|$OW/|  |"
# init scripts must stay executable through the image build
chmod +x "$OW"/files/etc/init.d/* 2>/dev/null || true

# ---------------------------------------------------------------- 6. config
say "Seeding .config"
cp "$HERE/config/cmcc_pz-l8.config" "$OW/.config"
( cd "$OW" && make defconfig >/dev/null )
grep -E '^CONFIG_TARGET_(BOARD|SUBTARGET|PROFILE)' "$OW/.config" || true

say "Ready.  Build with:  cd $OW && make -j\$(nproc)"
