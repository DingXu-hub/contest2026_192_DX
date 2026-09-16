#!/usr/bin/env python3
"""fit_mag_sphere.py - least-squares sphere fit for raw magnetometer samples.

Reads a JSON list of raw 20-bit magnetometer samples (objects with rx/ry/rz
counts, as captured by the watch during a 3D tumble) and fits a sphere by
least squares: algebraic Kasa fit for the initial guess, then a
Levenberg-Marquardt refinement on the true geometric residuals
(|sample - centre| - radius). Pure stdlib, so reviewers can reproduce the
calibration in docs/指南针病因分析-2026-09-15.md without dependencies.

usage: fit_mag_sphere.py [json-path] [--sensitivity UT_PER_LSB]
       (default json: docs/evidence/measurements/mag-tilt-109samples.json)
"""
import argparse
import json
import math
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_JSON = REPO_ROOT / 'docs' / 'evidence' / 'measurements' / 'mag-tilt-109samples.json'

# Acceptance gates from docs/指南针病因分析-2026-09-15.md §5:
RADIUS_MIN_UT, RADIUS_MAX_UT = 25.0, 65.0   # Earth's field, global range
RMS_MAX_FRAC = 0.08                          # reject biased fits (near metal)


def load_samples(path):
    """Return [(x, y, z), ...] raw counts from a JSON list of samples."""
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    if not isinstance(data, list):
        raise ValueError('expected a JSON list of samples')
    pts = []
    for i, s in enumerate(data):
        if isinstance(s, dict):
            try:
                pts.append((float(s['rx']), float(s['ry']), float(s['rz'])))
            except KeyError:
                pts.append((float(s['x']), float(s['y']), float(s['z'])))
        elif isinstance(s, (list, tuple)) and len(s) >= 3:
            pts.append((float(s[0]), float(s[1]), float(s[2])))
        else:
            raise ValueError(f'sample #{i}: cannot find rx/ry/rz counts')
    if len(pts) < 4:
        raise ValueError(f'need at least 4 samples, got {len(pts)}')
    return pts


def _solve(a, b):
    """Solve A x = b by Gaussian elimination with partial pivoting."""
    n = len(b)
    a = [row[:] for row in a]
    b = b[:]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(a[r][c]))
        a[c], a[p] = a[p], a[c]
        b[c], b[p] = b[p], b[c]
        if abs(a[c][c]) < 1e-12:
            raise ZeroDivisionError('singular normal equations')
        for r in range(c + 1, n):
            f = a[r][c] / a[c][c]
            for k in range(c, n):
                a[r][k] -= f * a[c][k]
            b[r] -= f * b[c]
    x = [0.0] * n
    for r in range(n - 1, -1, -1):
        x[r] = (b[r] - sum(a[r][k] * x[k] for k in range(r + 1, n))) / a[r][r]
    return x


def fit_algebraic(pts):
    """Kasa fit: minimise (x^2+y^2+z^2 + D x + E y + F z + G)^2."""
    sxx = sxy = sxz = syy = syz = szz = 0.0
    sx = sy = sz = sxr = syr = szr = sr = 0.0
    for x, y, z in pts:
        r2 = x * x + y * y + z * z
        sxx += x * x; sxy += x * y; sxz += x * z
        syy += y * y; syz += y * z; szz += z * z
        sx += x;     sy += y;     sz += z
        sxr += x * r2; syr += y * r2; szr += z * r2; sr += r2
    n = float(len(pts))
    d, e, f, g = _solve(
        [[sxx, sxy, sxz, sx],
         [sxy, syy, syz, sy],
         [sxz, syz, szz, sz],
         [sx,  sy,  sz,  n]],
        [-sxr, -syr, -szr, -sr])
    cx, cy, cz = -d / 2, -e / 2, -f / 2
    radius = math.sqrt((d * d + e * e + f * f) / 4 - g)
    return cx, cy, cz, radius


def fit_geometric(pts, init, max_iter=500, tol=1e-10):
    """Levenberg-Marquardt on geometric residuals |p - c| - R."""
    cx, cy, cz, radius = init
    lam = 1e-3

    def cost(cx, cy, cz, radius):
        return sum((math.sqrt((px - cx) ** 2 + (py - cy) ** 2 + (pz - cz) ** 2) - radius) ** 2
                   for px, py, pz in pts)

    cur = cost(cx, cy, cz, radius)
    for _ in range(max_iter):
        jtj = [[0.0] * 4 for _ in range(4)]
        jtr = [0.0] * 4
        for px, py, pz in pts:
            dx, dy, dz = px - cx, py - cy, pz - cz
            dist = math.sqrt(dx * dx + dy * dy + dz * dz)
            ri = dist - radius
            # d(dist - R)/d(centre) = (centre - p)/dist ; d/dR = -1
            j = (-dx / dist, -dy / dist, -dz / dist, -1.0)
            for a in range(4):
                jtr[a] += j[a] * ri
                for b in range(4):
                    jtj[a][b] += j[a] * j[b]
        improved = False
        for _ in range(50):
            damped = [row[:] for row in jtj]
            for a in range(4):
                damped[a][a] *= (1.0 + lam)
            try:
                delta = _solve(damped, [-v for v in jtr])
            except ZeroDivisionError:
                lam *= 10
                continue
            trial = (cx + delta[0], cy + delta[1], cz + delta[2], radius + delta[3])
            new = cost(*trial)
            if new < cur:
                step = max(abs(v) for v in delta)
                cx, cy, cz, radius = trial
                cur = new
                lam = max(lam / 10, 1e-12)
                improved = True
                break
            lam *= 10
        if not improved or step < tol:
            break
    return cx, cy, cz, radius


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('json', nargs='?', default=str(DEFAULT_JSON),
                    help='JSON list of raw samples (default: %(default)s)')
    ap.add_argument('--sensitivity', type=float, default=0.00625,
                    help='uT per LSB (default: %(default)s for MMC5603NJ 20-bit)')
    args = ap.parse_args()

    pts = load_samples(args.json)
    cx, cy, cz, radius = fit_geometric(pts, fit_algebraic(pts))

    residuals = [math.sqrt((px - cx) ** 2 + (py - cy) ** 2 + (pz - cz) ** 2) - radius
                 for px, py, pz in pts]
    rms = math.sqrt(sum(r * r for r in residuals) / len(residuals))

    sens = args.sensitivity
    radius_ut = radius * sens
    rms_ut = rms * sens
    rms_frac = rms / radius

    print(f'file    : {args.json}')
    print(f'samples : {len(pts)}')
    print(f'centre  : ({cx:+.1f}, {cy:+.1f}, {cz:+.1f}) counts'
          f'   [rounded: {round(cx):+d}, {round(cy):+d}, {round(cz):+d}]')
    print(f'radius  : {radius:.1f} counts = {radius_ut:.2f} uT'
          f'  (sensitivity {sens} uT/LSB)')
    print(f'rms     : {rms:.1f} counts = {rms_ut:.2f} uT'
          f'  ({100 * rms_frac:.2f}% of radius)')
    ok_radius = RADIUS_MIN_UT <= radius_ut <= RADIUS_MAX_UT
    ok_rms = rms_frac <= RMS_MAX_FRAC
    print(f'gates   : radius {RADIUS_MIN_UT:.0f}..{RADIUS_MAX_UT:.0f} uT'
          f' {"PASS" if ok_radius else "FAIL"}'
          f' | rms <= {100 * RMS_MAX_FRAC:.0f}% {"PASS" if ok_rms else "FAIL"}')
    return 0 if (ok_radius and ok_rms) else 1


if __name__ == '__main__':
    sys.exit(main())
