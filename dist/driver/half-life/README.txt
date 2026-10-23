Half-Life software-OpenGL speed config
======================================

OpenGL mode on this driver is rendered entirely on the CPU (Mesa llvmpipe),
because the G86's own 3D engine is not driven yet and Windows 98 only uses one
CPU core. Frame rate therefore depends mostly on:

  1. The OpenGL resolution (Options -> Video -> Video modes -> OpenGL).
     Cost is per pixel, so dropping from 640x480 to 320x240 is ~4x fewer
     pixels and roughly 4x the frame rate.
  2. How much work each pixel costs - texture filtering, dynamic lighting,
     decals, etc. That is what autoexec.cfg tunes.

Install
-------
Copy autoexec.cfg into your Half-Life "valve" folder (next to config.cfg).
It runs automatically at startup (valve.rc execs autoexec.cfg last, so it
overrides the saved config). For Team Fortress / other mods, copy it into
that mod's folder too.

Measure
-------
In-game, open the console (~), aim at a typical scene, and type:
  timerefresh
It redraws the current view 128 times and prints "<n> seconds (<fps> fps)".
Run it before and after to see the gain, and at different resolutions.

Revert
------
Delete valve\autoexec.cfg (or comment out individual lines with //).
