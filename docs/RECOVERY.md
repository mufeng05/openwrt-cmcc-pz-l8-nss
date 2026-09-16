# Recovering a PZ-L8 that will not boot

The board has two ways back and they want different things. Getting that wrong
turned one bad boot into an afternoon, so this is what its own bootloader
actually does, read out of `mtd11-APPSBL.bin`.

## sysupgrade, from a system that still runs

`sysupgrade -n <image>-squashfs-sysupgrade.bin`. That file is already a tar with
a `sysupgrade-cmcc_pz-l8/` directory member, which is what `nand_upgrade_tar`
needs; a `.ubi` goes down the `nand_upgrade_ubinized` path instead, which
streams into `ubiformat ... -f -` and fails silently. Convert one with
`mktar.py` if that is all you have.

**Do not run `sysupgrade -T` on the image first.** It calls `fwtool -i`, which
extracts the metadata *and truncates it off the file*. The next `sysupgrade`
on the same file then stops with "Image metadata not present" - the check
destroyed what it was checking. Flash a freshly downloaded copy.

`platform.sh` is patched for this board: stock calls `elecom_upgrade_prepare()`,
which looks for an A/B rootfs pair. There is none - the MIBIB has one `rootfs`
partition - so `smempart_next_root()` fails and the flash aborts. The patch
sets `CI_UBIPART="rootfs"` instead.

## Coming back from a vendor image

A vendor-line image will not take the `sysupgrade.bin`, and forcing it does
something other than fail. v1.6's `platform_do_upgrade` never looks at the tar:

```sh
image_is_nand || return && do_flash_ubi $1 "rootfs"
```

and `do_flash_ubi` is `ubidetach` followed by `ubiformat /dev/<rootfs mtd> -y -f
<file>`. So it wants a **ubinized image** - the `factory.ubi`, which is the
opposite of what this board's own sysupgrade wants. Feed it the tar and
ubiformat writes a tar onto the partition.

```sh
# on the vendor image (board name reads ap-mp02.1, not cmcc,pz-l8)
curl -s -o /tmp/f.ubi http://<server>/...-squashfs-factory.ubi   # no wget there
md5sum /tmp/f.ubi
sysupgrade -n -F /tmp/f.ubi
```

`-F` is required: `platform_check_image` calls `image_is_FIT`, and a `.ubi` is
not a FIT, so the check fails before the forced path is reached.

Three things that cost a run here:

- **Do not pipe it through `tail`.** `sysupgrade ... | tail -25` buffers until
  EOF, so a session killed by a client-side timeout shows nothing at all and
  looks like a silent no-op. Let the output stream.
- Give it four minutes. `ubiformat` scans and formats all 880 eraseblocks of the
  110 MiB partition before it writes, and the SSH connection dies partway
  through by design.
- Check `/proc/boot_info` first. `do_flash_ubi` has an A/B branch that redirects
  the write to `upgradepartition`; this board has no `/proc/boot_info` at all,
  so the branch is skipped and the write lands on `rootfs`. If a future board
  has one, the target is not the partition you named.

Build the U-Boot fallback *before* flashing, not after - `scripts/mkrecovery.py`
needs `mkimage` and `dtc`, which are not on a stock build host:

```sh
apt-get install -y u-boot-tools device-tree-compiler
MKIMAGE=/usr/bin/mkimage DTCDIR=/usr/bin python3 scripts/mkrecovery.py \
    <image>-squashfs-factory.ubi recovery.fit
```

## U-Boot web recovery, when nothing runs

Hold the button at power-on; the page is at `http://192.168.10.10`. It takes a
POST with the form field `firmware`.

**It does not take a `.ubi`.** After the upload it runs, literally:

```
imgaddr=$fileaddr && source $imgaddr:script
```

so the payload has to be a **FIT image with a `script` subimage**, and that
script does the flashing itself. Upload a raw ubinized image and the page still
says "Upload file size: N bytes" and serves flashing.html - and nothing is
written, because `source` finds no script. Three uploads looked like successes
and wrote nothing.

`scripts/mkrecovery.py` builds the right thing:

Every release carries one already, as `*-uboot-recovery.fit`; CI builds it
from the same factory image it publishes and fails the build if it grows past
the bootloader's 32 MiB limit. Build one by hand only for an image that was not
released:

```sh
python3 scripts/mkrecovery.py <image>-squashfs-factory.ubi recovery.fit
curl -F "firmware=@recovery.fit" http://192.168.10.10/
```

Then power-cycle without touching the button.

### What the bootloader checks

Read from the U-Boot image in the mtd backup, not guessed:

- the form field must be named `firmware`;
- anything larger than **0x2000000 (32 MiB)** is refused - which rules out the
  35 MB vendor images, whatever else is wrong with them;
- one upgrade type additionally demands exactly 0x100000 bytes;
- the upload lands in RAM at **0x44000000**, and `$fileaddr` is set to it, so a
  script must not extract over that.

`flash <part_name> <load_addr> <file_size>` is the vendor's own command:

| part_name | what it runs |
|---|---|
| `ubi_rootfs` | `ubi remove rootfs_data && ubi remove/create/write && ubi create rootfs_data` |
| any other UBI volume | `ubi write <addr> <name> <size>` |
| an SMEM partition | `nand erase <start> <len> && nand write <addr> <start> <size>` |

### Flash layout

From the board's own MIBIB (`mtd01` in `nwrt-baseline/mtd-backup/`), erase block
0x20000:

```
0:SBL1        0x000000   0:MIBIB       0x080000   0:BOOTCONFIG  0x100000
0:BOOTCONFIG1 0x140000   0:QSEE        0x180000   0:QSEE_1      0x280000
0:DEVCFG      0x380000   0:DEVCFG_1    0x3c0000   0:CDT         0x400000
0:CDT_1       0x440000   0:APPSBLENV   0x480000   0:APPSBL      0x500000
0:APPSBL_1    0x640000   0:ART         0x780000   0:TRAINING    0x880000
rootfs        0x900000   length 0x6e00000 (110 MiB)
```

There is **no `rootfs_1`**. The U-Boot env is
`bootcmd=bootipq, bootdelay=1, new_rootfs0=1, sysfail=0`.

The generated script erases the whole rootfs partition before writing, which is
what `ubiformat` does and what stops stale PEBs from the old UBI being attached
next to the new ones.

## Two things that cost time

**Write, then commit, then reboot.** UBIFS recovers its journal on nearly every
unclean boot and rolls the tail back, so a `sync` alone is not enough: edits to
`/etc` disappeared across several reboots, including the one that disabled the
WiFi handover. Force the commit:

```sh
sync; mount -o remount,ro /overlay && mount -o remount,rw /overlay
```

**nss-offload confirms 25 s after it runs.** Reboot inside that window and
`nss-failsafe` sees an unconfirmed boot and disarms the handover - correctly,
but it looks like a failure that did not happen. `reboot` (not `reboot -f`)
runs the init scripts' `stop()`, which clears the flag.
