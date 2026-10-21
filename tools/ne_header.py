import struct, sys
f = open(sys.argv[1], 'rb').read()
ne = struct.unpack_from('<I', f, 0x3c)[0]
assert f[ne:ne+2] == b'NE', f[ne:ne+2]
(magic, linkver, linkrev, enttab, cbenttab, crc, flags, autodata, heap, stack,
 csip, sssp, nseg, nmod, cbnrestab, segtab, rsrctab, restab, modtab, imptab,
 nrestab, nmovent, align, nres, exetype) = struct.unpack_from('<2sBBHHIHHHHIIHHHHHHHHIHHHB', f, ne)
print(f"NE: flags={flags:04x} autodata={autodata} heap={heap:#x} stack={stack:#x} entry CS:IP={csip>>16}:{csip&0xffff:04x} nseg={nseg} align={align} exetype={exetype}")
segs=[]
for i in range(nseg):
    off, ln, fl, mn = struct.unpack_from('<HHHH', f, ne+segtab+i*8)
    segs.append((off<<align, ln or 0x10000, fl, mn or 0x10000))
    print(f"  seg{i+1}: file={off<<align:#07x} len={ln:#06x} flags={fl:#06x} minalloc={mn:#06x} {'CODE' if not fl&1 else 'DATA'}")
# resident names
p = ne+restab
names=[]
while f[p]:
    n=f[p]; names.append((f[p+1:p+1+n].decode('latin1'), struct.unpack_from('<H',f,p+1+n)[0])); p+=n+3
print("resident names:", names[:40])
# module refs
p=ne+modtab; mods=[]
for i in range(nmod):
    o=struct.unpack_from('<H',f,p+i*2)[0]; q=ne+imptab+o; mods.append(f[q+1:q+1+f[q]].decode('latin1'))
print("imports:", mods)
# non-resident names
p=nrestab; nr=[]
while f[p]:
    n=f[p]; nr.append((f[p+1:p+1+n].decode('latin1'), struct.unpack_from('<H',f,p+1+n)[0])); p+=n+3
print("nonres names:", nr[:80])
# entry table
p=ne+enttab; ords=[]; o=1
end=p+cbenttab
while p<end:
    cnt=f[p]; typ=f[p+1]; p+=2
    if cnt==0: break
    if typ==0: o+=cnt; continue
    for k in range(cnt):
        if typ==0xff:
            fl, i3, seg, off = struct.unpack_from('<BHBH', f, p); p+=6; ords.append((o,seg,off)); 
        else:
            fl, off = struct.unpack_from('<BH', f, p); p+=3; ords.append((o,typ,off))
        o+=1
print("entries (ord,seg,off):", ords[:60])
