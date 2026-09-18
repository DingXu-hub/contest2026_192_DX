#!/usr/bin/env python3
"""att_still.py - stillness drift check for the 6-axis attitude estimator.

Polls the device with `!att` while the watch lies untouched, then reports the
heading drift, the roll/pitch spread and how many input glitches the
conditioning stage rejected.  This is the measurement that motivates the spike
filter and the ZUPT/bias handling, so it is the one to re-run after any change
to attitude6.c.

usage: att_still.py [COM4] [seconds]      (default 180 s)
"""
import binascii
import re
import sys
import time

import serial
import serial.tools.list_ports as ports

MAGIC = b'\xaa\x55'
PAD = b'\n' * 2


def crc16(d):
    return binascii.crc_hqx(d, 0xFFFF)


def frame(ftype, ch, seq, payload=b''):
    head = bytes([ftype, ch, seq, len(payload) & 0xff, (len(payload) >> 8) & 0xff])
    return b'\x00\x00\x00\x00' + MAGIC + head + \
        crc16(head + payload).to_bytes(2, 'little') + payload + PAD


def main():
    port = None
    seconds = 180.0
    for a in sys.argv[1:]:
        if re.fullmatch(r'COM\d+', a, re.I):
            port = a
        else:
            try:
                seconds = float(a)
            except ValueError:
                pass
    if port is None:
        for p in ports.comports():
            if 'CH340' in (p.description or '') or 'USB-SERIAL' in (p.description or ''):
                port = p.device
    if port is None:
        print('no CH340 serial port found')
        return 1

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 1000000
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.3)

    seq = 0

    def paced(data):
        for b in data:
            ser.write(bytes([b]))
            ser.flush()
            time.sleep(0.0025)

    def poll():
        nonlocal seq
        seq = (seq + 1) & 0xff
        paced(frame(1, 1, seq, b'!att'))
        buf = b''
        t0 = time.time()
        while time.time() - t0 < 1.5:
            buf += ser.read(4096)
            m = re.search(rb'yaw=([-+0-9.]+) roll=([-+0-9.]+)[^\n]*?'
                          rb'pitch=([-+0-9.]+)[^\n]*?spike=(\d+)', buf)
            if m:
                return (float(m.group(1)), float(m.group(2)),
                        float(m.group(3)), int(m.group(4)))
        return None

    print('--- leave the watch untouched and flat for %.0f s ---' % seconds)
    print('  t(s)    yaw     roll    pitch   spikes')
    t0 = time.time()
    first = prev = None
    samples = []
    while time.time() - t0 < seconds:
        r = poll()
        if r is None:
            print('  (no reply)')
            time.sleep(0.5)
            continue
        elapsed = time.time() - t0
        if first is None:
            first = r
        prev = r
        samples.append((elapsed, r))
        print('  %5.1f  %+6.2f  %+6.2f  %+6.2f   %d'
              % (elapsed, r[0], r[1], r[2], r[3]), flush=True)
        time.sleep(1.0)

    ser.close()
    if len(samples) < 3:
        print('not enough samples')
        return 1

    yaws = [s[1][0] for s in samples]
    rolls = [s[1][1] for s in samples]
    pitches = [s[1][2] for s in samples]
    tail = [s for s in samples if s[0] > samples[0][0] + 10.0]
    print('\n--- summary over %.0f s ---' % (samples[-1][0] - samples[0][0]))
    print('  yaw   first %+.2f  last %+.2f  drift %+.2f deg  range %.2f deg'
          % (yaws[0], yaws[-1], yaws[-1] - yaws[0], max(yaws) - min(yaws)))
    print('  roll  range %.2f deg   pitch range %.2f deg'
          % (max(rolls) - min(rolls), max(pitches) - min(pitches)))
    print('  input glitches rejected by the spike stage: %d'
          % (samples[-1][1][3] - samples[0][1][3]))
    print('  drift per minute: %+.2f deg/min'
          % ((yaws[-1] - yaws[0]) / (samples[-1][0] - samples[0][0]) * 60.0))
    print('\n  for reference, before the spike filter this board drifted')
    print('  about +1.2 deg/min (step-shaped, from 18 dps read glitches).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
