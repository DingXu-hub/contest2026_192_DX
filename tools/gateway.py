#!/usr/bin/env python3
"""gateway.py - PC side of the watch's framed serial link (see src/link.h).

What it gives the watch over the single USB serial wire:

  * a lossless, CRC-checked, acknowledged frame link (the CH340 silently
    drops short writes, which is why bare "!status" lines sometimes vanish)
  * a real clock: the gateway pushes its own time at connect, so the watch
    face shows the correct date/time without an RTC
  * the AI text protocol (""?question"", "!status", "@LLMREQ ...") - now
    framed, so commands can no longer be lost
  * an HTTP proxy: "!http <url>" on the watch -> this process fetches the
    URL and sends the status + body back, so the watch can read real data
    from the internet
  * a file channel (reserved) for GPX/firmware transfer

usage:
  py -3 tools/gateway.py [COM4] [--api-key sk-...] [--no-time] [--reset]

Type plain lines on stdin to send a framed text command to the watch.
"""

import argparse
import binascii
import queue
import re
import sys
import threading
import time
import urllib.error
import urllib.request

try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:
    print('pyserial required: pip install pyserial')
    raise SystemExit(1)

MAGIC = b'\xaa\x55'
T_CTRL, T_TEXT, T_DATA, T_ACK = 0, 1, 2, 3
CH_CTRL, CH_TEXT, CH_HTTP, CH_FILE, CH_IP = 0, 1, 2, 3, 4
C_PING, C_PONG, C_SET_TIME, C_TIME_ACK, C_INFO, C_WHOAMI = 1, 0x82, 2, 0x83, 3, 4
PAD = b'\n' * 2   # only a small terminator now that writes are paced
PAYLOAD_MAX = 1024


def crc16(data: bytes) -> int:
    return binascii.crc_hqx(data, 0xFFFF)


