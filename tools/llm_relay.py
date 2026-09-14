#!/usr/bin/env python3
"""llm_relay.py - PC-side relay for the on-device AI agent.

The watch runs the agent (tools + proactive engine); this script only
provides the LLM network path over the serial console:

    watch  --@LLMREQ <id> <prompt>-->  this script  --HTTPS-->  DeepSeek
    watch  <--@LLMRESP <id> <text>--  this script

It also lets the operator type commands that are forwarded to the agent:
    ?<question>     ask the agent (LLM)
    !start / !stop / !status / !timer N / !tip / !help

Usage:
    py -3 llm_relay.py [COMx] [api_key]

The key may also come from the DEEPSEEK_API_KEY environment variable.
Nothing is written to disk.
"""
import json
import os
import queue
import sys
import threading
import time
import urllib.request

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print('pyserial required: py -3 -m pip install pyserial')
    sys.exit(1)

PORT = sys.argv[1] if len(sys.argv) > 1 else 'auto'
KEY = sys.argv[2] if len(sys.argv) > 2 else os.environ.get('DEEPSEEK_API_KEY', '')

if PORT.lower() == 'auto' or PORT not in [p.device for p in list_ports.comports()]:
    cands = [p.device for p in list_ports.comports()
             if 'CH340' in (p.description or '')]
    if cands:
        PORT = cands[0]
print('using serial port:', PORT, flush=True)

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 1000000
ser.timeout = 0.2
# CH340's RTS/DTR are wired to the board reset.  pyserial asserts both on
# open(), which reboots the watch (losing a run in progress), so set the
# lines low BEFORE opening; only the explicit --reset path pulses them.
ser.dtr = False
ser.rts = False
ser.open()

if '--reset' in sys.argv:
    ser.rts = True
    time.sleep(0.3)
    ser.rts = False
    time.sleep(0.5)          # let the app come up and open the console

# The board's console UART regularly swallows the first bytes written
# right after the port is opened / the board resets (observed on CH340 +
# SF32LB52), so wake the RX path with a throwaway newline first.  Empty
# lines are ignored by the agent.
def warmup():
    ser.write(b'\n')
    ser.flush()
    time.sleep(0.05)


def wait_ready(idle=1.5, timeout=20.0):
    """Opening the CH340 usually resets the watch.  Wait until the boot
    chatter stops (and ideally until the agent announces itself) before
    sending anything: otherwise the first command races the boot and is
    silently dropped.  When the board was already running the link stays
    quiet and we proceed after `idle`."""
    t0 = time.time()
    seen = b''
    last_rx = time.time()
    time.sleep(0.8)              # let a reset-triggered boot start talking
    while time.time() - t0 < timeout:
        chunk = ser.read(8192)
        if chunk:
            seen += chunk
            last_rx = time.time()
            if b'agent-ready' in seen:
                print('agent ready after %.1fs' % (time.time() - t0),
                      flush=True)
                return True
        elif time.time() - last_rx > idle:
            print('link idle after %.1fs - assuming the watch is up'
                  % (time.time() - t0), flush=True)
            return True
    print('agent did not come up within %.0fs' % timeout, flush=True)
    return False


warmup()
wait_ready()

API_URL = 'https://api.deepseek.com/chat/completions'
MODEL = 'deepseek-chat'


def ask_llm(prompt: str) -> str:
    if not KEY:
        return '(no API key configured)'
    body = json.dumps({
        'model': MODEL,
        'messages': [
            {'role': 'system',
             'content': 'You are an assistant embedded in a running watch. '
                        'Answer in one short line (max 80 characters).'},
            {'role': 'user', 'content': prompt},
        ],
        'max_tokens': 80,
        'temperature': 0.7,
    }).encode()
    req = urllib.request.Request(
        API_URL, data=body,
        headers={'Content-Type': 'application/json',
                 'Authorization': 'Bearer ' + KEY})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data = json.loads(r.read().decode())
        text = data['choices'][0]['message']['content'].strip()
    except Exception as e:
        text = '(LLM error: %s)' % e
    # the device expects a single line
    return ' '.join(text.split())[:110]


out_q = queue.Queue()

# bumped by the reader whenever the device says anything; the writer uses
# it to tell "command accepted" from "command lost on the wire"
dev_activity = [0.0]


def writer():
    retries = 3
    for a in sys.argv:
        if a.startswith('--retries='):
            retries = int(a.split('=', 1)[1])

    while True:
        line = out_q.get()
        if line is None:
            break

        warmup()
        # The board's console UART is lossy for short bursts written from
        # the PC (observed: a single short line vanishes, and a burst can
        # lose its first byte).  Send a throwaway newline in the SAME
        # write plus a small pad to push past the driver's FIFO
        # threshold, then re-send until the device answers - stopping as
        # soon as it does so a command is not executed twice.
        payload = b'\n' + (line + '\n').encode() + b'\n' * 32
        for attempt in range(retries):
            before = dev_activity[0]
            ser.write(payload)
            ser.flush()
            print('-> %s%s' % (line, '' if attempt == 0 else
                               '  (retry %d)' % attempt), flush=True)
            t0 = time.time()
            while time.time() - t0 < 1.2:
                if dev_activity[0] != before:
                    break
                time.sleep(0.05)
            if dev_activity[0] != before:
                break
        else:
            print('   (no reply from the watch - check the cable/port)',
                  flush=True)


threading.Thread(target=writer, daemon=True).start()


def stdin_reader():
    # read bytes and decode as UTF-8: a Windows pipe may hand us GBK text
    # and a decode error would otherwise kill this thread
    for raw in sys.stdin.buffer:
        line = raw.decode('utf-8', 'replace').strip()
        if line:
            out_q.put(line)


threading.Thread(target=stdin_reader, daemon=True).start()

buf = b''
print('relay ready. type ?question or !help (Ctrl-C to quit)', flush=True)
try:
    while True:
        data = ser.read(4096)
        if not data:
            continue
        buf += data
        while b'\n' in buf:
            raw, buf = buf.split(b'\n', 1)
            line = raw.decode('utf-8', 'replace').strip('\r')
            if not line:
                continue
            dev_activity[0] = time.time()      # the device is talking
            if line.startswith('@LLMREQ'):
                parts = line.split(' ', 2)
                rid = parts[1] if len(parts) > 1 else '0'
                prompt = parts[2] if len(parts) > 2 else ''
                print('<- LLMREQ(%s): %s' % (rid, prompt), flush=True)
                answer = ask_llm(prompt)
                print('   answer:', answer, flush=True)
                out_q.put('@LLMRESP %s %s' % (rid, answer))
            else:
                # console log / tool log / proactive events
                print('<-', line, flush=True)
except KeyboardInterrupt:
    pass
finally:
    out_q.put(None)
    time.sleep(0.2)
    ser.close()
    print('relay stopped')
