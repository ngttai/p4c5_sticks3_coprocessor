"""iperf TCP (RX, TX or both) together with `linktest ... tone`; prints throughput, the link result and the S3 log.
Usage: P4_PORT=... S3_PORT=... PC_IP=... WS=<ssid> WP=<password> python3 combo.py [seconds] [rx|tx|both]"""
import os,serial,time,threading,sys,re,socket,os
P4=os.environ['P4_PORT']
S3=os.environ['S3_PORT']
SSID,PASS=os.environ['WS'],os.environ['WP']; PC=os.environ['PC_IP']; DUR=int(sys.argv[1]) if len(sys.argv)>1 else 60
def clean(t): return t.replace(SSID,'****').replace(PASS,'****')
s3log=[]; stop=False
def r3():
    s=serial.Serial(S3,115200,timeout=0.3); s.dtr=False; s.rts=False
    while not stop:
        d=s.read(4096)
        if d: s3log.append(d.decode(errors='replace'))
threading.Thread(target=r3,daemon=True).start()
p=serial.Serial(P4,115200,timeout=0.3,write_timeout=2); p.dtr=False; p.rts=False
def rd(sec,until=None):
    t=time.time(); b=''
    while time.time()-t<sec:
        b+=p.read(4096).decode(errors='replace')
        if until and until in b: break
    return b
def cmd(c): p.write((c+'\r\n').encode())
rd(9); cmd('sta_connect "%s" %s'%(SSID,PASS)); o=rd(20,'IPv4 address'); rd(3)
m=re.findall(r'IPv4 address: (\d+\.\d+\.\d+\.\d+)',o)
if not m: print('NO IP'); sys.exit(1)
IP=m[0]; print('P4 ip',IP,flush=True)
def sink():
    srv=socket.socket(); srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); srv.bind(('0.0.0.0',5001)); srv.listen(1); srv.settimeout(30)
    c,_=srv.accept(); c.settimeout(10)
    try:
        while c.recv(65536): pass
    except Exception: pass
def source():
    time.sleep(2); c=socket.socket(); c.connect((IP,5001)); buf=b'x'*16384; t=time.time()
    try:
        while time.time()-t<DUR: c.send(buf)
    except Exception: pass
    c.close()
def run(label,iperf_cmd,pc_fn):
    cmd('linktest %d tone'%(DUR+12)); time.sleep(2)
    th=threading.Thread(target=pc_fn); th.start()
    cmd(iperf_cmd); o=rd(DUR+25,'iperf exit'); o+=rd(1)
    th.join()
    o+=rd(14,'LINKTEST RESULT'); 
    print('---',label)
    for l in clean(o).splitlines():
        if re.search(r'Mbits|Interval|LINKTEST RESULT',l): print(l)
    sys.stdout.flush()
mode=sys.argv[2] if len(sys.argv)>2 else 'both'
if mode in('rx','both'): run('TCP RX PC->P4 + linktest tone','iperf -s -i 10 -t %d'%(DUR+5),source)
if mode in('tx','both'): run('TCP TX P4->PC + linktest tone','iperf -c %s -i 10 -t %d'%(PC,DUR),sink)
stop=True
t=''.join(s3log)
print('--- S3 audio_fe stats (last lines):')
print('\n'.join(l[:210] for l in t.splitlines() if '5s:' in l or 'cpu:' in l)[-3200:])
print('S3 reboots:',len(re.findall(r'rst:0x|Rebooting',t)))