def build_frame(ftype: int, ch: int, seq: int, payload: bytes = b'') -> bytes:
    """Wire layout: MAGIC | type ch seq len_lo len_hi | crc_lo crc_hi | payload
    The CRC covers the 5 header bytes *and* the payload (the device checks
    exactly that), while the payload itself follows the CRC field."""
    payload = payload[:PAYLOAD_MAX]
    head = bytes([ftype, ch, seq, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
    return MAGIC + head + crc16(head + payload).to_bytes(2, 'little') + payload + PAD


def parse_frame(buf: bytes):
    """Decode one frame (used by the self-check and by the receiver)."""
    if not buf.startswith(MAGIC) or len(buf) < 9:
        return None
    ftype, ch, seq = buf[2], buf[3], buf[4]
    ln = buf[5] | (buf[6] << 8)
    crc = buf[7] | (buf[8] << 8)
    if len(buf) < 9 + ln:
        return None
    payload = buf[9:9 + ln]
    head = bytes([ftype, ch, seq, buf[5], buf[6]])
    if crc16(head + payload) != crc:
        return None
    return ftype, ch, seq, payload


def selfcheck():
    f = build_frame(T_TEXT, CH_TEXT, 42, b'!link')
    assert parse_frame(f) == (T_TEXT, CH_TEXT, 42, b'!link'), 'frame codec broken'
    return True


class Gateway:
    def __init__(self, port: str, api_key: str = '', set_time: bool = True):
        self.port = port
        self.api_key = api_key
        self.set_time = set_time
        self.seq = 0
        self.rx_buf = bytearray()
        self.out_q = queue.Queue()
        self.pending: dict[int, tuple[float, bytes]] = {}
        self.running = True
        self.dev_seq_seen = 0
        self.rtt_ms = None
        self.log_rx = 0

    # ---------------- serial ----------------
    def open(self):
        ser = serial.Serial()
        ser.port = self.port
        ser.baudrate = 1000000
        ser.timeout = 0.05
        ser.dtr = False
        ser.rts = False
        ser.open()
        self.ser = ser

    def write_paced(self, data: bytes):
        """The watch's console RX path is effectively ONE BYTE DEEP: bytes
        written back-to-back overwrite each other before the app can read
        them (measured: only the last byte of every 4-byte chunk survived,
        and a 63-byte burst arrived as ~15 scattered characters).  Sending
        one byte per millisecond is lossless - verified 63/63 characters."""
        for i in range(len(data)):
            self.ser.write(data[i:i + 1])
            self.ser.flush()
            time.sleep(0.0012)

    def send(self, ftype: int, ch: int, payload: bytes = b'', retry: bool = True):
        self.seq = (self.seq + 1) & 0xFF
        frame = build_frame(ftype, ch, self.seq, payload)
        self.write_paced(frame)
        if retry:
            self.pending[self.seq] = (time.time(), frame)
        return self.seq

    def writer(self):
        while self.running:
            try:
                item = self.out_q.get(timeout=0.1)
            except queue.Empty:
                # retry unacked frames (device is the ACK sender)
                now = time.time()
                for s, (t, frame) in list(self.pending.items()):
                    if now - t > 0.6:
                        self.write_paced(frame)
                        self.pending[s] = (now, frame)
                        if now - t > 1.2:
                            print(f'!! no ACK for seq {s}', flush=True)
                            del self.pending[s]
                continue
            ftype, ch, payload = item
            self.send(ftype, ch, payload)

    # ---------------- channel handlers ----------------
    def handle(self, ftype, ch, seq, payload):
        if ftype == T_ACK:
            info = self.pending.pop(seq, None)
            if info:
                self.rtt_ms = (time.time() - info[0]) * 1000.0
            return
        # acknowledge every good frame
        self.write_paced(build_frame(T_ACK, ch, seq))

        if ftype == T_CTRL:
            if payload[:1] == bytes([C_PONG]) and len(payload) >= 5:
                up = int.from_bytes(payload[1:5], 'big')
                rtt = f'{self.rtt_ms:.0f} ms' if self.rtt_ms else '?'
                print(f'<- PONG uptime={up} ms  rtt={rtt}', flush=True)
            elif payload[:1] == bytes([C_TIME_ACK]):
                print('<- clock synced', flush=True)
            return

        if ftype == T_TEXT:
            line = payload.decode('utf-8', 'replace')
            print('<-', line, flush=True)
            m = re.match(r'@LLMREQ\s+(\d+)\s+(.*?)(?:\s+\(watch:.*\))?$', line)
            if m:
                rid, prompt = m.group(1), m.group(2)
                answer = self.ask_llm(prompt)
                print(f'   answer: {answer}', flush=True)
                self.out_q.put((T_TEXT, CH_TEXT,
                                f'@LLMRESP {rid} {answer}'.encode()))
            return

        if ftype == T_DATA and ch == CH_HTTP:
            req = payload.decode('utf-8', 'replace')
            if req.startswith('GET '):
                url = req[4:].strip()
                print(f'<- HTTP GET {url}', flush=True)
                threading.Thread(target=self.fetch, args=(url,),
                                 daemon=True).start()
            return

        if ftype == T_DATA and ch == CH_FILE:
            print(f'<- file chunk {len(payload)} B', flush=True)
            return

        print(f'<- frame type={ftype} ch={ch} len={len(payload)}', flush=True)

    def fetch(self, url):
        status, body = 0, ''
        try:
            req = urllib.request.Request(url, headers={
                'User-Agent': 'huangshan-watch/1.0'})
            with urllib.request.urlopen(req, timeout=20) as r:
                status = r.status
                body = r.read(3000).decode('utf-8', 'replace')
        except urllib.error.HTTPError as e:
            status = e.code
            body = e.read(500).decode('utf-8', 'replace') if e.fp else str(e)
        except Exception as e:                       # noqa: BLE001
            status = 0
            body = f'error: {e}'
        body = ' '.join(body.split())[:400]
        print(f'   -> HTTP {status} {body[:80]}', flush=True)
        self.out_q.put((T_DATA, CH_HTTP, f'{status} {body}'.encode()))

    def ask_llm(self, prompt):
        if not self.api_key:
            return '(no api key)'
        import json
        data = json.dumps({
            'model': 'deepseek-chat',
            'messages': [
                {'role': 'system',
                 'content': 'You are an assistant inside a running watch. '
                            'Reply in one short line, max 80 characters.'},
                {'role': 'user', 'content': prompt}],
            'max_tokens': 80, 'temperature': 0.7,
        }).encode()
        req = urllib.request.Request(
            'https://api.deepseek.com/chat/completions', data=data,
            headers={'Content-Type': 'application/json',
                     'Authorization': 'Bearer ' + self.api_key})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                text = json.loads(r.read())['choices'][0]['message']['content']
        except Exception as e:                       # noqa: BLE001
            text = f'(LLM error: {e})'
        return ' '.join(text.split())[:110]

    # ---------------- receiver ----------------
    def pump(self):
        buf = self.rx_buf
        while True:
            i = buf.find(MAGIC)
            if i < 0:
                # keep the tail in case the magic is split across reads
                if buf:
                    self.print_logs(bytes(buf[:-1]))
                    del buf[:-1]
                return
            if i > 0:
                self.print_logs(bytes(buf[:i]))
                del buf[:i]
            if len(buf) < 9:
                return
            ftype, ch, seq = buf[2], buf[3], buf[4]
            ln = buf[5] | (buf[6] << 8)
            crc = buf[7] | (buf[8] << 8)
            if ln > PAYLOAD_MAX:
                del buf[0]
                continue
            if len(buf) < 9 + ln:
                return
            payload = bytes(buf[9:9 + ln])
            body = bytes([ftype, ch, seq, buf[5], buf[6]]) + payload
            if crc16(body) != crc:
                del buf[0]
                continue
            del buf[:9 + ln]
            self.handle(ftype, ch, seq, payload)

    def print_logs(self, raw: bytes):
        text = raw.decode('utf-8', 'replace')
        text = re.sub(r'\x1b\[[0-9;]*m', '', text)
        for line in text.splitlines():
            line = line.strip()
            # strip control/unprintable bytes: the bytes between frames are
            # arbitrary (partial frames, padding, escape sequences)
            line = ''.join(c for c in line if c.isprintable())
            if line and not line.startswith('SFBL') and 'P6' not in line:
                print('  |', line.encode('ascii', 'replace').decode(), flush=True)

    def stdin_reader(self):
        for raw in sys.stdin.buffer:
            line = raw.decode('utf-8', 'replace').strip()
            if line:
                self.out_q.put((T_TEXT, CH_TEXT, line.encode()))

    # ---------------- run ----------------
    def run(self):
        self.open()
        if self.set_time:
            time.sleep(0.8)
            now = time.time()
            payload = bytes([C_SET_TIME]) + \
                int(now).to_bytes(4, 'big') + \
                int((now % 1) * 1e6).to_bytes(4, 'big')
            self.send(T_CTRL, CH_CTRL, payload)
            print('-> SET_TIME', flush=True)
        self.send(T_CTRL, CH_CTRL, bytes([C_WHOAMI]))
        self.send(T_CTRL, CH_CTRL, bytes([C_PING]))

        threading.Thread(target=self.writer, daemon=True).start()
        threading.Thread(target=self.stdin_reader, daemon=True).start()
        print('gateway ready - type lines to send framed text to the watch',
              flush=True)
        try:
            while self.running:
                data = self.ser.read(4096)
                if data:
                    self.rx_buf += data
                    self.pump()
        except KeyboardInterrupt:
            pass
        finally:
            self.running = False
            self.ser.close()
            print('gateway stopped')


def pick_port(explicit):
    if explicit:
        return explicit
    for p in list_ports.comports():
        if 'CH340' in (p.description or ''):
            return p.device
    return 'COM4'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port', nargs='?', default=None)
    ap.add_argument('--api-key', default='')
    ap.add_argument('--no-time', action='store_true')
    args = ap.parse_args()
    gw = Gateway(pick_port(args.port), args.api_key, not args.no_time)
    print('using', gw.port, flush=True)
    gw.run()


if __name__ == '__main__':
    selfcheck()
    main()
