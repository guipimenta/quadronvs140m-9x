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
| `t61.py put mesa3d.dll C:\somewhere\mesa3d.dll` | same, the other way, in 8 MiB parts (w98ctl gives one PUT 20 s in total); the 80 MB mesa3d.dll takes ~3 min, so try a new one from `C:\remote` first (`tlvtest.exe ... dll C:\remote\m3d.dll`, `ostest dll ...`) before installing it |
| `t61.py run -t 300 ostest.exe threads 1` | run an exe in `C:\remote` (or a `C:\path\x.exe` in its own folder), print its output |
| `t61.py sh -t 60 "dir /b C:\NVLOGS" "ver"` | DOS lines run as a batch file in `C:\remote` (one per argument; newlines split too) |
| `t61.py start C:\WINDOWS\SYSTEM\dxdiag.exe` / `kill dxdiag.exe` | start without waiting / terminate |
| `t61.py logs DIR` | fetch C:\nvshim.log, nvlast, nvkick, nvtail, nvclk |
| `t61.py shot screen.png` | screenshot of the T61 desktop |
| `t61.py install FILE...` | copy into C:\WINDOWS\SYSTEM, previous copy kept in C:\NVBACKUP |
| `t61.py reboot [force]` | restart like the Start menu, wait for the agent, skip the network logon (Cancel, nothing typed), wait for the desktop |
| `t61.py autostart on\|off\|show` | start the agent at boot: RunServices (before logon) + Run, both `C:\remote\t61boot.exe supervise` |
| `t61.py wins [TITLE]` / `kids TITLE` | visible windows / a window's controls (class, id, rectangle, text) |
| `t61.py press TITLE BUTTON` | click a dialog button by its text (works without focus or coordinates) |
| `t61.py focus TITLE` / `close TITLE` / `waitwin TITLE SECS` | foreground, WM_CLOSE, wait for a window |
| `t61.py click X Y [right\|double]` / `key ctrl+tab tab*3 enter` / `text "abc"` | mouse and keyboard |
| `t61.py debug C:\path\x.exe [ARGS]` | run detached under a Win32 debugger: DLL loads, exceptions (module+offset, registers, stack), the exiting thread's stack, to `C:\remote\t61debug.txt` |
| `t61.py endtasks` | a shutdown stuck on the agent console's End Task prompt: answer it, then as `reboot` |

Limits:

- Autostart is ON since 2026-10-30: `t61boot.exe supervise` (a copy of the helper) keeps
  `w98agent.exe` running and retries every 10 s while the network is not up; its log is
  `C:\remote\t61boot.log`. A hang at boot or shutdown still needs a power cycle by hand.
- `reboot force` (ExitWindowsEx EWX_FORCE from the console helper) hung the T61 once after
  the display driver's Disable; the default goes through SHExitWindowsEx like the Start menu.
- A file in use cannot be replaced (mesa3d.dll while a GL/D3D program runs: `kill` it first).
- The debugger cannot see faults in 16-bit code (DDRAW16, nvmini.drv): such a process can
  vanish with exit code -1 and no exception event.
- `shot` captures what GDI sees. OpenGL programs on the GPU present through our own
  path straight into the scanout, so `shot` shows the last GDI picture (e.g. Half-Life's
  "Loading...") while the game runs. Direct3D (vmhal3d) frames do show. A VRAM scanout
  grabber is still to be written; until then judge OpenGL runs by C:\nvlast.txt progress
  and the user's eyes.
- Closing a game: `close TITLE` posts WM_CLOSE; Half-Life then asks "Quit Half-Life
  without saving the current game" in an UNTITLED dialog: `press "*" Ok`. Do not `kill` a
  game that holds the screen in fullscreen DirectDraw mode: it froze the T61 once.
  Half-Life's console needs `-console` on its command line (else the backtick opens chat).
- Focus uses AttachThreadInput, not the usual synthetic Alt tap: Alt press+release puts
  the target's menu bar in menu mode and the typed keys open menus instead.
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
