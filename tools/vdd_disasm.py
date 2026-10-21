import sys, subprocess, bisect, re, glob, os
D=sys.argv[1]
g2o={'_LGROUP':1,'_IMGROUP':2,'_PMGROUP':3,'_IGROUP':4,'_PTEXT':5,'_PDATA':6,'_LOCKABLE':8,'_RARE':9,'_VMCREATE':10,'_VMDESTROY':11,'_THCREATE':12,'_VMRESUME':13,'_PNP':14,'_DOSVM':15,'_TEXT':16,'CONST':18}
imgs={}
for p in glob.glob(D+'/vddimg_*.bin'):
    idx=int(p.split('vddimg_')[1].split('_')[0]); imgs[idx]=open(p,'rb').read()
syms={}; byname={}
for line in open(D+'/vdd_se_debug.syms'):
    g,off,name=line.split(); off=int(off,16)
    syms.setdefault(g,[]).append((off,name)); byname.setdefault(name,(g,off))
for g in syms: syms[g].sort()
# service tables from DDK .INC files
devids={}; services={}
for p in glob.glob(D+'/inc/*.INC'):
    txt=open(p,'r',errors='replace').read()
    for m in re.finditer(r'^\s*(\w+?)_DEVICE_ID\s+EQU\s+0*([0-9A-Fa-f]+)[hH]',txt,re.M):
        devids[int(m.group(2),16)]=m.group(1)
    for m in re.finditer(r'Begin_Service_Table\s+(\w+)',txt):
        name=m.group(1); start=m.end()
        endm=re.search(r'End_Service_Table',txt[start:]); body=txt[start:start+endm.start()] if endm else txt[start:]
        lst=re.findall(r'^\s*'+name+r'_Service\s+(\w+)',body,re.M)
        if lst: services[name]=lst
def svcname(v):
    dev=v>>16; idx=v&0xffff
    dn=devids.get(dev,f"dev{dev:#x}")
    lst=services.get(dn)
    return f"{dn}.{lst[idx]}" if lst and idx<len(lst) else f"{dn}.svc{idx:#x}"
def symat(g,off):
    lst=syms[g]; i=bisect.bisect_right(lst,(off,'\xff'))-1
    if i<0: return None
    o,n=lst[i]; return n if o==off else f"{n}+{off-o:#x}"
def dis(nm, maxlines):
    if ':' in nm:
        g,a,b=nm.split(':'); off=int(a,16); end=off+int(b,16)
    else:
        if nm not in byname: print("?? no symbol",nm); return
        g,off=byname[nm]; lst=syms[g]; i=bisect.bisect_right(lst,(off,'\xff'))
        end=lst[i][0] if i<len(lst) else len(imgs[g2o[g]])
        end=min(end, off+0x800)
    data=imgs[g2o[g]][off:end]
    open(D+'/tmp.bin','wb').write(data)
    skips=[]; j=0
    while True:
        j=data.find(b'\xcd\x20',j)
        if j<0: break
        skips.append(j); j+=6
    out=[]; pos=0
    for s in skips+[len(data)]:
        if s>pos:
            open(D+'/tmp.bin','wb').write(data[pos:s])
            out+=subprocess.run(['ndisasm','-b32','-o',hex(off+pos),D+'/tmp.bin'],capture_output=True,text=True).stdout.splitlines()
        if s<len(data):
            out.append(f"{off+s:08X}  skipping")
        pos=s+6
    print(f"==== {nm}  ({g} obj{g2o[g]} {off:#x}-{end:#x})")
    n=0
    for ln in out:
        if n>=maxlines: print('...'); break
        m=re.match(r'^([0-9A-F]+)\s+skipping',ln)
        if m:
            a=int(m.group(1),16); v=int.from_bytes(data[a-off+2:a-off+6],'little')
            print(f"{a:08X}  VxDCall {svcname(v)} ({v:#010x})"); n+=1; continue
        ann=''
        m=re.search(r'(call|jmp|j[a-z]+)\s+(?:dword\s+|near\s+)?0x([0-9a-f]+)$',ln)
        if m:
            t=int(m.group(2),16); s=symat(g,t)
            if s: ann='   ; '+s
        if not ann:
            m3=re.search(r'push (?:dword )?0x([0-9a-f]+)$',ln) or re.search(r',0x([0-9a-f]+)$',ln)
            if m3:
                v=int(m3.group(1),16); ob=(v>>24)+1; o=v&0xffffff
                im=imgs.get(ob)
                if im and o<len(im):
                    s=im[o:o+48].split(b'\0')[0]
                    if len(s)>=3 and all(32<=c<127 for c in s): ann='   ; "'+s.decode()+'"'
                    else:
                        gg=[k for k,vv in g2o.items() if vv==ob]
                        if gg: 
                            t=symat(gg[0],o)
                            if t: ann='   ; '+gg[0]+':'+t
        print(ln[:70]+ann); n+=1
maxlines=int(sys.argv[2])
for nm in sys.argv[3:]: dis(nm,maxlines)
