"""Đo độ trễ link bằng xung click (`linklat [số_lần]`). S3 phải chạy cp_s3 ở chế độ bypass AFE (đo acoustic) hoặc AUDIO_FE_LINK_ECHO (đo điện).
Dùng: P4_PORT=... python3 lat.py linklat 8"""
import os,serial,time,sys,re
P4=os.environ['P4_PORT']
p=serial.Serial(P4,115200,timeout=0.3,write_timeout=2); p.dtr=False; p.rts=False
def rd(sec,until=None):
    t=time.time(); b=''
    while time.time()-t<sec:
        b+=p.read(4096).decode(errors='replace')
        if until and until in b: break
    return b
rd(9); p.write((' '.join(sys.argv[1:])+'\r\n').encode())
o=rd(40,'LINKLAT RESULT'); o+=rd(1)
print('\n'.join(l for l in o.splitlines() if 'click' in l or 'LINKLAT' in l or 'linklat' in l))
