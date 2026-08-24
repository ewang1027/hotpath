#!/usr/bin/env python3
"""Generate the figures in docs/ as hand-written SVG.

No matplotlib, no numpy -- deliberately. The figures are small enough to write
directly, which keeps them dependency-free, diffable in git, tiny, and rendered
natively by GitHub. Colours are chosen to read on both the light and dark
GitHub themes, since a README image cannot follow prefers-color-scheme.

    ./scripts/make_plots.py            # regenerate everything into docs/img
"""
import csv, math, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "docs", "img")
TAPES = os.environ.get("HOTPATH_TAPE_DIR", os.path.expanduser("~/market-data/tapes"))

FG = "#8b949e"      # axes + labels: legible on white and on #0d1117
GRID = "#8b949e"
ACCENT = "#3178c6"
BAD = "#d1495b"
GOOD = "#2a9d5c"
SERIES = ["#3178c6", "#d1495b", "#e08a1e", "#2a9d5c"]


def esc(t):
    return str(t).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Svg:
    def __init__(self, w, h):
        self.w, self.h, self.parts = w, h, []

    def add(self, s):
        self.parts.append(s)

    def line(self, x1, y1, x2, y2, stroke=GRID, width=1, opacity=1.0, dash=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
                 f'stroke="{stroke}" stroke-width="{width}" opacity="{opacity}"{d}/>')

    def text(self, x, y, s, size=11, anchor="middle", fill=FG, weight="normal"):
        self.add(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" text-anchor="{anchor}" '
                 f'fill="{fill}" font-weight="{weight}" '
                 f'font-family="ui-sans-serif,-apple-system,Segoe UI,Helvetica,Arial,sans-serif">'
                 f'{esc(s)}</text>')

    def circle(self, x, y, r, fill, opacity=1.0):
        self.add(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="{fill}" opacity="{opacity}"/>')

    def polyline(self, pts, stroke, width=2):
        d = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
        self.add(f'<polyline points="{d}" fill="none" stroke="{stroke}" stroke-width="{width}"/>')

    def save(self, name):
        os.makedirs(OUT, exist_ok=True)
        body = "\n".join(self.parts)
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {self.w} {self.h}" '
               f'width="{self.w}" height="{self.h}" role="img">\n{body}\n</svg>\n')
        path = os.path.join(OUT, name)
        open(path, "w").write(svg)
        print("wrote", os.path.relpath(path, ROOT))


def sweep_rows():
    """Run the cross-sectional sweep (or reuse a cached CSV via HOTPATH_SWEEP_CSV)."""
    cached = os.environ.get("HOTPATH_SWEEP_CSV")
    if cached and os.path.exists(cached):
        text = open(cached).read()
    else:
        text = subprocess.run([os.path.join(HERE, "symbol_sweep.sh")],
                              capture_output=True, text=True, check=True).stdout
    return list(csv.DictReader(text.splitlines()))


def pearson(a, b):
    n = len(a); ma = sum(a) / n; mb = sum(b) / n
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    da = math.sqrt(sum((x - ma) ** 2 for x in a))
    db = math.sqrt(sum((y - mb) ** 2 for y in b))
    return num / (da * db)


def place_labels(svg, items, size=9.5, avoid=(), r=6):
    """Greedy non-overlapping label placement.

    Tries a few offsets around each point and takes the first that clears every
    label already placed AND every data marker in `avoid`. Cheap, and enough for
    a couple of dozen points -- the alternative was hand-nudging coordinates,
    which rots the moment the data moves.

    Avoiding the markers matters as much as avoiding other labels: without it a
    label happily lands on top of a neighbouring dot."""
    boxes = [(x - r, y - r, x + r, y + r) for x, y in avoid]
    cands = [(0, -10, "middle"), (0, 16, "middle"), (11, 4, "start"), (-11, 4, "end"),
             (0, -21, "middle"), (0, 27, "middle"), (11, -8, "start"), (-11, -8, "end")]
    for x, y, text, colour in items:
        w = 6.2 * len(text)
        for dx, dy, anchor in cands:
            cx = x + dx
            x0 = cx - (w / 2 if anchor == "middle" else (0 if anchor == "start" else w))
            box = (x0, y + dy - size, x0 + w, y + dy + 2)
            if all(box[2] < b[0] or box[0] > b[2] or box[3] < b[1] or box[1] > b[3]
                   for b in boxes):
                boxes.append(box)
                svg.text(cx, y + dy, text, size=size, anchor=anchor, fill=colour)
                break


