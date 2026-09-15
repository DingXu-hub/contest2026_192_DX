#!/usr/bin/env python3
"""mic_test.py - drive the watch's !mic bring-up tool over the framed link.

Paced writes (the console RX is one byte deep) + per-command waits, with
device replies, framed TEXT lines and raw logs printed.

usage: mic_test.py [COM4] "!mic" "!mic poll" ...   (each arg = one command)
"""
import binascii
import sys
import time

try:
    sys.stdout.reconfigure(errors='replace')
except Exception:                 # noqa: BLE001
    pass

import serial
import serial.tools.list_ports as ports

MAGIC = b'\xaa\x55'
PAD = b'\n' * 2


def crc16(d):
    return binascii.crc_hqx(d, 0xFFFF)


def frame(ftype, ch, seq, payload=b''):
    head = bytes([ftype, ch, seq, len(payload) & 0xff, (len(payload) >> 8) & 0xff])
    return MAGIC + head + crc16(head + payload).to_bytes(2, 'little') + payload + PAD


def main():
    args = sys.argv[1:]
    port = None
    cmds = []
    for a in args:
        if a.startswith('!'):
            cmds.append(a)
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
        # the watch console RX is one byte deep: pace the writes
        for i in range(len(data)):
            ser.write(data[i:i + 1])
            ser.flush()
            time.sleep(0.0012)

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

    seq = 0
    for cmd in cmds:
        seq = (seq + 1) & 0xFF
        paced(frame(1, 1, seq, cmd.encode()))
        # long operations need a longer window (poll ~0.4 s, sweep ~11 s)
        wait = 12.0 if 'sweep' in cmd else (2.5 if 'poll' in cmd else 1.5)
        buf = b''
        t0 = time.time()
        while time.time() - t0 < wait:
            buf += ser.read(8192)
        txt = buf.decode('utf-8', 'replace')
        for line in txt.splitlines():
            line = ''.join(c for c in line if c.isprintable())
            if line.strip():
                print('   |', line.encode('ascii', 'replace').decode()[:150],
                      flush=True)
    ser.close()


if __name__ == '__main__':
    main()
