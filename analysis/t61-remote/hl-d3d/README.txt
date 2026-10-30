Half-Life 1.0.1.0 Direct3D on the 3117 driver (T61, 2026-10-30/31), see docs/HW3D_PLAN.md.
c1a0-gpu.png / c1a0-sw.png  default run, GPU vs llvmpipe (same damage)
gl_clear_1.png              +gl_clear 1: the world's base pass never lands (red)
depth-off.png               +gl_clear 1 with the HAL's depth test forced off: red-tinted = lightmap pass only
hlreg-before.txt            Half-Life's renderer settings before the tests (restored)
vmhal-debug-instrumentation.patch
    against ~/projects/audio_reverse/vmhal9x-src (upstream 8bc480e): config.mk DEBUG=1 DDDEBUG=3,
    log gate C:\vmhal.go (C:\vmhal.all = every line), selected topics, TL vertex anomalies,
    per-scene z range + GL depth readback, C:\vmhal.nodepth switch, XP caps (CLIPTLVERTS,
    guard band +-1e8, no SORT* devcaps). Build: git archive the tree into a scratch dir, copy
    the fixlink/nocrt/regex submodules, apply, make vmhal9x.dll vmhal3d.dll; install with
    tools/remote/t61.py install (WININIT.INI, reboot). Log: C:\vmhal9x.log.