def plot_crossover(rows):
    """The signature finding: when a sorted level vector stops being viable."""
    pts = [(r["symbol"], float(r["shifted_per_event"]), float(r["intrusive_vs_map"]))
           for r in rows]
    r = pearson([math.log10(max(p[1], 0.5)) for p in pts], [p[2] for p in pts])

    W, H = 760, 460
    L, R, T, B = 78, 24, 56, 62
    s = Svg(W, H)
    px = lambda v: L + (math.log10(max(v, 0.5)) - math.log10(0.5)) / (math.log10(3000) - math.log10(0.5)) * (W - L - R)
    py = lambda v: T + (1.9 - v) / (1.9 - 0.4) * (H - T - B)

    s.text(L, 24, "A sorted level vector stops paying off once books get deep",
           size=15, anchor="start", weight="600")
    s.text(L, 42, f"25 symbols, one trading day.  Pearson r = {r:+.3f} on log x",
           size=11.5, anchor="start")

    for gv in [0.5, 0.75, 1.0, 1.25, 1.5, 1.75]:
        y = py(gv)
        s.line(L, y, W - R, y, opacity=0.15)
        s.text(L - 8, y + 4, f"{gv:.2f}x", size=10, anchor="end")
    for gv in [1, 10, 100, 1000]:
        x = px(gv)
        s.line(x, T, x, H - B, opacity=0.15)
        s.text(x, H - B + 18, f"{gv:,}", size=10)

    # break-even: above this the intrusive design beats std::map
    y1 = py(1.0)
    s.line(L, y1, W - R, y1, stroke=FG, width=1.5, opacity=0.85, dash="5 4")
    s.text(W - R - 4, y1 - 7, "break-even vs std::map", size=10, anchor="end")

    labels = []
    for name, sh, ratio in pts:
        x, y = px(sh), py(ratio)
        loses = ratio < 1.0
        s.circle(x, y, 5, BAD if loses else GOOD, 0.9)
        if name in ("AMZN", "GOOGL", "TSLA", "AAPL", "FB", "NFLX", "NVDA",
                    "MSFT", "SPY", "QQQ", "INTC", "F", "SIRI", "IWM"):
            labels.append((x, y, name, BAD if loses else GOOD))
    # Label the losers first: they carry the finding, so they get the best slots.
    labels.sort(key=lambda t: (t[3] != BAD, t[0]))
    place_labels(s, labels, avoid=[(px(sh), py(ra)) for _, sh, ra in pts])

    s.text((L + W - R) / 2, H - 16, "vector elements shifted per event   (level churn x book depth, log scale)", size=11.5)
    s.add(f'<text x="18" y="{(T + H - B) / 2:.0f}" font-size="11.5" fill="{FG}" '
          f'text-anchor="middle" transform="rotate(-90 18 {(T + H - B) / 2:.0f})" '
          f'font-family="ui-sans-serif,-apple-system,Segoe UI,Helvetica,Arial,sans-serif">'
          f'intrusive book speedup vs std::map</text>')
    ly = H - B - 34
    s.circle(L + 10, ly, 5, BAD, 0.9)
    s.text(L + 20, ly + 4, "loses to std::map (7 of 25)", size=10, anchor="start", fill=BAD)
    s.circle(L + 10, ly + 18, 5, GOOD, 0.9)
    s.text(L + 20, ly + 22, "beats std::map (18 of 25)", size=10, anchor="start", fill=GOOD)
    s.save("design-crossover.svg")


