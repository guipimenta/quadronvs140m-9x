# Minimal MAPSYM (.SYM) reader: prints segments and symbols
import struct, sys
f=open(sys.argv[1],'rb').read()
p=0
while True:
    # MAPDEF
    (ppNextMap, bFlags, bReserved1, pSegEntry, cConsts, pConstDef, cSegs, pSegDef, cbMaxSym, cbModName) = struct.unpack_from('<HBBHHHHHBB', f, p)
    name=f[p+16:p+16+cbModName].decode('latin1')
    print(f"MAP {name} segs={cSegs} consts={cConsts}")
    seg=pSegDef*16
    for i in range(cSegs):
        (ppNextSeg, cSymbols, pSymDef, wReserved1, wReserved2, wReserved3, wReserved4, bFlags, bReserved1, ppLineDef, bReserved2, bReserved3, cbSegName) = struct.unpack_from('<HHHHHHHBBHBBB', f, seg)
        sname=f[seg+21:seg+21+cbSegName].decode('latin1')
        print(f" SEG {sname} syms={cSymbols} flags={bFlags:#x}")
        big = bFlags & 1
        for k in range(cSymbols):
            off=struct.unpack_from('<H', f, seg+pSymDef+k*2)[0]
            q=seg+off
            if big:
                val=struct.unpack_from('<I',f,q)[0]; ln=f[q+4]; nm=f[q+5:q+5+ln]
            else:
                val=struct.unpack_from('<H',f,q)[0]; ln=f[q+2]; nm=f[q+3:q+3+ln]
            print(f"  {val:08x} {nm.decode('latin1')}")
        if ppNextSeg==0: break
        seg=ppNextSeg*16
    if ppNextMap==0: break
    p=ppNextMap*16
