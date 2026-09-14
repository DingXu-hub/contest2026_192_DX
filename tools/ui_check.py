#!/usr/bin/env python3
"""ui_check.py - quantitative screenshot inspection for the watch UI.

Prints the dominant colours, a luminance ASCII rendering and samples a few
probe points, so layout/colour regressions can be verified without a
vision model.
"""
import glob
import os
import sys
from collections import Counter

from PIL import Image

OUT = sys.argv[1] if len(sys.argv) > 1 else r'D:\共享文件夹\photos'
FILES = sorted(glob.glob(os.path.join(OUT, 'ui_*.png')))
RAMP = ' .:-=+*#%@'


def ascii_art(im, cols=64, rows=30):
    W, H = im.size
    px = im.load()
    out = []
    for r in range(rows):
        line = ''
        for c in range(cols):
            x = int(c * W / cols)
            y = int(r * H / rows)
            R, G, B = px[x, y]
            lum = (R * 299 + G * 587 + B * 114) // 1000
            line += RAMP[min(9, lum * 10 // 256)]
        out.append(line)
    return out


for f in FILES:
    im = Image.open(f).convert('RGB')
    px = im.load()
    W, H = im.size
    print('== %s  %dx%d  unique=%d' % (os.path.basename(f), W, H,
                                       len(Counter(im.getdata()))))
    for col, n in Counter(im.getdata()).most_common(6):
        print('   #%02X%02X%02X  %d px' % (col[0], col[1], col[2], n))
    for art in ascii_art(im):
        print('   ' + art)
