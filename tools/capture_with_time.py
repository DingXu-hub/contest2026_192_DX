#!/usr/bin/env python3
"""capture_with_time.py - devshot capture that also injects SET_TIME/PING.

Opening the port resets the watch (no RTC => clock reads 1970 and the UI
falls back to its demo date).  This script therefore pushes the PC's own
time over the framed link *while* capturing, so the dumped frames show the
real clock and the link indicator.

usage: capture_with_time.py PORT OUTDIR SECONDS
"""
import binascii
import os
import sys
import time

import serial
from PIL import Image

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
OUT = sys.argv[2] if len(sys.argv) > 2 else r'D:\共享文件夹\photos'
DURATION = float(sys.argv[3]) if len(sys.argv) > 3 else 50.0
W, H = 390, 450
HDR = b'P6\n390 450\n255\n'
DATA = W * H * 3
MAGIC = b'\xaa\x55'
PAD = b'\n' * 2


def crc16(d):
    return binascii.crc_hqx(d, 0xFFFF)


def frame(ftype, ch, seq, payload=b''):
    head = bytes([ftype, ch, seq, len(payload) & 0xff, (len(payload) >> 8) & 0xff])
    return MAGIC + head + crc16(head + payload).to_bytes(2, 'little') + payload + PAD


def paced(ser, data):
    """the watch's console RX is one byte deep: pace the writes"""
    for i in range(len(data)):
        ser.write(data[i:i + 1])
        ser.flush()
        time.sleep(0.0012)


ser = serial.Serial()
ser.port = PORT
ser.baudrate = 1000000
ser.timeout = 0.05
ser.dtr = False
ser.rts = False
ser.open()
ser.rts = True
time.sleep(0.3)
ser.rts = False                    # deliberate reset -> deterministic boot

buf = b''
saved = 0
seq = 0
t0 = time.time()
sent = False
while time.time() - t0 < DURATION:
    el = time.time() - t0
    if not sent and el > 9.0:      # app is up by now
        now = time.time()
        seq += 1
        paced(ser, frame(0, 0, seq, bytes([2]) + int(now).to_bytes(4, 'big') +
                         int((now % 1) * 1e6).to_bytes(4, 'big')))
        print('sent SET_TIME at %.1fs' % el, flush=True)
        seq += 1
        paced(ser, frame(0, 0, seq, bytes([1])))     # PING -> gateway present
        print('sent PING', flush=True)
        sent = True

    buf += ser.read(65536)
    if len(buf) > 4_000_000:
        buf = buf[-1_000_000:]
    while True:
        i = buf.find(HDR)
        if i < 0 or len(buf) - i < len(HDR) + DATA:
            break
        raw = buf[i + len(HDR): i + len(HDR) + DATA]
        buf = buf[i + len(HDR) + DATA:]
        Image.frombytes('RGB', (W, H), raw).save(
            os.path.join(OUT, 'ts_%02d.png' % saved))
        print('saved ts_%02d.png at %.1fs' % (saved, time.time() - t0), flush=True)
        saved += 1
ser.close()
print('frames:', saved)
