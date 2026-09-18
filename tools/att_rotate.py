#!/usr/bin/env python3
"""att_rotate.py - operator-guided physical check of the 6-axis estimator.

Prints each step of the protocol while it runs, shows live readings, and grades
every step at the end.  This is the hardware counterpart of `!att test`: the
self-test proves the maths against synthetic truth, this proves the signs and
the frame against a real wrist.

usage: att_rotate.py [COM4]
"""
import binascii
import re
import sys
import time

import serial
import serial.tools.list_ports as ports

MAGIC = b'\xaa\x55'
PAD = b'\n' * 2

# (label, seconds, hint)
STEPS = [
    ('1', 6.0, '平放桌面，手离开          期望 roll≈0  pitch≈0'),
    ('2', 7.0, '把表向右侧翻，3 点朝下    期望 roll≈+90'),
    ('3', 6.0, '平放回桌面                期望 roll 回到 0'),
    ('4', 9.0, '从上往下看，顺时针水平转 90 度   期望 yaw 增加 ≈+90'),
    ('5', 12.0, '平放静止                  期望 yaw 漂移 < 3 度'),
]


def crc16(d):
    return binascii.crc_hqx(d, 0xFFFF)


def frame(ftype, ch, seq, payload=b''):
    head = bytes([ftype, ch, seq, len(payload) & 0xff, (len(payload) >> 8) & 0xff])
    return b'\x00\x00\x00\x00' + MAGIC + head + \
        crc16(head + payload).to_bytes(2, 'little') + payload + PAD


def main():
    wait_mode = '--wait' in sys.argv
    port = None
    for a in sys.argv[1:]:
        if re.fullmatch(r'COM\d+', a, re.I):
            port = a
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
        while time.time() - t0 < 0.6:
            buf += ser.read(4096)
        txt = ''.join(c for c in buf.decode('utf-8', 'replace') if c.isprintable())
        m = re.search(r'yaw=([+-][\d.]+) roll=([+-][\d.]+) \(acc ([+-][\d.]+)\) '
                      r'pitch=([+-][\d.]+) \(acc ([+-][\d.]+)\)', txt)
        if m:
            return tuple(float(x) for x in m.groups())     # y, r, ra, p, pa
        return None

    print('== 6 轴姿态真机复核；脚本会自己判定，你只需按提示动手 ==\n', flush=True)
    time.sleep(2.0)

    if wait_mode:
        input('准备好了按回车开始（每一步会先等你按回车）... ')
        print('')

    phase = []
    for label, secs, hint in STEPS:
        print('步骤 %s (%.0f 秒): %s' % (label, secs, hint), flush=True)
        if wait_mode:
            input('  >>> 摆好姿势后按回车，然后开始 %.0f 秒的动作... ' % secs)
        t_end = time.time() + secs
        vals = []
        while time.time() < t_end:
            r = poll()
            if r is None:
                print('    (无响应)')
                continue
            vals.append(r)
            print('    yaw %+7.1f   roll %+6.1f (acc %+6.1f)   pitch %+6.1f (acc %+6.1f)'
                  % (r[0], r[1], r[2], r[3], r[4]), flush=True)
        phase.append(vals)
        print('', flush=True)
    ser.close()

    def med(v, i):
        s = sorted(x[i] for x in v if x)
        return s[len(s) // 2] if s else float('nan')

    res = []
    v = phase[0]
    res.append(('步骤1 平放 roll/pitch≈0',
                abs(med(v, 1)) < 12 and abs(med(v, 3)) < 12,
                'roll %+.1f pitch %+.1f' % (med(v, 1), med(v, 3))))
    v = phase[1]
    r_max = max((x[1] for x in v if x), default=float('nan'))
    res.append(('步骤2 右翻 roll≈+90', 60 < r_max < 130, 'roll 峰值 %+.1f' % r_max))
    v = phase[2]
    res.append(('步骤3 回平 roll≈0', abs(med(v, 1)) < 12,
                'roll %+.1f' % med(v, 1)))
    a = phase[1][-1] if phase[1] else None
    b = phase[3][-1] if phase[3] else None
    dyaw = (b[0] - a[0]) if (a and b) else float('nan')
    res.append(('步骤4 顺时针 90 度 -> yaw +90', 55 < dyaw < 130,
                'Δyaw %+.1f' % dyaw))
    v = phase[4]
    drift = (v[-1][0] - v[0][0]) if v else float('nan')
    res.append(('步骤5 静止漂移 < 3 度', abs(drift) < 3.0,
                'Δyaw %+.2f' % drift))

    print('== 判定 ==')
    for name, ok, detail in res:
        print('  %s %-34s %s' % ('[PASS]' if ok else '[FAIL]', name, detail))
    print('\n  所有步骤通过即可写入文档: 真机复核 roll 符号、右翻 90 度、'
          '顺时针方向、静止稳定性。')
    return 0 if all(r[1] for r in res) else 1


if __name__ == '__main__':
    sys.exit(main())
