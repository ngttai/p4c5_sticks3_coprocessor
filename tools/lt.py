"""Run commands on the P4 console (e.g. `linktest 20 tone`) and print the result together with the S3 audio_fe lines.
Usage: P4_PORT=... S3_PORT=... python3 lt.py 'linktest 20 tone' 'linktest 10'"""
import os,serial,time,threading,sys,re
P4=os.environ['P4_PORT']
S3=os.environ['S3_PORT']
cmds=sys.argv[1:]
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
rd(9)
for c in cmds:
    p.write((c+'\r\n').encode())
    secs=int(c.split()[1]) if c.startswith('linktest') else 3
    o=rd(secs+8,'LINKTEST RESULT'); o+=rd(1)
    print('>>',c); print('\n'.join(l for l in o.splitlines() if 't=' in l or 'LINKTEST' in l or 'linktest' in l or 'fail' in l.lower()))
stop=True
t=''.join(s3log)
print('--- S3 audio_fe lines:')
print('\n'.join(l[:200] for l in t.splitlines() if 'audio_fe' in l and '5s:' in l)[-1500:])