def latency_rows(sym):
    out = subprocess.run([os.path.join(ROOT, "build", "src", "latency_sweep"),
                          os.path.join(TAPES, f"{sym}.tape"), "--size", "100"],
                         capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == "0":
            ns, rest = 0.0, f[1:]
        elif len(f) >= 2 and f[1] in ("us", "ms"):
            ns, rest = float(f[0]) * (1e3 if f[1] == "us" else 1e6), f[2:]
        else:
            continue
        # A data row is exactly: fills shares stale% swept markout fresh_mo pnl.
        # The trailing "relative to zero latency" summary also starts "1 us",
        # so match on shape rather than on the prefix alone.
        if len(rest) != 7 or not rest[2].endswith("%"):
            continue
        rows.append((ns, float(rest[4])))   # latency ns -> markout bps
    return rows


def plot_latency(syms):
    data = {s: latency_rows(s) for s in syms}
    W, H = 760, 430
    L, R, T, B = 78, 120, 56, 62
    s = Svg(W, H)
    lo, hi = 1e2, 1e7      # 0 is drawn at the left edge as a separate tick
    px = lambda v: L + (math.log10(max(v, lo)) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * (W - L - R)
    allv = [m for rs in data.values() for _, m in rs]
    ymin, ymax = min(allv) * 1.15, 0.02
    py = lambda v: T + (ymax - v) / (ymax - ymin) * (H - T - B)

    s.text(L, 24, "Slower re-quoting makes every fill worse", size=15, anchor="start", weight="600")
    s.text(L, 42, "mean 10s markout per fill; more negative = more adversely selected",
           size=11.5, anchor="start")

    for i in range(0, 8):
        gv = ymin + (ymax - ymin) * i / 7
        y = py(gv)
        s.line(L, y, W - R, y, opacity=0.15)
        s.text(L - 8, y + 4, f"{gv:.2f}", size=10, anchor="end")
    # The leftmost gridline carries the zero-latency point, so it is labelled 0
    # rather than 0.1 us -- nothing is actually measured at 0.1 us.
    for gv, lbl in [(1e2, "0"), (1e3, "1 us"), (1e4, "10 us"),
                    (1e5, "100 us"), (1e6, "1 ms"), (1e7, "10 ms")]:
        x = px(gv)
        s.line(x, T, x, H - B, opacity=0.15)
        s.text(x, H - B + 18, lbl, size=10)
    y0 = py(0.0)
    if T <= y0 <= H - B:
        s.line(L, y0, W - R, y0, stroke=FG, width=1.2, opacity=0.6)

    for i, (sym, rs) in enumerate(data.items()):
        col = SERIES[i % len(SERIES)]
        pts = [(px(ns if ns > 0 else lo), py(m)) for ns, m in rs]
        s.polyline(pts, col, 2)
        for x, y in pts:
            s.circle(x, y, 3, col)
        s.text(W - R + 8, pts[-1][1] + 4, sym, size=11, anchor="start", fill=col, weight="600")

    s.text((L + W - R) / 2, H - 16, "re-quote latency (log scale; leftmost point is zero)", size=11.5)
    s.add(f'<text x="18" y="{(T + H - B) / 2:.0f}" font-size="11.5" fill="{FG}" '
          f'text-anchor="middle" transform="rotate(-90 18 {(T + H - B) / 2:.0f})" '
          f'font-family="ui-sans-serif,-apple-system,Segoe UI,Helvetica,Arial,sans-serif">'
          f'mean 10s markout (bps)</text>')
    s.save("latency-markout.svg")


if __name__ == "__main__":
    rows = sweep_rows()
    plot_crossover(rows)
    plot_latency(["AAPL", "MSFT", "INTC", "SPY"])
