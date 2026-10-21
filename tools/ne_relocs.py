import struct, sys
f = open(sys.argv[1], 'rb').read()
want = int(sys.argv[2]) if len(sys.argv)>2 else None
ne = struct.unpack_from('<I', f, 0x3c)[0]
(magic, linkver, linkrev, enttab, cbenttab, crc, flags, autodata, heap, stack,
 csip, sssp, nseg, nmod, cbnrestab, segtab, rsrctab, restab, modtab, imptab,
 nrestab, nmovent, align, nres, exetype) = struct.unpack_from('<2sBBHHIHHHHIIHHHHHHHHIHHHB', f, ne)
mods=[]
for i in range(nmod):
    o=struct.unpack_from('<H',f,ne+modtab+i*2)[0]; q=ne+imptab+o; mods.append(f[q+1:q+1+f[q]].decode('latin1'))
def impname(o):
    q=ne+imptab+o; return f[q+1:q+1+f[q]].decode('latin1')
for i in range(nseg):
    off, ln, fl, mn = struct.unpack_from('<HHHH', f, ne+segtab+i*8)
    if want and i+1!=want: continue
    base=off<<align; ln=ln or 0x10000
    if not fl&0x100: print(f"seg{i+1}: no relocs"); continue
    p=base+ln; n=struct.unpack_from('<H',f,p)[0]; p+=2
    print(f"seg{i+1}: {n} relocs")
    for k in range(n):
        st, rf, so, t1, t2 = struct.unpack_from('<BBHHH', f, p); p+=8
        kind=rf&3
        if kind==0: tgt=f"internal seg{t1&0xff}:{t2:04x}" if (t1&0xff)!=0xff else f"internal movable entry {t2}"
        elif kind==1: tgt=f"{mods[t1-1]}.{t2}"
        elif kind==2: tgt=f"{mods[t1-1]}!{impname(t2)}"
        else: tgt=f"osfixup {t1}"
        # follow chain
        chain=[so]
        if not rf&4:
            cur=so
            for _ in range(64):
                nxt=struct.unpack_from('<H',f,base+cur)[0]
                if nxt==0xffff or nxt==cur or nxt>=ln: break
                chain.append(nxt); cur=nxt
        print(f"  type={st} {'add' if rf&4 else 'chain'} at {','.join(f'{c:04x}' for c in chain)} -> {tgt}")
