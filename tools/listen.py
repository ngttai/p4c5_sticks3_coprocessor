"""Đọc cổng serial mà không reset board (dtr và rts tắt). Mở cổng bằng công cụ khác (monitor, cat) thường làm reset P4, và qua GPIO50 cả S3.
Dùng: python3 listen.py <cổng> <giây>"""
import serial,sys,time
s=serial.Serial();s.port=sys.argv[1];s.baudrate=115200;s.dtr=False;s.rts=False;s.timeout=1;s.open()
end=time.time()+float(sys.argv[2])
while time.time()<end:
    d=s.read(4096)
    if d: sys.stdout.write(d.decode('utf8','replace'));sys.stdout.flush()
