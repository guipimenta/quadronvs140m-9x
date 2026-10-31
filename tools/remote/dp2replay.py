#!/usr/bin/env python3
"""dp2replay.py - replay a raw Direct3D DrawPrimitives2 capture as screen-space triangles.

The debug vmhal3d (analysis/t61-remote/hl-d3d/vmhal-debug-instrumentation.patch, capture
hunk in hal3d_DrawPrimitives2_32) buffers every DP2 call once C:\\vmhal.capon exists, until
three scene ends have passed (two complete scenes), and writes C:\\vmhal.cap ONCE: per call
a record header (8 dwords: 'DP2C', dwVertexType, command bytes, vertex bytes, dwFlags,
first vertex index captured, stride, command buffer address), the command buffer, then
only the vertex range the batch references (indices are rebased with the first index).

This tool parses the DP2 opcodes the captured title uses and rasterizes the pre-transformed
(XYZRHW) triangles exactly as Direct3D defines them - screen x/y, z test LESSEQUAL into a
z-buffer, no clipping - so the picture is what the data says, independent of the HAL's
conversion to OpenGL. Colour = a hash of the bound texture handle (stage 0 TEXTUREMAP),
blended draws (ALPHABLENDENABLE) are skipped, culling ignored.

  dp2replay.py frame.cap out.png [scene#]     (scenes split at RENDERSTATE SCENECAPTURE=0)
  dp2replay.py frame.cap --stats
  dp2replay.py frame.cap --tlv tris.bin [scene#]
      the scene's triangles in vmhal3d's vertex order for driver/vmdisp9x-nv/tools/tlvtest.exe
      (format 2: DWORD flags, DWORD rgb, float v[3][6] = x y z rhw u v; flags bit 0-1 cull
      0 none/1 GL_FRONT/2 GL_BACK, bit 2 indexed list (HAL reverses the order and swaps the
      cull face), bit 3 z test, bit 4 z write, bit 5 blended, bit 6 continues the previous
      row's fan, bit 7 first triangle of a fan, bits 8-11 D3DCMPFUNC)
"""
import struct, sys, zlib

RS_ZENABLE, RS_ZWRITE, RS_CULL, RS_ZFUNC, RS_ABLEND, RS_SCENECAPTURE = 7, 14, 22, 23, 27, 62
OP = {1: 'POINTS', 2: 'INDEXEDLINELIST', 3: 'INDEXEDTRIANGLELIST', 8: 'RENDERSTATE', 15: 'LINELIST',
      16: 'LINESTRIP', 17: 'INDEXEDLINESTRIP', 18: 'TRIANGLELIST', 19: 'TRIANGLESTRIP',
      20: 'INDEXEDTRIANGLESTRIP', 21: 'TRIANGLEFAN', 22: 'INDEXEDTRIANGLEFAN', 23: 'TRIANGLEFAN_IMM',
      24: 'LINELIST_IMM', 25: 'TEXTURESTAGESTATE', 26: 'INDEXEDTRIANGLELIST2', 27: 'INDEXEDLINELIST2',
      28: 'VIEWPORTINFO', 29: 'WINFO', 32: 'ZRANGE', 42: 'CLEAR'}


def records(path):
    d = open(path, 'rb').read()
    o = 0
    while o + 32 <= len(d):
        magic, fvf, clen, vbytes, flags, vlen, stride, tick = struct.unpack_from('<8I', d, o)
        if magic != 0x43325044:
            raise SystemExit(f'bad record magic at {o}')
        o += 32
        cmd = d[o:o + clen]; o += clen
        vb = d[o:o + vbytes]; o += vbytes
        yield fvf, cmd, vb, stride, flags, tick, vlen


def vertex(buf, i, stride):
    x, y, z, rhw = struct.unpack_from('<4f', buf, i * stride)
    return x, y, z, rhw


