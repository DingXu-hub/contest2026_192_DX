#!/usr/bin/env python3
"""attitude_test.py - physical verification of the 6-axis attitude estimator.

Polls the device with `!att` (framed, paced writes) and prints a table plus a
summary so the numbers can be compared with what the watch was physically
doing.

Protocol (the operator performs these while the script runs):
    t=0..6 s    flat on the desk            -> roll ~ 0, pitch ~ 0
    t=6..12 s   roll right (3 o'clock down) -> roll ~ +90
    t=12..18 s  flat                        -> roll ~ 0 again
    t=18..26 s  rotate clockwise ~90 deg    -> yaw increases by ~+90
    t=26..34 s  flat and still              -> yaw drift over 8 s should be < 3 deg

usage: attitude_test.py [COM4] [seconds]
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
    secs = 40.0
    for a in sys.argv[1:]:
        if a.isdigit():
            secs = float(a)
        else:
            port = a
    if not port:
        for p in ports.comports():
            if 'CH340' in (p.description or ''):
                port = p.device

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 1000000
    ser.timeout = 0.05
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.rts = True
    time.sleep(0.3)
    ser.rts = False

    def paced(data):
        for i in range(len(data)):
            ser.write(data[i:i + 1])
            ser.flush()
            time.sleep(0.0025)

    # wait for the app
    b = b''
    t0 = time.time()
    while time.time() - t0 < 14:
        b += ser.read(8192)
        if b'agent-ready' in b:
            break
    print('app ready:', b'framed link ready' in b, flush=True)
    time.sleep(1.0)
    ser.reset_input_buffer()
    print('--- protocol: flat 6s | roll right 6s | flat 6s | rotate cw 8s | still 8s ---',
          flush=True)

    seq = 0
    samples = []
    t_start = time.time()
    while time.time() - t_start < secs:
        seq = (seq + 1) & 0xFF
        paced(ser, frame(1, 1, seq, b'!att'))
        buf = b''
        t1 = time.time()
        while time.time() - t1 < 0.5:
            buf += ser.read(4096)
        txt = ''.join(c for c in buf.decode('utf-8', 'replace') if c.isprintable())
        m = re.search(r'yaw=([+-][\d.]+) roll=([+-][\d.]+) \(acc ([+-][\d.]+)\) '
                      r'pitch=([+-][\d.]+) \(acc ([+-][\d.]+)\) rate=([+-][\d.]+) '
                      r'dps dt=([\d.]+)', txt)
        if m:
            t = time.time() - t_start
            y, r, ra, p, pa, rate, dt = (float(x) for x in m.groups())
            samples.append((t, y, r, ra, p, pa, rate, dt))
            print('  t=%5.1f  roll %+6.1f (acc %+6.1f)  pitch %+6.1f (acc %+6.1f)  '
                  'yaw %+7.1f  rate %+6.1f  dt %.3f'
                  % (t, r, ra, p, pa, y, rate, dt), flush=True)
    ser.close()

    if not samples:
        print('no samples - is the gateway/relay holding the port?')
        return

    rolls = [s[2] for s in samples]
    pitches = [s[4] for s in samples]
    yaws = [s[1] for s in samples]
    acc_err = [abs(s[2] - s[3]) for s in samples]

    print('\n--- summary ---')
    print('  roll   min %+6.1f  max %+6.1f' % (min(rolls), max(rolls)))
    print('  pitch  min %+6.1f  max %+6.1f' % (min(pitches), max(pitches)))
    print('  yaw    first %+7.1f  last %+7.1f  total %+7.1f'
          % (yaws[0], yaws[-1], yaws[-1] - yaws[0]))
    print('  |filtered - accel-only| roll: mean %.1f deg, max %.1f deg'
          % (sum(acc_err) / len(acc_err), max(acc_err)))
    tail = [s for s in samples if s[0] > samples[-1][0] - 8.0]
    if len(tail) > 3:
        print('  drift over the last %.1f s of stillness: yaw %+.2f deg'
              % (tail[-1][0] - tail[0][0], tail[-1][1] - tail[0][1]))
    print('\nCheck: flat -> roll/pitch ~0 ; right side down -> roll ~ +90 ;'
          '\n       clockwise 90 deg -> yaw + ~90 ; stillness -> tiny drift.')


if __name__ == '__main__':
    main()
