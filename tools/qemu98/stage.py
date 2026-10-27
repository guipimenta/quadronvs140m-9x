#!/usr/bin/env python3
"""stage.py <mountpoint> <repo> [--bind] [--native0] [--bat FILE]
Prepare the mounted Win98 image for a driver test boot:
  copies dist/driver into C:\\NVTEST (and the exes alone into C:\\NVTEST2, i.e. no mesa3d.dll beside them),
  installs nvmini.drv/.vxd + vmdisp9x/vmhal DLLs + mesa3d.dll into C:\\WINDOWS\\SYSTEM,
  --bind    writes C:\\NVTEST\\BIND.REG: binds QEMU's std VGA devnode (PCI 1234:1111) to a new Display\\0001
            class key that loads nvmini.vxd/nvmini.drv (import it from a StartUp batch, then restart),
  --native0 writes C:\\NVTEST\\NATIVE.REG: HKLM\\Software\\vmdisp9x\\vesa Native=0 so nvmini.vxd takes the VBE
            path on a machine without the G86 (otherwise it hands the .drv no FBHDA and Windows uses vga.drv),
  --bat     installs FILE as the StartUp batch (C:\\WINDOWS\\Start Menu\\Programs\\StartUp\\NVTEST.BAT, CRLF).
Win98 COMMAND.COM does not wait for GUI programs: use `start /w prog` in the batch."""
import sys, os, shutil, glob
M, R = sys.argv[1], sys.argv[2]; opts = sys.argv[3:]
D = os.path.join(R, "dist", "driver")
def key(path, vals): return "[%s]\r\n" % path + "".join('"%s"=%s\r\n' % (k, v) for k, v in vals) + "\r\n"
nt, nt2, sysd = (os.path.join(M, d) for d in ("NVTEST", "NVTEST2", "WINDOWS/SYSTEM"))
os.makedirs(nt, exist_ok=True); os.makedirs(nt2, exist_ok=True)
for f in glob.glob(os.path.join(D, "*")):
    if os.path.isfile(f): shutil.copy(f, nt)
    if f.endswith((".exe", ".bat")): shutil.copy(f, nt2)
for f in ("nvmini.drv", "nvmini.vxd", "vmdisp9x.dll", "vmhal9x.dll", "vmhal3d.dll", "mesa3d.dll"):
    shutil.copy(os.path.join(D, f), os.path.join(sysd, f.upper() if f == "mesa3d.dll" else f))
if "--bind" in opts:
    dn = "HKEY_LOCAL_MACHINE\\Enum\\PCI\\VEN_1234&DEV_1111&SUBSYS_11001AF4&REV_02\\BUS_00&DEV_02&FUNC_00"
    reg = "REGEDIT4\r\n\r\n"
    reg += "[-HKEY_LOCAL_MACHINE\\Enum\\PCI\\VEN_1234&DEV_1111&SUBSYS_11001AF4&REV_02\\000200]\r\n\r\n[-HKEY_LOCAL_MACHINE\\Enum\\PCI\\VEN_1234&DEV_1111&SUBSYS_11001AF4&REV_02\\001000]\r\n\r\n"
    reg += key(dn, [("Class", '"Display"'), ("ClassGUID", '"{4d36e968-e325-11ce-bfc1-08002be10318}"'), ("Driver", '"Display\\\\0001"'), ("Mfg", '"(Standard display types)"'), ("DeviceDesc", '"QEMU VGA (nvmini test)"'), ("ConfigFlags", 'hex:00,00,00,00')])
    reg += key("HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\Display\\0001", [("Ver", '"4.0"'), ("DevLoader", '"*vdd"'), ("InfPath", '"MSDISP.INF"'), ("InfSection", '"VGA"'), ("ProviderName", '"Microsoft"'), ("DriverDesc", '"QEMU VGA (nvmini test)"'), ("MatchingDeviceId", '"PCI\\\\CC_0300"')])
    reg += key("HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\Display\\0001\\DEFAULT", [("vdd", '"*vdd"'), ("minivdd", '"nvmini.vxd"'), ("drv", '"nvmini.drv"'), ("Mode", '"16,1024,768"'), ("DDC", '"0"'), ("RefreshRate", '"-1"')])
    for bpp in (8, 16, 32):
        for res in ("640,480", "800,600", "1024,768"): reg += "[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\Display\\0001\\MODES\\%d\\%s]\r\n\r\n" % (bpp, res)
    reg += key("HKEY_LOCAL_MACHINE\\Config\\0001\\Display\\Settings", [("Resolution", '"1024,768"'), ("BitsPerPixel", '"16"')])
    reg += key("HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\OpenGLdrivers", [("SOFTWARE", '"mesa3d.dll"')])
    open(os.path.join(nt, "BIND.REG"), "wb").write(reg.encode()); print("BIND.REG written")
if "--native0" in opts:
    reg = "REGEDIT4\r\n\r\n" + key("HKEY_LOCAL_MACHINE\\Software\\vmdisp9x\\vesa", [("Native", "dword:00000000"), ("InitStop", "dword:00000000"), ("BootPattern", "dword:00000000")])
    open(os.path.join(nt, "NATIVE.REG"), "wb").write(reg.encode()); print("NATIVE.REG written")
if "--bat" in opts:
    src = opts[opts.index("--bat") + 1]
    txt = open(src).read().replace("\r\n", "\n").replace("\n", "\r\n")
    su = os.path.join(M, "WINDOWS", "Start Menu", "Programs", "StartUp")
    open(os.path.join(su, "NVTEST.BAT"), "wb").write(txt.encode()); print("StartUp NVTEST.BAT installed from", src)
print("staged:", sorted(os.listdir(nt))[:12], "...")