def walk(path):
    """Yield events: ('state', rs, value), ('tss', stage, state, value), ('tri', v0, v1, v2),
    ('viewport', x, y, w, h), ('clear', flags), ('scene', 0/1), ('unknown', op)."""
    for fvf, cmd, vb, stride, flags, tick, vfirst in records(path):
        def vertex(buf, i, stride, _f=vfirst, _vb=vb):
            if buf is _vb:
                i -= _f
            x, y, z, rhw = struct.unpack_from('<4f', buf, i * stride)
            return x, y, z, rhw
        p, end = 0, len(cmd)
        while p + 4 <= end:
            op, _, count = struct.unpack_from('<BBH', cmd, p)
            p += 4
            if op == 8:
                for i in range(count):
                    rs, val = struct.unpack_from('<2I', cmd, p); p += 8
                    yield ('state', rs, val)
                    if rs == RS_SCENECAPTURE:
                        yield ('scene', val)
            elif op == 25:
                for i in range(count):
                    st, ts, val = struct.unpack_from('<HHI', cmd, p); p += 8
                    yield ('tss', st, ts, val)
            elif op == 23:   # TRIANGLEFAN_IMM: edge flags, aligned vertices inline
                p += 4
                p = (p + 3) & ~3
                n = count + 2
                vs = [vertex(cmd[p:], i, stride) for i in range(n)]
                p += n * stride
                for i in range(1, n - 1):
                    yield ('tri', vs[0], vs[i], vs[i + 1])
            elif op == 26:   # INDEXEDTRIANGLELIST2: wVStart, then {v1,v2,v3} (6 bytes, no flags)
                base, = struct.unpack_from('<H', cmd, p); p += 2
                for i in range(count):
                    a, b, c = struct.unpack_from('<3H', cmd, p); p += 6
                    try:
                        yield ('tri', vertex(vb, base + a, stride), vertex(vb, base + b, stride), vertex(vb, base + c, stride))
                    except struct.error:
                        yield ('badindex', base, a, b, c)
            elif op == 3:
                for i in range(count):
                    a, b, c, _f = struct.unpack_from('<4H', cmd, p); p += 8
                    yield ('tri', vertex(vb, a, stride), vertex(vb, b, stride), vertex(vb, c, stride))
            elif op == 18:
                s, = struct.unpack_from('<H', cmd, p); p += 2
                for i in range(count):
                    yield ('tri', vertex(vb, s + 3 * i, stride), vertex(vb, s + 3 * i + 1, stride), vertex(vb, s + 3 * i + 2, stride))
            elif op == 21:
                s, = struct.unpack_from('<H', cmd, p); p += 2
                for i in range(count):
                    yield ('tri', vertex(vb, s, stride), vertex(vb, s + i + 1, stride), vertex(vb, s + i + 2, stride))
            elif op == 28:
                for i in range(count):
                    x, y, w, h = struct.unpack_from('<4I', cmd, p); p += 16
                    yield ('viewport', x, y, w, h)
            elif op in (29, 32):
                p += 8 * count
            elif op == 42:
                fl, = struct.unpack_from('<I', cmd, p)
                p += 16 + 16 * count
                yield ('clear', fl)
            else:
                yield ('unknown', op)
                break


def raster(events, W, H, scene_wanted):
    color = bytearray(W * H * 3)
    zbuf = [2.0] * (W * H)
    state = {RS_ZENABLE: 1, RS_ZWRITE: 1, RS_ABLEND: 0}
    tex = 0
    scene = 0
    ntri = drawn = 0
    for ev in events:
        k = ev[0]
        if k == 'scene' and ev[1] == 0:
            scene += 1
            if scene > scene_wanted:
                break
            continue
        if k == 'state':
            state[ev[1]] = ev[2]
        elif k == 'tss' and ev[1] == 0 and ev[2] == 0:
            tex = ev[3]
        elif k == 'tri' and scene == scene_wanted:
            ntri += 1
            if state.get(RS_ABLEND):
                continue
            h = zlib.crc32(struct.pack('<I', tex))
            col = (64 + (h & 0x7f), 64 + ((h >> 8) & 0x7f), 64 + ((h >> 16) & 0x7f))
            drawn += fill(color, zbuf, W, H, ev[1], ev[2], ev[3], col, state.get(RS_ZENABLE, 1), state.get(RS_ZWRITE, 1))
    return color, ntri, drawn


def fill(color, zbuf, W, H, a, b, c, col, ztest, zwrite):
    (x0, y0, z0, _), (x1, y1, z1, _), (x2, y2, z2, _) = a, b, c
    area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if area == 0:
        return 0
    minx, maxx = max(int(min(x0, x1, x2)), 0), min(int(max(x0, x1, x2)) + 1, W)
    miny, maxy = max(int(min(y0, y1, y2)), 0), min(int(max(y0, y1, y2)) + 1, H)
    n = 0
    for y in range(miny, maxy):
        py = y + 0.5
        for x in range(minx, maxx):
            px = x + 0.5
            w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area
            w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area
            w2 = 1.0 - w0 - w1
            if w0 < 0 or w1 < 0 or w2 < 0:
                continue
            z = w0 * z0 + w1 * z1 + w2 * z2
            i = y * W + x
            if ztest and z > zbuf[i]:
                continue
            if zwrite:
                zbuf[i] = z
            color[i * 3:i * 3 + 3] = bytes(col)
            n += 1
    return n


