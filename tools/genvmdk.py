#!/usr/bin/env python3
"""Write a VMware monolithicFlat VMDK (text descriptor + zeroed extent).

The ATA driver talks to the legacy IDE primary at 0x1F0, so the disk has
to sit on the IDE bus in the VMX (ide0:0), not SCSI. A monolithicFlat
image is just a small descriptor pointing at a raw extent — no VMware
tools required to produce one.

    python3 tools/genvmdk.py                  # vmware/tazos-disk.vmdk, 8 MB
    python3 tools/genvmdk.py path.vmdk 16     # 16 MB at an explicit path
"""
from __future__ import print_function

import os
import sys

DEFAULT_PATH = os.path.join("vmware", "tazos-disk.vmdk")
DEFAULT_MB = 8


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PATH
    mb = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_MB

    if mb < 8:
        sys.exit("need at least 8 MB (the filesystem occupies ~4 MB of slots)")

    sectors = mb * 1024 * 1024 // 512
    # 16 heads, 32 sectors/track keeps cylinders an integer for 8 MB (32)
    # and 16 MB (64). VMware uses this CHS only as a hint.
    heads, spt = 16, 32
    cyl = sectors // (heads * spt)
    if cyl * heads * spt != sectors:
        sys.exit("sector count %d is not divisible by %d*%d CHS" %
                 (sectors, heads, spt))

    directory = os.path.dirname(os.path.abspath(path)) or "."
    os.makedirs(directory, exist_ok=True)

    base = os.path.basename(path)
    if base.endswith(".vmdk"):
        extent_name = base[:-5] + "-flat.vmdk"
    else:
        extent_name = base + "-flat.vmdk"
    extent_path = os.path.join(directory, extent_name)

    descriptor = """# Disk Descriptor File
version=1
CID=abcdef01
parentCID=ffffffff
createType="monolithicFlat"

# Extent description
RW {sectors} FLAT "{extent}" 0

# The disk is IDE in the VMX; this records that so VMware does not
# silently reattach it to SCSI.
ddb.adapterType = "ide"
ddb.geometry.cylinders = "{cyl}"
ddb.geometry.heads = "{heads}"
ddb.geometry.sectors = "{spt}"
ddb.virtualHWVersion = "19"
""".format(sectors=sectors, extent=extent_name, cyl=cyl, heads=heads, spt=spt)

    with open(path, "w", newline="\n") as f:
        f.write(descriptor)

    with open(extent_path, "wb") as f:
        f.truncate(mb * 1024 * 1024)

    print("wrote %s (%d MB) and %s" % (path, mb, extent_path))


if __name__ == "__main__":
    main()
