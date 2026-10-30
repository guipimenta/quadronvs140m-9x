#!/usr/bin/env python3
"""t61.py - drive the ThinkPad T61 (Windows 98 SE) from this desktop.

Wraps the win98-remote controller (protocol 1, the agent w98agent.exe must be
running on the T61) and the helper tools/remote/t61util.exe, uploaded
automatically. Lifts the agent's limits: absolute Windows paths, files over
64 MiB (split + join, CRC32 checked end to end), DOS command lines, screenshots.

  t61.py ping | info | ps | kill NAME
  t61.py dir PATH          t61.py type PATH
  t61.py get REMOTE LOCAL  t61.py put LOCAL REMOTE     (REMOTE: C:\\path or agent-folder name)
  t61.py run [-t SECS] EXE [ARGS...]     (EXE: agent-folder name or C:\\path, runs in its folder)
  t61.py sh  [-t SECS] "DOS LINE" ["LINE"...]  (a batch file; newlines also split lines)
  t61.py start EXE [ARGS...]             (do not wait: GUI programs, games)
  t61.py logs [LOCALDIR]                 (C:\\nvshim.log, nvlast, nvkick, nvtail, nvclk)
  t61.py shot LOCAL.png
  t61.py install FILE...                 (into C:\\WINDOWS\\SYSTEM, old copy kept in C:\\NVBACKUP;
                                          a file in use is replaced at the next reboot via WININIT.INI)
  t61.py reboot [-t SECS] [force]        (Start-menu restart; waits up to SECS, default 600,
                                          for the agent, skips the network logon, waits for the desktop)
  t61.py endtasks                        (a shutdown stuck on End Task prompts: answer them, then as reboot)
  t61.py wait [SECS]                     (wait for the agent to answer)
  t61.py autostart on|off|show           (HKLM RunServices + Run start t61boot.exe supervise at boot)
  t61.py wins [TITLE] | kids TITLE | focus TITLE | close TITLE
  t61.py press TITLE BUTTON              (click a dialog button by its text)
  t61.py waitwin TITLE SECS | click X Y [right|double] | key COMBO... | text STRING
  t61.py debug EXE [ARGS]                (run EXE detached under a debugger: crash module+offset,
                                          registers, stack in C:\\remote\\t61debug.txt)

Environment: W98REMOTE_DIR (default ~/projects/win98-remote) holds
controller/w98ctl.py and controller.json.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import struct
import sys
import tempfile
import zlib

HERE = Path(__file__).resolve().parent
HELPER = HERE / "t61util.exe"
REMOTE_DIR = Path(os.environ.get("W98REMOTE_DIR", Path.home() / "projects/win98-remote"))
CHUNK = 60 * 1024 * 1024          # under the agent's 64 MiB per-file limit
SYSDIR = "C:\\WINDOWS\\SYSTEM"
BACKUP = "C:\\NVBACKUP"
LOGS = ["C:\\nvshim.log", "C:\\nvlast.txt", "C:\\nvkick.txt", "C:\\nvtail.txt", "C:\\nvclk.txt"]


def load_controller():
    spec = importlib.util.spec_from_file_location("w98ctl", REMOTE_DIR / "controller" / "w98ctl.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


w98 = load_controller()


def is_path(name):
    return ":" in name or "\\" in name


class T61:
    def __init__(self):
        import json
        cfg = json.loads((REMOTE_DIR / "controller.json").read_text())
        self.c = w98.Client(cfg["host"], cfg["port"], cfg["token"])
        self.tmp = Path(tempfile.mkdtemp(prefix="t61-"))

    # The helper is stored under a name carrying its CRC32 and uploaded only when
    # that version is missing: re-uploading on every call would fail while an
    # orphan of a timed-out run still holds the file.
    HELPER_NAME = "t61u%08x.exe" % zlib.crc32(HELPER.read_bytes())

    def util(self, *args, timeout=60, echo=True, check=True):
        """Run t61util.exe; return (exit code, its output)."""
        try:
            res = self.c.run(self.HELPER_NAME, list(args), timeout)
        except w98.ProtocolError as exc:
            if str(exc) != "launch":
                raise
            self.c.put(HELPER, self.HELPER_NAME)
            res = self.c.run(self.HELPER_NAME, list(args), timeout)
        log = self.tmp / "lastrun.log"
        self.c.get("lastrun.log", log)
        out = log.read_bytes().decode("latin-1")
        if echo:
            sys.stdout.write(out)
        if check and res["exit_code"] != 0:
            raise RuntimeError(f"t61util {args[0]} failed (exit {res['exit_code']}): {out.strip()[-300:]}")
        return res["exit_code"], out

    def crc(self, path):
        _, out = self.util("crc", path, echo=False)
        size, crc = map(int, out.split()[:2])
        return size, crc

    def get(self, remote, local):
        local = Path(local)
        if not is_path(remote):
            self.c.get(remote, local)
            return
        size, crc = self.crc(remote)
        with open(local, "wb") as out:
            for off in range(0, max(size, 1), CHUNK):
                n = min(CHUNK, size - off)
                self.util("part", remote, str(off), str(n), "t61get.bin", echo=False)
                part = self.tmp / "part.bin"
                self.c.get("t61get.bin", part)
                out.write(part.read_bytes())
        self.util("del", "t61get.bin", echo=False)
        data_crc = zlib.crc32(local.read_bytes())
        if local.stat().st_size != size or data_crc != crc:
            raise RuntimeError(f"{remote}: CRC/size mismatch after download")
        print(f"{remote} -> {local} ({size} bytes, CRC32 ok)")

    def put(self, local, remote):
        local = Path(local)
        data = local.read_bytes()
        if not is_path(remote) and len(data) <= CHUNK:
            self.c.put(local, remote)
            print(f"{local} -> {remote} ({len(data)} bytes, CRC32 ok)")
            return
        parts = []
        for i, off in enumerate(range(0, max(len(data), 1), CHUNK)):
            p = self.tmp / f"t61p{i}.bin"
            p.write_bytes(data[off:off + CHUNK])
            self.c.put(p, p.name)
            parts.append(p.name)
        stage = "t61put.bin"
        if len(parts) == 1:
            stage = parts[0]
        else:
            self.util("join", stage, *parts, echo=False)
        if is_path(remote):
            self.util("copy", stage, remote, echo=False)
        elif stage != remote:
            self.util("copy", stage, remote, echo=False)
        for name in set(parts + [stage]):
            self.util("del", name, echo=False, check=False)
        size, crc = self.crc(remote)
        if size != len(data) or crc != zlib.crc32(data):
            raise RuntimeError(f"{remote}: CRC/size mismatch after upload")
        print(f"{local} -> {remote} ({size} bytes, CRC32 ok)")


def bmp_to_png(bmp, png):
    raw = Path(bmp).read_bytes()
    off, = struct.unpack_from("<I", raw, 10)
    w, h, _, bpp = struct.unpack_from("<iiHH", raw, 18)
    if bpp != 24:
        raise ValueError("expected a 24-bit BMP")
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(abs(h)):
        src = (abs(h) - 1 - y) if h > 0 else y
        line = raw[off + src * stride: off + src * stride + w * 3]
        rgb = bytearray(line)
        rgb[0::3], rgb[2::3] = line[2::3], line[0::3]
        rows.append(b"\0" + bytes(rgb))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    Path(png).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, abs(h), 8, 2, 0, 0, 0))
                          + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))


def wait_back(t, limit, down_first=True):
    """After a reboot: wait for the agent to go away, then to answer again."""
    import time
    t0 = time.time()

    def up():
        try:
            t.c.ping()
            return True
        except (OSError, w98.ProtocolError):
            return False
    if down_first:
        while up():
            if time.time() - t0 > 240:
                print("T61 still up after 240 s: the shutdown is stuck (look at `t61.py wins`)")
                return 1
            time.sleep(3)
        print(f"T61 went down after {time.time() - t0:.0f} s")
    while time.time() - t0 < limit:
        if up():
            print(f"T61 agent back after {time.time() - t0:.0f} s")
            return 0
        time.sleep(10)
    print(f"T61 agent not back after {limit} s (hung at boot? agent not autostarted?)")
    return 1


def logon(t):
    """The agent is back before the logon (RunServices). Skip the Microsoft
    Networking logon with Cancel (empty user and password: no credentials
    are entered) and wait for the desktop."""
    code, out = t.util("waitwin", "Program Manager", "5", echo=False, check=False)
    for _ in range(3):
        if code == 0:
            print("desktop up")
            return 0
        t.util("press", "Enter Network Password", "Cancel", echo=False, check=False)
        t.util("press", "Welcome to Windows", "Cancel", echo=False, check=False)
        code, out = t.util("waitwin", "Program Manager", "90", timeout=110, echo=False, check=False)
    print("no desktop yet")
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("verb")
    ap.add_argument("args", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    a.timeout = 60
    if a.args[:1] in (["-t"], ["--timeout"]) and len(a.args) > 1:
        a.timeout, a.args = int(a.args[1]), a.args[2:]
    t, v, x = T61(), a.verb, a.args
    try:
        if v == "ping":
            t.c.ping()
            print("T61 agent reachable")
        elif v in ("info", "ps"):
            t.util(v)
        elif v in ("dir", "type", "kill") and len(x) == 1:
            t.util(v, x[0])
        elif v == "get" and len(x) == 2:
            t.get(x[0], x[1])
        elif v == "put" and len(x) == 2:
            t.put(x[0], x[1])
        elif v == "run" and x:
            if is_path(x[0]):
                code, _ = t.util("exec", str(a.timeout), *x, timeout=min(a.timeout + 15, 600), check=False)
            else:
                res = t.c.run(x[0], x[1:], a.timeout)
                t.c.get("lastrun.log", t.tmp / "lastrun.log")
                sys.stdout.write((t.tmp / "lastrun.log").read_bytes().decode("latin-1"))
                print(f"\nexit code {res['exit_code']}{' (timed out)' if res['timed_out'] else ''}")
                code = res["exit_code"]
            return 0 if code == 0 else 1
        elif v == "sh" and x:
            lines = [ln for arg in x for ln in arg.splitlines() if ln.strip()]
            code, _ = t.util("sh", str(a.timeout), *lines, timeout=min(a.timeout + 15, 600), check=False)
            return 0 if code == 0 else 1
        elif v == "start" and x:
            t.util("start", *x)
        elif v == "logs":
            dest = Path(x[0] if x else "t61-logs")
            dest.mkdir(parents=True, exist_ok=True)
            for log in LOGS:
                code, _ = t.util("crc", log, echo=False, check=False)
                if code == 0:
                    t.get(log, dest / log.split("\\")[-1])
                else:
                    print(f"{log}: not present")
        elif v == "shot" and len(x) == 1:
            t.util("shot", "t61shot.bmp")
            bmp = t.tmp / "shot.bmp"
            t.c.get("t61shot.bmp", bmp)
            t.util("del", "t61shot.bmp", echo=False)
            bmp_to_png(bmp, x[0])
            print(f"saved {x[0]}")
        elif v == "install" and x:
            t.util("mkdir", BACKUP, echo=False)
            for f in x:
                name = Path(f).name
                dst = f"{SYSDIR}\\{name}"
                if t.util("crc", dst, echo=False, check=False)[0] == 0:
                    t.util("copy", dst, f"{BACKUP}\\{name}", echo=False)
                    print(f"backed up {dst} -> {BACKUP}\\{name}")
                try:
                    t.put(f, dst)
                except RuntimeError as exc:
                    if "error 32" not in str(exc):
                        raise
                    # in use (a DLL DirectDraw keeps loaded): stage it and let
                    # WININIT.INI replace it at the next boot
                    stem, ext = name.rsplit(".", 1) if "." in name else (name, "")
                    staged = (stem[:7] + "n." + ext[:3]).lower()
                    t.put(f, staged)
                    t.util("pending", dst, "C:\\remote\\" + staged)
                    print(f"{dst} is in use: replaced at the next reboot")
        elif v in ("wins", "kids", "focus", "close", "press", "waitwin", "click", "key", "text", "autostart", "debug") and (x or v == "wins"):
            secs = int(x[1]) + 15 if v == "waitwin" and len(x) == 2 else 60
            code, _ = t.util(v, *x, timeout=min(secs, 600), check=False)
            return 0 if code == 0 else 1
        elif v == "reboot":
            try:
                t.util("reboot", *x[:1])
            except (OSError, w98.ProtocolError):
                pass                      # the reply can be lost as Windows shuts down
            if wait_back(t, a.timeout if a.timeout != 60 else 600):
                return 1
            return logon(t)
        elif v == "endtasks":
            t.util("endtasks")
            if wait_back(t, a.timeout if a.timeout != 60 else 600):
                return 1
            return logon(t)
        elif v == "wait":
            return wait_back(t, int(x[0]) if x else 600, down_first=False)
        else:
            ap.print_help()
            return 2
        return 0
    except (OSError, ValueError, RuntimeError, w98.ProtocolError) as exc:
        print(f"t61: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