def save_png(path, W, H, rgb):
    raw = b''.join(b'\0' + bytes(rgb[y * W * 3:(y + 1) * W * 3]) for y in range(H))
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def export_tlv(path, out, want):
    st = {7: 1, 14: 1, 22: 3, 23: 4, 27: 0}
    tex = 0
    scene = 0
    rows = []
    for fvf, cmd, vb, stride, flags, base_addr, vfirst in records(path):
        def V(buf, i):
            x, y, z, r, c, u, v = struct.unpack_from('<4fI2f', buf, i * stride)
            return (x, y, z, r, u, v)
        p, end = 0, len(cmd)
        while p + 4 <= end:
            op, _, count = struct.unpack_from('<BBH', cmd, p); p += 4
            tris = []
            if op == 8:
                for i in range(count):
                    rs, val = struct.unpack_from('<2I', cmd, p); p += 8
                    st[rs] = val
                    if rs == RS_SCENECAPTURE and val == 0:
                        scene += 1
                continue
            elif op == 25:
                for i in range(count):
                    s_, ts, val = struct.unpack_from('<HHI', cmd, p); p += 8
                    if s_ == 0 and ts == 0:
                        tex = val
                continue
            elif op == 23:
                p += 4; p = (p + 3) & ~3
                n = count + 2
                vs = [V(cmd[p:], i) for i in range(n)]
                p += n * stride
                tris = [((0x80 if i == 1 else 0x40), vs[0], vs[i], vs[i + 1]) for i in range(1, n - 1)]
            elif op == 26:
                base, = struct.unpack_from('<H', cmd, p); p += 2
                for i in range(count):
                    a, b, c = struct.unpack_from('<3H', cmd, p); p += 6
                    tris.append((4, V(vb, base + c - vfirst), V(vb, base + b - vfirst), V(vb, base + a - vfirst)))
            elif op == 28:
                p += 16 * count; continue
            elif op in (29, 32):
                p += 8 * count; continue
            elif op == 42:
                p += 16 + 16 * count; continue
            else:
                raise SystemExit(f'unknown op {op}')
            if scene != want:
                continue
            h = zlib.crc32(struct.pack('<I', tex))
            col = (64 + (h & 0x7f)) | (64 + ((h >> 8) & 0x7f)) << 8 | (64 + ((h >> 16) & 0x7f)) << 16
            cull = {1: 0, 2: 1, 3: 2}.get(st.get(RS_CULL, 3), 0)
            for bits, a, b, c in tris:
                fl = (cull | bits | (st.get(RS_ZENABLE, 1) and 1) << 3 | (st.get(RS_ZWRITE, 1) and 1) << 4
                      | (st.get(RS_ABLEND, 0) and 1) << 5 | (st.get(RS_ZFUNC, 4) & 15) << 8)
                rows.append(struct.pack('<2I18f', fl, col, *a, *b, *c))
    open(out, 'wb').write(b''.join(rows))
    print(f'scene {want}: {len(rows)} triangles -> {out}')


def main():
    path = sys.argv[1]
    if len(sys.argv) > 3 and sys.argv[2] == '--tlv':
        export_tlv(path, sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 1)
        return
    if len(sys.argv) > 2 and sys.argv[2] == '--stats':
        from collections import Counter
        c = Counter(); scenes = []; W = H = 0; xs = []; ys = []
        for ev in walk(path):
            c[ev[0] if ev[0] != 'unknown' else f'unknown op {ev[1]}'] += 1
            if ev[0] == 'scene':
                scenes.append(ev[1])
            if ev[0] == 'viewport':
                W, H = ev[3], ev[4]
            if ev[0] == 'tri':
                for v in ev[1:]:
                    xs.append(v[0]); ys.append(v[1])
        print(dict(c)); print('scene marks', scenes[:20]); print('viewport', W, H)
        if xs:
            print('x', min(xs), max(xs), 'y', min(ys), max(ys))
        return
    out = sys.argv[2]
    want = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    W = H = 0
    for ev in walk(path):
        if ev[0] == 'viewport':
            W, H = ev[3], ev[4]
            break
    W, H = W or 640, H or 480
    rgb, ntri, drawn = raster(walk(path), W, H, want)
    save_png(out, W, H, rgb)
    print(f'scene {want}: {ntri} triangles, {drawn} pixels written -> {out} ({W}x{H})')


if __name__ == '__main__':
    main()
