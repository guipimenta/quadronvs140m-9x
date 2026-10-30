# Remote control of the T61

`t61.py` drives the ThinkPad T61 (Windows 98 SE) over the LAN. It builds on the
win98-remote project (`~/projects/win98-remote`): its agent `w98agent.exe` runs on
the T61 in `C:\remote`, and `controller/w98ctl.py` plus `controller.json` (the
pairing token, never commit it) live on this desktop. Protocol and setup:
`docs/remote/` and that project's README.

The agent alone can only put/get flat file names in `C:\remote`, at most 64 MiB, and
run an .exe there. `t61util.exe` (source `t61util.c`) adds the rest. The wrapper
stores it as `t61u<crc32>.exe` and uploads it only when that version is missing.

| Command | What it does |
| --- | --- |
| `t61.py ping` / `info` / `ps` | reachability; Windows, folders, memory, screen mode; processes |
| `t61.py dir C:\nvmini` / `type C:\nvlast.txt` | list a folder, print a file |
| `t61.py get C:\nvshim.log out.log` | any path, any size (split into 60 MiB parts, CRC32 end to end) |
| `t61.py put mesa3d.dll C:\somewhere\mesa3d.dll` | same, the other way |
| `t61.py run -t 300 ostest.exe threads 1` | run an exe in `C:\remote` (or a `C:\path\x.exe` in its own folder), print its output |
| `t61.py sh -t 60 "dir /b C:\NVLOGS" "ver"` | DOS lines run as a batch file in `C:\remote` (one per argument; newlines split too) |
| `t61.py start C:\WINDOWS\SYSTEM\dxdiag.exe` / `kill dxdiag.exe` | start without waiting / terminate |
| `t61.py logs DIR` | fetch C:\nvshim.log, nvlast, nvkick, nvtail, nvclk |
| `t61.py shot screen.png` | screenshot of the T61 desktop |
| `t61.py install FILE...` | copy into C:\WINDOWS\SYSTEM, previous copy kept in C:\NVBACKUP |
| `t61.py reboot` | restart Windows |

Limits:

- The agent does not start by itself after a reboot or crash: the user restarts it
  (until it is put in the Startup folder). So `install` of a file that needs a reboot
  (nvmini.vxd) and `reboot` end the remote session until the user is back.
- A file in use cannot be replaced (mesa3d.dll while a GL/D3D program runs: `kill` it first).
- There is no mouse/keyboard input: GUI tests (dxdiag's buttons) still need the user.
- `sh` captures what the batch prints to stdout; COMMAND.COM's own errors ("Bad command
  or file name") go to the console and are lost. The T61's PATH is C:\WINDOWS and
  C:\WINDOWS\COMMAND: the package's .bat helpers need their folder (`cd \nvmini`).
- A timeout kills only the immediate child; a DOS box it started can outlive it.
- A GPU hang freezes Windows, the agent with it: power-cycle by hand.
- Children never inherit the agent's lastrun.log handle (t61util swaps its own output
  handle for a private duplicate first). A started game that inherited it would hold
  the file open, and the agent would refuse every request until the game exits.

Build the helper (checked against Win98 exports):

    i686-w64-mingw32-gcc -O2 -s -Wall -Wno-format-truncation tools/remote/t61util.c -o tools/remote/t61util.exe -lgdi32
