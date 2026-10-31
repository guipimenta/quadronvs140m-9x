Half-Life 1.0.1.0 Direct3D on the 3117/3118 driver (T61, 2026-10-30/31), see docs/HW3D_PLAN.md.
c1a0-gpu.png / c1a0-sw.png  default run on 3117, GPU vs llvmpipe
gl_clear_1.png              +gl_clear 1: red screen - the same under XP, a Half-Life quirk (false lead)
depth-off.png               +gl_clear 1 with the HAL's depth test forced off
hlreg-before.txt            Half-Life's renderer settings before the tests (restored)

ROOT CAUSE (fixed in 3118, mesa3d.dll's shim): the shim's libdrm never filled the bufctx
pending/current lists, so nv50 fenced no buffer; with draws accumulating until a flush
(since 3095) Mesa's full 512 KiB immediate-mode vertex buffer was reused from offset 0 and
the frame's first draws (the tram's near walls) were overwritten before the GPU read them.
3117-c1a0-broken.png         the frame on 3117 (Direct3D, GPU)
3117-c1a0-frame.cap          one-shot DP2 capture of that frame (2 complete scenes)
3117-c1a0-dp2replay.png      the capture rasterized on the PC (tools/remote/dp2replay.py): the
                             data Half-Life sends is right (compare analysis/t61-remote/xp-ref/)
3117-c1a0-redfan-overdraw.png  debug HAL drawing every fan again in solid red: wrong places and
                             colours on the T61 = the corruption is below the HAL
3117-c1a0-glstate.txt        GL state inside the HAL at the first fans (C:\vmhal.gls): ordinary
3118-c1a0.png                the same frame on 3118: right (white corner wedges still open)
3118-c1a0-wedges.png         3118 with the shipped vmhal3d: the wedges (pure white, single near-plane fans)
3118-c1a0-debug-smooth.png   debug HAL, C:\vmhal.smooth (D3D flat drawn smooth): no wedges
3119-c1a0.png, 3119-c1a0-turned.png   build 3119 (patched vmhal3d.dll): clean
3118-c1a0-walltexture.bin    a wall texture's mip chain as the HAL uploads it (C:\vmhal.texdump),
                             input for `tlvtest ... texfile`
tlvtest/t61-gpu-hltexture-flat.png    that texture + flat shading on the G86: the wedges
tlvtest/t61-llvmpipe-hltexture-flat.png   the same on llvmpipe: clean
tlvtest/t61-gpu-notexture-flat.png    flat shading without texture: white + gradients on
                             constant-colour triangles = the hardware's clipped flat colour
tlvtest/                     driver/vmdisp9x-nv/tools/tlvtest.exe on the T61 GPU, input =
                             `dp2replay.py 3117-c1a0-frame.cap --tlv tris.bin 1`:
                             plain = right; hal 1 on 3117 = Half-Life's damage; all switches
                             on 3118 = right; mip chain on 3118 = level 0 everywhere
vmhal-debug-instrumentation.patch
    against ~/projects/audio_reverse/vmhal9x-src (upstream 8bc480e): config.mk DEBUG=1 DDDEBUG=3,
    log gate C:\vmhal.go (C:\vmhal.all = every line), selected topics, TL vertex anomalies,
    per-scene z range + GL depth readback, C:\vmhal.nodepth (no depth test), XP caps,
    C:\vmhal.capon (one-shot DP2 capture -> C:\vmhal.cap), C:\vmhal.redfan (fans overdrawn red),
    C:\vmhal.gls (GL state dump -> C:\vmhal.gls.txt), C:\vmhal.noblend (skip lightmap-pass fans),
    C:\vmhal.uvfan (fans overdrawn with texcoords as colour), C:\vmhal.lod0 (LOD bias -16),
    C:\vmhal.texnc/texnd/texna (fans drawn again without cull/depth/alpha test),
    C:\vmhal.vbdump (GL inputs of off-screen fan vertices -> C:\vmhal.vb.txt),
    C:\vmhal.texdump (the fan's texture: surface mip chain -> C:\vmhal.tex.bin, GL copy via
    FBO -> C:\vmhal.gltex.bin), C:\vmhal.smooth (D3D flat drawn smooth), and the dd_fifo fix
    (driver/vmhal9x-bin/vmhal3d-ddfifo.patch). Build: git archive the tree into a scratch dir,
    copy the fixlink/nocrt/regex submodules, apply, make vmhal9x.dll vmhal3d.dll; install with
    tools/remote/t61.py install (WININIT.INI, reboot). Log: C:\vmhal9x.log.
