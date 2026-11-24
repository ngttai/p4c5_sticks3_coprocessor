"""Read a serial port without resetting the board (dtr and rts off). Tools that toggle DTR/RTS when opening the port (monitor, flash) can reset the P4, and through GPIO50 the S3 as well.
Usage: python3 listen.py <port> <seconds>"""
import serial,sys,time
s=serial.Serial();s.port=sys.argv[1];s.baudrate=115200;s.dtr=False;s.rts=False;s.timeout=1;s.open()
end=time.time()+float(sys.argv[2])
while time.time()<end:
    d=s.read(4096)
    if d: sys.stdout.write(d.decode('utf8','replace'));sys.stdout.flush()
