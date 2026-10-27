#!/bin/bash
# mkharness.sh <workdir> [vdi]  - create a scratch Win98 SE raw image + launcher from the VirtualBox disk.
# The work dir gets: w98.raw (3.6 GB), qemu.sh (launcher: KVM, Pentium III, QEMU std VGA, debugcon -> e9.txt,
# QMP on 127.0.0.1:4448, headless, -no-reboot so a Windows restart/shutdown ends the QEMU process).
set -e
W=$(realpath "$1"); VDI=${2:-"/home/guilherme/VirtualBox VMs/Windows 98/Windows 98_1.vdi"}
mkdir -p "$W"
[ -f "$W/w98.raw" ] || qemu-img convert -O raw "$VDI" "$W/w98.raw"
cat > "$W/qemu.sh" <<EOT
#!/bin/bash
exec qemu-system-i386 -enable-kvm -cpu pentium3 -m 256 -drive file=$W/w98.raw,format=raw,if=ide,index=0 -vga std -debugcon file:$W/e9.txt -qmp tcp:127.0.0.1:4448,server,nowait -display none -monitor none -rtc base=localtime -no-reboot 2>$W/qemu.err
EOT
chmod +x "$W/qemu.sh"
echo "harness in $W: $(ls "$W" | tr '\n' ' ')"
