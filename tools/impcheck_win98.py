import sys, struct, os
def pe(d):
    p=struct.unpack_from('<I',d,0x3c)[0]; n=struct.unpack_from('<H',d,p+6)[0]; o=struct.unpack_from('<H',d,p+20)[0]
    secs=[(struct.unpack_from('<I',d,p+24+o+40*i+12)[0],struct.unpack_from('<I',d,p+24+o+40*i+16)[0],struct.unpack_from('<I',d,p+24+o+40*i+20)[0]) for i in range(n)]
    def r2o(r):
        for va,rs,ro in secs:
            if va<=r<va+rs: return r-va+ro
        return None
    return p, r2o
def exports(path):
    d=open(path,'rb').read(); p,r2o=pe(d); rva=struct.unpack_from('<I',d,p+24+96)[0]; e=r2o(rva)
    nf,nn,ad,nm,od=struct.unpack_from('<IIIII',d,e+20); names=set()
    for i in range(nn):
        nr=struct.unpack_from('<I',d,r2o(nm)+4*i)[0]; o=r2o(nr); names.add(d[o:d.index(b'\0',o)].decode('latin1'))
    return names, nf
def imports(path):
    d=open(path,'rb').read(); p,r2o=pe(d); rva=struct.unpack_from('<I',d,p+24+104)[0]; o=r2o(rva); res={}
    while True:
        ilt,ts,fc,nmr,iat=struct.unpack_from('<IIIII',d,o)
        if not nmr: break
        no=r2o(nmr); dll=d[no:d.index(b'\0',no)].decode('latin1').lower(); lst=[]; t=r2o(ilt or iat)
        while True:
            v=struct.unpack_from('<I',d,t)[0]
            if not v: break
            if v & 0x80000000: lst.append('#%d'%(v&0xffff))
            else:
                ho=r2o(v)+2; lst.append(d[ho:d.index(b'\0',ho)].decode('latin1'))
            t+=4
        res[dll]=lst; o+=20
    return res
expdir=sys.argv[1]; exp={}
for f in os.listdir(expdir):
    if f.lower().endswith('.dll'):
        try: exp[f.lower()]=exports(os.path.join(expdir,f))
        except Exception as ex: print('exp fail',f,ex)
for path in sys.argv[2:]:
    print('=====', path); imp=imports(path)
    for dll,lst in imp.items():
        if dll not in exp: print('  ', dll, 'not in export set'); continue
        names,nf=exp[dll]; miss=[s for s in lst if not s.startswith('#') and s not in names]
        print('  %-14s %4d imports, missing on Win98: %s' % (dll, len(lst), miss if miss else 'none'))
