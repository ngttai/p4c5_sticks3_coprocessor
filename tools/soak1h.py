"""Soak: TCP RX rồi TX (mỗi nửa HALF giây, mặc định 1800) cùng link I2S và AFE.
Dùng: P4_PORT=... S3_PORT=... PC_IP=... WS=<ssid> WP=<mật khẩu> python3 soak1h.py [giây_mỗi_nửa]
Cần PC cùng mạng. Kết quả: $OUT_DIR/soak1h_summary.txt (mặc định thư mục tạm). Tên và mật khẩu WiFi được che trong log."""
import os,serial,time,threading,sys,re,socket,os,statistics
P4=os.environ['P4_PORT']
S3=os.environ['S3_PORT']
SSID,PASS=os.environ['WS'],os.environ['WP']; PC=os.environ['PC_IP']; HALF=int(sys.argv[1]) if len(sys.argv)>1 else 1800
def clean(t): return t.replace(SSID,'****').replace(PASS,'****')
s3=[]; p4=[]; stop=False
def r3():
    s=serial.Serial(S3,115200,timeout=0.3); s.dtr=False; s.rts=False
    while not stop:
        d=s.read(4096)
        if d: s3.append(d.decode(errors='replace'))
threading.Thread(target=r3,daemon=True).start()
p=serial.Serial(P4,115200,timeout=0.3,write_timeout=2); p.dtr=False; p.rts=False
def rd(sec,until=None):
    t=time.time(); b=''
    while time.time()-t<sec:
        x=p.read(4096).decode(errors='replace'); b+=x; p4.append(x)
        if until and until in b: break
    return b
def cmd(c): p.write((c+'\r\n').encode())
rd(9); cmd('sta_connect "%s" %s'%(SSID,PASS)); o=rd(20,'IPv4 address'); rd(3)
m=re.findall(r'IPv4 address: (\d+\.\d+\.\d+\.\d+)',o)
if not m: print('NO IP'); sys.exit(1)
IP=m[0]
def sink():
    srv=socket.socket(); srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); srv.bind(('0.0.0.0',5001)); srv.listen(1); srv.settimeout(60)
    c,_=srv.accept(); c.settimeout(20)
    try:
        while c.recv(65536): pass
    except Exception: pass
def source():
    time.sleep(2); c=socket.socket(); c.connect((IP,5001)); buf=b'x'*16384; t=time.time()
    try:
        while time.time()-t<HALF: c.send(buf)
    except Exception: pass
    c.close()
res={}
def run(label,iperf_cmd,pc_fn):
    cmd('linktest %d'%(HALF+15)); time.sleep(2)   # playback im lặng: link được kiểm, loa không phát tiếng
    th=threading.Thread(target=pc_fn); th.start()
    cmd(iperf_cmd); o=rd(HALF+40,'iperf exit'); o+=rd(2)
    th.join(); o+=rd(20,'LINKTEST RESULT')
    iv=[float(x) for x in re.findall(r'^\s*\d+\.\d+-\s*\d+\.\d+ sec\s+([\d.]+) Mbits',clean(o),re.M)]
    iv=iv[:-1] if len(iv)>3 else iv
    lt=re.findall(r'LINKTEST RESULT: (.*)',o)
    res[label]=(iv,lt)
run('TCP RX PC->P4','iperf -s -i 10 -t %d'%(HALF+5),source)
time.sleep(5)
run('TCP TX P4->PC','iperf -c %s -i 10 -t %d'%(PC,HALF),sink)
stop=True
t=''.join(s3); q=clean(''.join(p4))
out=[]
for k,(iv,lt) in res.items():
    if iv: out.append('%s: %d intervals(10s) avg=%.2f min=%.2f max=%.2f Mbps, stalls(<1Mbps)=%d | %s'%(k,len(iv),statistics.mean(iv),min(iv),max(iv),sum(1 for x in iv if x<1),lt[-1] if lt else 'no LINKTEST result'))
    else: out.append(k+': no iperf data')
mins=[int(x) for x in re.findall(r'min=(\d+)',t) if True]
cpu=re.findall(r'cpu: core0=([\d.]+)% core1=([\d.]+)%',t)
lag=re.findall(r'avg=([\d.]+) ms max=([\d.]+) ms',t)
out.append('S3 reboots (rst:/Rebooting after start): %d'%len(re.findall(r'Rebooting|rst:0x',t)))
out.append('S3 error lines: %s'%sorted(set(re.findall(r'^E \(\d+\) (\S+)',t,re.M))))
out.append('S3 internal min free: first=%s last=%s lowest=%s KB'%(mins[0]//1024 if mins else '?',mins[-1]//1024 if mins else '?',min(mins)//1024 if mins else '?'))
if cpu: out.append('S3 CPU core0 avg=%.1f max=%.1f | core1 avg=%.1f max=%.1f'%(statistics.mean(float(a) for a,b in cpu),max(float(a) for a,b in cpu),statistics.mean(float(b) for a,b in cpu),max(float(b) for a,b in cpu)))
if lag: out.append('AFE lag avg of avgs=%.1f ms, worst max=%.1f ms'%(statistics.mean(float(a) for a,b in lag),max(float(b) for a,b in lag)))
out.append('tx_underrun/rx_overrun/tx_drop last: '+(re.findall(r'(tx_underrun=\d+ tx_drop=\d+ rx_overrun=\d+ rx_underrun=\d+)',t) or ['?'])[-1])
out.append('P4 problem lines: %s'%len(re.findall(r'TRANSPORT_FAILURE|transport failure|queue full|Guru|abort\(\)|ESP_ERROR_CHECK failed',q,re.I)))
open(os.path.join(os.environ.get('OUT_DIR','/tmp'),'soak1h_summary.txt'),'w').write('\n'.join(out))
open(os.path.join(os.environ.get('OUT_DIR','/tmp'),'soak1h_s3_tail.txt'),'w').write('\n'.join(t.splitlines()[-120:]))
print('\n'.join(out))
