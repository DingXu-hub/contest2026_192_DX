#!/usr/bin/env python3
"""link_test.py - exercise the watch's framed link directly.

Sends control pings, framed text commands and reports ACK/RTT/CRC stats,
so the link layer can be verified without the full gateway.

usage: link_test.py [COM4] [--cmd "!status"] [--text "?why"]
"""
import argparse
import binascii
import re
import sys
import time

import serial
import serial.tools.list_ports as list_ports

MAGIC = b'\xaa\x55'
T_CTRL, T_TEXT, T_DATA, T_ACK = 0, 1, 2, 3
CH_CTRL, CH_TEXT = 0, 1
C_PING, C_PONG, C_WHOAMI = 1, 0x82, 4
PAD = b'\n' * 2


def crc16(d):
    return binascii.crc_hqx(d, 0xFFFF)


def frame(ftype, ch, seq, payload=b''):
    head = bytes([ftype, ch, seq, len(payload) & 0xff, (len(payload) >> 8) & 0xff])
    return MAGIC + head + crc16(head + payload).to_bytes(2, 'little') + payload + PAD


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port', nargs='?')
    ap.add_argument('--cmd', action='append', default=[])
    ap.add_argument('--ping', type=int, default=3)
    args = ap.parse_args()

    port = args.port
    if not port:
        for p in list_ports.comports():
            if 'CH340' in (p.description or ''):
                port = p.device
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 1000000
    ser.timeout = 0.05
    ser.dtr = False
    ser.rts = False
    ser.open()
    print('port', port, flush=True)

    sent = {}
    seq = 0

    def paced(data):
        # the watch's console RX is one byte deep: pace the writes
        for i in range(len(data)):
            ser.write(data[i:i + 1]); ser.flush(); time.sleep(0.0012)

    def send(ftype, ch, payload=b''):
        nonlocal seq
        seq = (seq + 1) & 0xFF
        paced(frame(ftype, ch, seq, payload))
        sent[seq] = (time.time(), ftype, ch, payload)
        return seq

    # who am i + pings + commands
    send(T_CTRL, CH_CTRL, bytes([C_WHOAMI]))
    for _ in range(args.ping):
        send(T_CTRL, CH_CTRL, bytes([C_PING]))
        time.sleep(0.25)
    for c in args.cmd:
        send(T_TEXT, CH_TEXT, c.encode())
        time.sleep(0.4)

    acked, rtts, texts = 0, [], []
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < 6:
        buf += ser.read(8192)
        while True:
            i = buf.find(MAGIC)
            if i < 0:
                if buf:
                    s = bytes(buf).decode('utf-8', 'replace')
                    for line in re.sub(r'\x1b\[[0-9;]*m', '', s).splitlines():
                        line = line.strip()
                        if line and not line.startswith('\x00'):
                            print('  log|', line[:120], flush=True)
                    buf.clear()
                break
            if i > 0:
                s = bytes(buf[:i]).decode('utf-8', 'replace')
                for line in re.sub(r'\x1b\[[0-9;]*m', '', s).splitlines():
                    line = line.strip()
                    if line:
                        print('  log|', line[:120], flush=True)
                del buf[:i]
            if len(buf) < 9:
                break
            ftype, ch, fseq = buf[2], buf[3], buf[4]
            ln = buf[5] | (buf[6] << 8)
            crc = buf[7] | (buf[8] << 8)
            if ln > 1024:
                del buf[0]
                continue
            if len(buf) < 9 + ln:
                break
            payload = bytes(buf[9:9 + ln])
            body = bytes([ftype, ch, fseq, buf[5], buf[6]]) + payload
            if crc16(body) != crc:
                print('  !! CRC error', flush=True)
                del buf[0]
                continue
            del buf[:9 + ln]
            if ftype == T_ACK:
                info = sent.pop(fseq, None)
                if info:
                    acked += 1
                    rtts.append((time.time() - info[0]) * 1000)
                    print(f'  ACK seq={fseq} ({info[1]}/{info[2]}) '
                          f'rtt={rtts[-1]:.0f}ms', flush=True)
            elif ftype == T_CTRL and payload[:1] == bytes([C_PONG]):
                up = int.from_bytes(payload[1:5], 'big')
                print(f'  PONG uptime={up}ms', flush=True)
            elif ftype == T_TEXT:
                line = payload.decode('utf-8', 'replace')
                print('  TEXT|', line, flush=True)
                texts.append(line)
            else:
                print(f'  frame t={ftype} ch={ch} {payload[:60]!r}', flush=True)
    ser.close()
    print(f'--- acked {acked}/{len(sent)+acked} frames, '
          f'mean rtt {sum(rtts)/len(rtts):.0f}ms' if rtts else
          f'--- acked {acked}, no rtt', flush=True)
    print('--- texts:', texts, flush=True)


if __name__ == '__main__':
    main()
