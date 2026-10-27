#!/bin/bash
# mount.sh <workdir> up|down  - attach/detach the raw image's FAT32 partition with udisksctl (no root);
# prints the mount point on 'up'. Always 'down' before booting QEMU.
W=$(realpath "$1")
if [ "$2" = "up" ]; then
  L=$(udisksctl loop-setup -f "$W/w98.raw" | grep -oE "/dev/loop[0-9]+"); echo "$L" > "$W/.loop"
  udisksctl mount -b ${L}p1 >/dev/null 2>&1; lsblk -no MOUNTPOINT ${L}p1
else
  L=$(cat "$W/.loop"); sync; udisksctl unmount -b ${L}p1 >/dev/null 2>&1; udisksctl loop-delete -b $L && rm -f "$W/.loop" && echo detached
fi
