"""Ghi log serial của S3 vào file cho đến khi gặp "ALL DONE" (afe_s3, bài double-talk, mỗi vòng 20 s).
Dùng: S3_PORT=... python3 cap_dt.py <file_ra.txt>"""
import os,sys,serial,time
s=serial.Serial(os.environ['S3_PORT'],115200,timeout=0.3); s.dtr=False; s.rts=False
t=time.time(); OUT=sys.argv[1]; f=open(OUT,'w')
while time.time()-t<1200:
    d=s.read(4096)
    if d:
        f.write(d.decode(errors='replace')); f.flush()
        if 'ALL DONE' in open(OUT).read(): time.sleep(8); break
