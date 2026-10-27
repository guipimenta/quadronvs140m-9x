import socket, json, sys, time
PORT=4448
def cmd(execute, args=None):
    s=socket.create_connection(("127.0.0.1",PORT),timeout=20); f=s.makefile("rw"); f.readline()
    s.sendall(b'{"execute":"qmp_capabilities"}\n'); f.readline()
    m={"execute":execute}
    if args: m["arguments"]=args
    s.sendall(json.dumps(m).encode()+b"\n"); r=f.readline().strip(); s.close(); return r
def key(k):
    keys=[{"type":"qcode","data":x} for x in k.split("-")]
    r=cmd("send-key",{"keys":keys}); time.sleep(0.12); return r
a=sys.argv[1]
if a=="shot": print(cmd("screendump",{"filename":sys.argv[2]}))
elif a=="keys":
    for k in sys.argv[2:]: print(k, key(k))
elif a=="type":
    m={'-':'minus','.':'dot',' ':'spc','\\':'backslash',':':'shift-semicolon','_':'shift-minus','/':'slash',
       '=':'equal','"':'shift-apostrophe','\n':'ret',',':'comma',';':'semicolon','(':'shift-9',')':'shift-0',
       '*':'shift-8','%':'shift-5','>':'shift-dot','<':'shift-comma','?':'shift-slash','!':'shift-1','@':'shift-2',
       '#':'shift-3','$':'shift-4','&':'shift-7','+':'shift-equal','|':'shift-backslash','~':'shift-grave_accent',
       '\'':'apostrophe','[':'bracket_left',']':'bracket_right','{':'shift-bracket_left','}':'shift-bracket_right'}
    for ch in sys.argv[2]:
        if ch in m: key(m[ch])
        elif ch.isdigit(): key(ch)
        elif ch.isalpha(): key(('shift-' if ch.isupper() else '')+ch.lower())
        else: print("skip",repr(ch))
    if len(sys.argv)>3 and sys.argv[3]=="ret": key("ret")
elif a=="quit": print(cmd("quit"))
elif a=="status": print(cmd("query-status"))
