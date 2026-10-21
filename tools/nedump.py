import struct, sys
def dump(p):
    d=open(p,"rb").read(); ne=struct.unpack_from("<I",d,0x3c)[0]; h=lambda o,f="<H": struct.unpack_from(f,d,ne+o)[0]
    print("=== %s (%d bytes) NE@%#x"%(p.split('/')[-1],len(d),ne))
    print(" linker %d.%d entoff %#x entlen %#x flags %04x autods %d heap %#x stack %#x csip %04x:%04x sssp %04x:%04x"%(d[ne+2],d[ne+3],h(4),h(6),h(0x0c),h(0x0e),h(0x10),h(0x12),h(0x16),h(0x14),h(0x1a),h(0x18)))
    print(" nseg %d nmod %d nresnm %#x segtab %#x restab %#x resnm %#x modtab %#x imptab %#x nonres %#x(%d) nmovent %d align %d nres %d exetype %#x addflags %#x expver %04x"%(h(0x1c),h(0x1e),h(0x20),h(0x22),h(0x24),h(0x26),h(0x28),h(0x2a),h(0x2c,"<I"),h(0x30),h(0x32),h(0x34),h(0x36),d[ne+0x36],d[ne+0x37],h(0x3e)))
    align=h(0x32); nseg=h(0x1c)
    for i in range(nseg):
        off,ln,fl,mn=struct.unpack_from("<HHHH",d,ne+h(0x22)+8*i)
        print("  seg%d file=%#x len=%#x flags=%04x minalloc=%#x"%(i+1,off<<align,ln,fl,mn))
    # resource table
    rt=ne+h(0x24); shift=struct.unpack_from("<H",d,rt)[0]; p=rt+2; print(" resources (shift %d):"%shift)
    while True:
        tid,cnt=struct.unpack_from("<HH",d,p); 
        if tid==0: break
        p+=8
        for j in range(cnt):
            ro,rl,rf,rid=struct.unpack_from("<HHHH",d,p); p+=12
            print("   type %#06x id %#06x file=%#x len=%#x flags=%04x"%(tid,rid,ro<<shift,rl<<shift,rf))
    # entry table bundles
    p=ne+h(4); end=p+h(6); o=1; bundles=[]
    while p<end:
        cnt,typ=d[p],d[p+1]; p+=2
        if cnt==0: break
        if typ==0: bundles.append("skip%d"%cnt); o+=cnt; continue
        ents=[]
        for j in range(cnt):
            if typ==0xFF: fl,int3f,seg,off=struct.unpack_from("<BHBH",d,p); p+=6; ents.append("%d:mov s%d:%04x f%02x"%(o,seg,off,fl))
            else: fl,off=struct.unpack_from("<BH",d,p); p+=3; ents.append("%d:fix s%d:%04x f%02x"%(o,typ,off,fl))
            o+=1
        bundles.append(("movable" if typ==0xFF else "fixed(seg%d)"%typ)+"x%d["%cnt+" ".join(ents[:3])+("..." if cnt>3 else "")+"]")
    print(" entry bundles:", " | ".join(bundles))
    # module refs / imported names
    nm=h(0x1e); mods=[]
    for i in range(nm):
        no=struct.unpack_from("<H",d,ne+h(0x28)+2*i)[0]; q=ne+h(0x2a)+no; mods.append(d[q+1:q+1+d[q]].decode())
    print(" modules:",mods)
    q=ne+h(0x2a); names=[]
    while q<ne+h(0x4)+0 or q< ne+h(0x2c,"<I"):
        l=d[q]; 
        if q>=ne+h(4): break
        names.append(d[q+1:q+1+l].decode(errors="replace")); q+=1+l
        if len(names)>40: break
    print(" imported names:",names)
    print(" resident names:", d[ne+h(0x26)+1:ne+h(0x26)+1+d[ne+h(0x26)]])
for p in sys.argv[1:]: dump(p)
