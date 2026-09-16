#!/usr/bin/env python3
"""Build a U-Boot recovery image for the PZ-L8, in the format its bootloader
actually wants.

Reverse-engineered from mtd11-APPSBL.bin (the board's own U-Boot):

  * the HTTP form field is "firmware"; anything over 0x2000000 is refused,
    and one upgrade type additionally demands exactly 0x100000 bytes;
  * the uploaded file is copied to RAM at 0x44000000 and $fileaddr is set to it;
  * then it runs, literally:
        imgaddr=$fileaddr && source $imgaddr:script

So the payload is not a ubinized image at all - it is a FIT with a `script`
subimage, and that script does the flashing itself.  Uploading a raw .ubi gets
"Upload file size: N bytes" and a flashing.html page, and then nothing is
written, because `source` finds no script.  That is exactly what three
attempts looked like.

Partition geometry comes from the board's own MIBIB (mtd01 in the backup):

        rootfs   offset 0x900000   length 0x6E00000 (110 MiB)

and there is no rootfs_1 - this board has no A/B pair.

The script erases the whole rootfs partition before writing, which is what
ubiformat does and what stops stale PEBs from the previous UBI being attached
alongside the new ones.
"""
import glob
import os
import shutil
import subprocess
import sys

# mkimage needs dtc on its PATH.  Both come out of an OpenWrt build, but a
# distro's u-boot-tools and device-tree-compiler work just as well, which is
# what CI uses.  Order: explicit environment, then a tree under OW, then PATH.
#
# The tree paths are globbed rather than spelled out.  They carry the kernel
# version, and writing it down means this breaks on the next bump - quietly,
# because the fallback would just be "not found".
OW = os.environ.get("OW", ".")


def _first_existing(patterns):
    for pattern in patterns:
        for match in sorted(glob.glob(pattern)):
            if os.path.exists(match):
                return match
    return None


def _dirof(path):
    if path is None:
        return None
    return path if os.path.isdir(path) else os.path.dirname(path)


MKIMAGE = (os.environ.get("MKIMAGE")
           or _first_existing([OW + "/staging_dir/host/bin/mkimage"])
           or shutil.which("mkimage"))

DTCDIR = (os.environ.get("DTCDIR")
          or _dirof(_first_existing([
              OW + "/build_dir/target-*/linux-*/linux-*/scripts/dtc/dtc",
              OW + "/build_dir/target-*/linux-*/linux-*/scripts/dtc",
              OW + "/staging_dir/host/bin/dtc",
          ]))
          or _dirof(shutil.which("dtc")))

if not MKIMAGE:
    sys.exit("mkimage not found.  Set MKIMAGE, or OW to an OpenWrt tree, "
             "or install u-boot-tools.")
if not DTCDIR:
    sys.exit("dtc not found.  Set DTCDIR, or OW to an OpenWrt tree, "
             "or install device-tree-compiler.")

WORK = "/tmp/pzl8-recovery"
if len(sys.argv) != 3:
    sys.exit("usage: mkrecovery.py <squashfs-factory.ubi> <out.fit>")
UBI, OUT = sys.argv[1], sys.argv[2]

ROOTFS_OFF = 0x900000
ROOTFS_LEN = 0x6E00000
LOAD = 0x48000000          # well clear of the 0x44000000 upload buffer

os.makedirs(WORK, exist_ok=True)
size = os.path.getsize(UBI)
if size % 0x20000:
    sys.exit("image is not erase-block aligned: %d" % size)
if size > ROOTFS_LEN:
    sys.exit("image does not fit in the rootfs partition")

script = """echo == PZ-L8 recovery ==
nand device 0
echo erasing rootfs 0x%x + 0x%x
nand erase 0x%x 0x%x
echo extracting image to 0x%x
imxtract $imgaddr rootfs 0x%x
echo writing 0x%x bytes
nand write 0x%x 0x%x 0x%x
echo done, resetting
reset
""" % (ROOTFS_OFF, ROOTFS_LEN, ROOTFS_OFF, ROOTFS_LEN,
       LOAD, LOAD, size, LOAD, ROOTFS_OFF, size)

open(os.path.join(WORK, "flash.scr"), "w", newline="\n").write(script)

its = """/dts-v1/;

/ {
	description = "CMCC PZ-L8 U-Boot recovery";
	#address-cells = <1>;

	images {
		rootfs {
			description = "ubinized rootfs";
			data = /incbin/("%s");
			type = "firmware";
			arch = "arm64";
			os = "linux";
			compression = "none";
		};

		script {
			description = "flash script";
			data = /incbin/("%s");
			type = "script";
			arch = "arm64";
			os = "linux";
			compression = "none";
		};
	};
};
""" % (UBI, os.path.join(WORK, "flash.scr"))

itsp = os.path.join(WORK, "recovery.its")
open(itsp, "w", newline="\n").write(its)

print("mkimage: %s" % MKIMAGE)
print("dtc dir: %s" % DTCDIR)
env = dict(os.environ, PATH=DTCDIR + ":" + os.environ.get("PATH", ""))
r = subprocess.run([MKIMAGE, "-f", itsp, OUT], capture_output=True, text=True,
                   env=env)
sys.stdout.write(r.stdout)
sys.stderr.write(r.stderr)
if r.returncode:
    sys.exit(r.returncode)

print("--- script ---")
print(script)
print("output: %s  %d bytes" % (OUT, os.path.getsize(OUT)))
