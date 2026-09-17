#!/usr/bin/env python3
"""skill_test.py - verify the on-device runtime skills over the framed link.

Sends !skills / !skill <name> / a free-text trigger and prints the replies.
usage: skill_test.py [COM4]
"""
import subprocess
import sys

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
cmds = ['!skills', '!skill hydrate', '!skill reload', '天气', '!mic regs']
subprocess.run([sys.executable, __file__.replace('skill_test.py', 'mic_test.py'),
                PORT] + cmds, check=False)
