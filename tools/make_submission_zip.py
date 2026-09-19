#!/usr/bin/env python3
"""make_submission_zip.py - pack the contest upload bundle.

Official rule (from the submission template):
    the source + AI coding logs stay in the team repo (the reviewers clone it),
    everything else goes into ONE zip uploaded on the contest site, named
    <team>-<work>-<repo>.zip

usage: py -3 tools/make_submission_zip.py [--photos DIR] [--video FILE] [--out DIR]
"""
import glob
import os
import sys
import zipfile

REPO = r'D:\共享文件夹\contest2026_192_DX'
NAME = 'DX队-黄山派智能跑步手表-contest2026_192_DX.zip'


def arg(flag, default=None):
    if flag in sys.argv:
        return sys.argv[sys.argv.index(flag) + 1]
    return default


def main():
    out_dir = arg('--out', r'D:\共享文件夹\contest2026_192_DX')
    photos_dir = arg('--photos', r'C:\Users\Administrator\Desktop\参赛材料\照片')
    video = arg('--video', None)
    if video is None:
        vids = []
        for d in (r'C:\Users\Administrator\Desktop\参赛材料',
                  r'C:\Users\Administrator\Desktop', r'D:\共享文件夹'):
            vids += glob.glob(os.path.join(d, '*.mp4')) + glob.glob(os.path.join(d, '*.mov'))
        video = vids[0] if vids else None

    files, missing = [], []

    for pat, label in ((os.path.join(REPO, '技术报告_contest2026_192_DX.pdf'), '技术报告 PDF'),
                       (os.path.join(REPO, '技术报告_contest2026_192_DX.docx'), '技术报告 DOCX'),
                       (os.path.join(REPO, '作品介绍_contest2026_192_DX.pdf'), '作品介绍 PDF')):
        (files.append(pat) if os.path.exists(pat) else missing.append(label))

    if video and os.path.exists(video):
        files.append(video)
    else:
        missing.append('演示视频（<=5 分钟，mp4/mov）')

    shots = sorted(glob.glob(os.path.join(photos_dir, '*.jpg')) +
                   glob.glob(os.path.join(photos_dir, '*.jpeg')) +
                   glob.glob(os.path.join(photos_dir, '*.png')))
    if shots:
        files += shots
    else:
        missing.append('硬件实物多角度照片（放 %s）' % photos_dir)

    out = os.path.join(out_dir, NAME)
    if not missing:
        with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
            for f in files:
                z.write(f, os.path.basename(f))
        print('打包完成: %s  (%.1f MB, %d 个文件)'
              % (out, os.path.getsize(out) / 1e6, len(files)))
    else:
        print('还缺 %d 项，暂不打包:' % len(missing))
        for m in missing:
            print('  -', m)
        print('\n（海报 / 答辩 PPT 是"入围决赛才交"，不在本包内）')
        print('（源码与 AI 日志留在仓库，评审直接 clone，不进 zip）')
    return 0 if not missing else 1


if __name__ == '__main__':
    sys.exit(main())
