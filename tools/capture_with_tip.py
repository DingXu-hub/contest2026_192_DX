#!/usr/bin/env python3
"""capture_with_tip.py - devshot capture that also injects AI commands.

Resets the board, collects the auto-dumped PPM frames and (optionally)
writes one or more agent commands at given times, so the AI notification
card can be verified in a screenshot.

usage: capture_with_tip.py PORT OUTDIR SECONDS [--tip SECONDS "!tip text"]
"""
import glob
import os
import sys
import time

import serial
from PIL import Image

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
OUT = sys.argv[2] if len(sys.argv) > 2 else r'D:\共享文件夹\photos'
DURATION = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
W, H = 390, 450
HDR = b'P6\n390 450\n255\n'
DATA = W * H * 3

tips = []
rest = sys.argv[4:]
i = 0
while i + 1 < len(rest):
    if rest[i] == '--tip':
        at = float(rest[i + 1])
        text = rest[i + 2] if i + 2 < len(rest) else '!tip hi'
        tips.append((at, text))
        i += 3
    else:
        i += 1
tips.sort()

ser = serial.Serial(PORT, 1000000, timeout=0.2)
ser.rts = True
time.sleep(0.05)
ser.rts = False

buf = b''
saved = 0
t0 = time.time()
next_tip = 0
while time.time() - t0 < DURATION:
    el = time.time() - t0
    while next_tip < len(tips) and el >= tips[next_tip][0]:
        _, text = tips[next_tip]
        ser.write((text + '\n').encode())
        ser.flush()
        print('sent at %.0fs: %s' % (el, text))
        next_tip += 1

    buf += ser.read(65536)
    if len(buf) > 4_000_000:
        buf = buf[-1_000_000:]
    while True:
        idx = buf.find(HDR)
        if idx < 0:
            break
        if len(buf) - idx < len(HDR) + DATA:
            break
        raw = buf[idx + len(HDR): idx + len(HDR) + DATA]
        buf = buf[idx + len(HDR) + DATA:]
        Image.frombytes('RGB', (W, H), raw).save(
            os.path.join(OUT, 'cap_%02d.png' % saved))
        print('saved cap_%02d.png' % saved)
        saved += 1
print('done, %d frames' % saved)
