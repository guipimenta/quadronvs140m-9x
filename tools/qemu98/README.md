# Windows 98 SE test harness (QEMU, headless)

Reproduces the software side of the driver package without the T61: nvmini.drv/.vxd on
QEMU's standard VGA (VBE path, `Native=0`), the Mesa ICD, wgltest/pftest/icdload/pfprobe.
Everything that does not need the G86 can be iterated here in ~3 minutes per boot.

Source disk: the VirtualBox VM "Windows 98" (`Windows 98_1.vdi`, Win98 SE + vmdisp9x
installed). Scripts (no root needed; udisksctl attaches the image, QMP drives the VM):

    W=/tmp/w98work
    tools/qemu98/mkharness.sh $W            # VDI -> $W/w98.raw + $W/qemu.sh (KVM, P3, std VGA, -no-reboot)
    M=$(tools/qemu98/mount.sh $W up)        # mount the FAT32 partition
    python3 tools/qemu98/stage.py $M . --bind --native0 --bat mytests.bat
    tools/qemu98/mount.sh $W down
    tools/qemu98/run_boot.sh $W boot1 480   # boot; Esc every 6 s from t=70 s; screenshots boot1_NNN.png
    tools/qemu98/qmp.py keys ctrl-esc ; tools/qemu98/qmp.py type "..." ; tools/qemu98/qmp.py shot x.ppm

First boot of a fresh image: Windows finds QEMU's VGA (PCI 1234:1111) as new hardware
(Esc dismisses the wizard), the StartUp batch imports BIND.REG + NATIVE.REG and restarts
(`C:\WINDOWS\RUNDLL32.EXE user,exitwindowsexec`; with `-no-reboot` the restart ends QEMU,
run `run_boot.sh` again). Second boot: nvmini.drv loads on the VBE path, 1024x768x16.
The batch should stage itself with marker files (`if exist STAGE1.OK goto stage2`), run
console tools with `> file`, GUI tools with `start /w`, copy `C:\nvshim.log` after each
Mesa test, and end with `RUNDLL32 user,exitwindows` (shutdown ends QEMU). Read results
by mounting the image again. `e9.txt` holds the VxD/DRV debug console (port E9).
The image's old tray tool (Tray3d, from the VBox install) crashes on plain VGA; Enter
closes it. `pgrep -f "^qemu-system-i386 "` tells whether the VM is still up.
