import struct, sys, collections
def relocs(p):
    d=open(p,"rb").read(); ne=struct.unpack_from("<I",d,0x3c)[0]
    nseg=struct.unpack_from("<H",d,ne+0x1c)[0]; segtab=struct.unpack_from("<H",d,ne+0x22)[0]; align=struct.unpack_from("<H",d,ne+0x32)[0]
    print("=== %s"%p.split('/')[-1])
    for i in range(nseg):
        off,ln,fl,mn=struct.unpack_from("<HHHH",d,ne+segtab+8*i)
        if not (fl&0x100): print(" seg%d: no relocs"%(i+1)); continue
        base=(off<<align); p0=base+(ln if ln else 0x10000); cnt=struct.unpack_from("<H",d,p0)[0]; p0+=2
        c=collections.Counter(); osf=[]
        for j in range(cnt):
            at,rt,so,t1,t2=struct.unpack_from("<BBHHH",d,p0+8*j)
            kind={0:"internal",1:"import-ord",2:"import-name",3:"OSFIXUP"}.get(rt&3,"?")
            add="+additive" if rt&4 else ""
            if (rt&3)==0: kind+=("(movable->ord %d)"%t2 if t1&0xff==0xff else "(seg%d)"%(t1&0xff))
            if (rt&3)==3: osf.append((so,t1))
            c["atype%d %s%s"%(at,kind,add)]+=1
        print(" seg%d: %d relocs"%(i+1,cnt)); 
        for k,v in sorted(c.items()): print("    %-40s %d"%(k,v))
        if osf: print("    OSFIXUP targets:", sorted(set(t for _,t in osf)))
for p in sys.argv[1:]: relocs(p)
