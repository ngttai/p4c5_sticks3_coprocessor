"""Log the S3 serial output to a file until "ALL DONE" appears (afe_s3 double-talk test, 20 s per round).
Usage: S3_PORT=... python3 cap_dt.py <output_file>"""
import os,sys,serial,time
s=serial.Serial(os.environ['S3_PORT'],115200,timeout=0.3); s.dtr=False; s.rts=False
t=time.time(); OUT=sys.argv[1]; f=open(OUT,'w')
while time.time()-t<1200:
    d=s.read(4096)
    if d:
        f.write(d.decode(errors='replace')); f.flush()
        if 'ALL DONE' in open(OUT).read(): time.sleep(8); break
