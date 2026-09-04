# -*- coding: utf-8 -*-
"""didi identity — single source of truth. Every asset derives from one grid."""
import os
D = "svg"

# ── Grid constants (one module system for mark + wordmark) ──────────────
SW   = 11.5          # universal stroke weight
R    = 20            # bowl radius (centre-line)
OUT  = R + SW/2      # 25.75 — bowl outer radius
CY   = 59            # bowl centre y  → baseline at CY+OUT = 84.75
BASE = CY + OUT
ASC  = 15            # ascender centre-line top → visual top ASC-SW/2 = 9.25
XTOP = CY - OUT      # 33.25

GRAD = ('<linearGradient id="{i}" gradientUnits="userSpaceOnUse" '
        'x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}">'
        '<stop offset="0" stop-color="#8A2BE2"/>'
        '<stop offset=".52" stop-color="#6A5ACD"/>'
        '<stop offset="1" stop-color="#478cbf"/></linearGradient>')

def mark(bulb_x, bulb_r, neck_y, neck_w, neck_h, sw=SW, paint="currentColor"):
    """node → pipe → scene-ring + ascender.  Returns body at native coords."""
    return (
      f'<g fill="none" stroke="{paint}" stroke-width="{sw}" stroke-linecap="round">'
      f'<circle cx="58" cy="{CY}" r="{R}"/><path d="M78.5 {ASC} V79"/></g>'
      f'<g fill="{paint}"><circle cx="{bulb_x}" cy="{CY}" r="{bulb_r}"/>'
      f'<rect x="{bulb_x}" y="{neck_y}" width="{neck_w}" height="{neck_h}"/></g>')

MASTER  = dict(bulb_x=20, bulb_r=11.5, neck_y=52.5, neck_w=17, neck_h=13)          # G2
COMPACT = dict(bulb_x=16, bulb_r=12,   neck_y=52,   neck_w=22, neck_h=14, sw=12)   # G3

def svg(vb, body, defs=""):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{vb}" fill="none">'
            f'{defs}{body}</svg>')

def write(name, s):
    open(f"{D}/{name}", "w", encoding="utf-8").write(s)
    print(" ", name)

# ── 1. the mark ────────────────────────────────────────────────────────
write("didi-mark.svg",
      svg("0 0 100 100", f'<g transform="translate(2,3)">{mark(**MASTER)}</g>'))
write("didi-mark-compact.svg",
      svg("0 0 100 100", f'<g transform="translate(2,3)">{mark(**COMPACT)}</g>'))
# gradient runs along the request path: node (violet) → ring (blue)
g = GRAD.format(i="dg", x1=8, y1=72, x2=88, y2=20)
write("didi-mark-gradient.svg",
      svg("0 0 100 100", f'<g transform="translate(2,3)">{mark(**MASTER, paint="url(#dg)")}</g>',
          f"<defs>{g}</defs>"))

# ── 2. the wordmark — same grid, same vocabulary; every i-dot is a node ─
def d_glyph(x):
    bx = x + OUT
    return (f'<circle cx="{bx}" cy="{CY}" r="{R}"/>'
            f'<path d="M{bx+R} {ASC} V79"/>'), x + 2*OUT
def i_glyph(x):
    sx = x + SW/2
    return (f'<path d="M{sx} {XTOP+SW/2} V79"/>',
            f'<circle cx="{sx}" cy="20" r="7"/>'), x + SW

def wordmark(paint="currentColor", exit_paint=None):
    SIDE, pen, strokes, dots = 14, 0, [], []
    for ch in "didi":
        if ch == "d":
            s, pen = d_glyph(pen); strokes.append(s)
        else:
            (s, dot), pen = i_glyph(pen); strokes.append(s); dots.append(dot)
        pen += SIDE
    w = pen - SIDE
    tail = ""
    if exit_paint:                     # last i-dot = the request leaving didi
        tail = f'<g fill="{exit_paint}">{dots.pop()}</g>'
    body = (f'<g fill="none" stroke="{paint}" stroke-width="{SW}" stroke-linecap="round">'
            f'{"".join(strokes)}</g><g fill="{paint}">{"".join(dots)}</g>{tail}')
    return body, w

wm, WMW = wordmark()
write("didi-wordmark.svg",
      svg(f"0 0 {WMW+8} 94", f'<g transform="translate(4,0)">{wm}</g>'))

# ── 3. lockups ─────────────────────────────────────────────────────────
# The mark IS a lowercase d, so setting it beside the wordmark reads "d didi".
# The signature resolves that: the node+pipe feeds the wordmark's OWN first d,
# making mark and wordmark one continuous object instead of two rival glyphs.
LEAD = MASTER["bulb_r"] + (32.25 - MASTER["bulb_x"])   # 23.75 — bulb reach left of bowl

VIOLET, BLUE, MID = "#8A2BE2", "#478cbf", "#6A5ACD"

def signature(paint="currentColor", node_paint=None, exit_paint=None):
    body, w = wordmark(paint, exit_paint)
    bx = MASTER["bulb_x"] - 32.25          # bulb cx relative to first bowl's outer left
    np_ = node_paint or paint
    node = (f'<g fill="{np_}"><circle cx="{bx}" cy="{CY}" r="{MASTER["bulb_r"]}"/>'
            f'<rect x="{bx}" y="{MASTER["neck_y"]}" width="{MASTER["neck_w"]}" '
            f'height="{MASTER["neck_h"]}"/></g>')
    return f'<g transform="translate({LEAD},0)">{node}{body}</g>', LEAD + w + 1.25

sig, SIGW = signature()
write("didi-signature.svg", svg(f"0 0 {SIGW+8} 94", f'<g transform="translate(4,0)">{sig}</g>'))

# Brand signature: MCP violet enters at the node, Godot blue exits at the last
# i-dot. Two coloured dots carry the whole bridge idea; the letters stay clean.
sig_b, _ = signature("currentColor", VIOLET, BLUE)
write("didi-signature-brand.svg",
      svg(f"0 0 {SIGW+8} 94", f'<g transform="translate(4,0)">{sig_b}</g>'))

wm, WMW = wordmark()

# Stacked: mark above wordmark — no stutter, the two read as symbol + name.
MARK_CX = (MASTER["bulb_x"] - MASTER["bulb_r"] + 78.5 + SW/2) / 2      # mark visual centre
write("didi-lockup-stacked.svg",
      svg(f"0 0 {WMW+9} 214",
          f'<g transform="translate(4,0)">'
          f'<g transform="translate({WMW/2 - MARK_CX*1.3},-8) scale(1.3)">{mark(**MASTER)}</g>'
          f'<g transform="translate(0,120)">{wm}</g></g>'))

# ── 4. app icon tile + favicon ─────────────────────────────────────────
g3 = GRAD.format(i="tg", x1=0, y1=512, x2=512, y2=0)
write("didi-icon-rounded.svg",
      svg("0 0 512 512",
          f'<rect width="512" height="512" rx="114" fill="#0E1116"/>'
          f'<g transform="translate(89,87) scale(3.6)">{mark(**MASTER, paint="url(#tg)")}</g>',
          f"<defs>{g3}</defs>"))
write("didi-icon-rounded-light.svg",
      svg("0 0 512 512",
          f'<rect width="512" height="512" rx="114" fill="#F6F7F9"/>'
          f'<g transform="translate(89,87) scale(3.6)">{mark(**MASTER, paint="#3B2E7E")}</g>'))
write("favicon.svg",
      svg("0 0 100 100", f'<g transform="translate(2,3)">{mark(**COMPACT, paint="#6A5ACD")}</g>'))

print(f"  grid: wordmark {WMW} | signature {SIGW} | lead {LEAD}")

# ── 5. social preview + README banner ──────────────────────────────────
INK, MUTE, LINE = "#F6F7F9", "#8A93A3", "#242A34"
FONT = "Segoe UI, Inter, Helvetica Neue, Arial, sans-serif"
MONO = "Cascadia Mono, Consolas, SF Mono, Menlo, monospace"

def constellation(w, h, seed=7, fg=None, k=1.0):
    """Faint node-graph field — the product's own subject, used as texture."""
    import random
    rnd = random.Random(seed)
    pts = [(rnd.uniform(0, w), rnd.uniform(0, h)) for _ in range(26)]
    out = []
    for i, (x, y) in enumerate(pts):
        for x2, y2 in pts[i+1:]:
            if (x-x2)**2 + (y-y2)**2 < 165**2:
                out.append(f'<path d="M{x:.0f} {y:.0f} L{x2:.0f} {y2:.0f}"/>')
    edges = f'<g stroke="{fg or INK}" stroke-width="1" opacity="{0.045*k:.3f}">{"".join(out)}</g>'
    dots = "".join(f'<circle cx="{x:.0f}" cy="{y:.0f}" r="2.5"/>' for x, y in pts)
    return edges + f'<g fill="{fg or INK}" opacity="{0.085*k:.3f}">{dots}</g>'

def banner(w, h, sig_s, sig_y, title_pt, sub_y, M, rule=True, centre=True, dark=True):
    ground = "#0E1116" if dark else "#F6F7F9"
    fg     = INK if dark else "#12151B"
    mute   = MUTE if dark else "#5C6675"
    line   = LINE if dark else "#DFE3EA"
    body, _ = signature(fg, VIOLET, BLUE)
    sw_ = SIGW * sig_s
    sx0 = (w - sw_) / 2 if centre else M
    tx, ty = (w / 2 if centre else M + LEAD * sig_s), sub_y
    sy0 = sig_y
    parts = [
      f'<rect width="{w}" height="{h}" fill="{ground}"/>',
      constellation(w, h, fg=fg, k=1.0 if dark else 0.45),
      f'<g transform="translate({sx0},{sy0}) scale({sig_s})">{body}</g>',
      f'<text x="{tx}" y="{ty}" font-family="{MONO}" font-size="{title_pt}" '
      f'letter-spacing="{title_pt*0.30:.1f}" fill="{mute}" '
      f'text-anchor="{"middle" if centre else "start"}">GODOT-MCP-NATIVE</text>',
    ]
    if rule:
        parts.append(f'<path d="M{M} {h-104} H{w-M}" stroke="{line}" stroke-width="2"/>')
        parts.append(
          f'<text x="{M}" y="{h-60}" font-family="{FONT}" font-size="25" fill="{fg}" '
          f'opacity=".88">Native C++20 MCP server for Godot 4.5+</text>'
          f'<text x="{w-M}" y="{h-60}" text-anchor="end" font-family="{MONO}" font-size="20" '
          f'fill="{mute}">98 tools &#183; local IPC &#183; zero deps</text>')
    return svg(f"0 0 {w} {h}", "".join(parts))

write("social-preview.svg", banner(1280, 640, 2.55, 168, 20, 420, 96))
write("readme-banner.svg",       banner(1280, 300, 1.45, 66, 14, 252, 104, rule=False, centre=False))
write("readme-banner-light.svg", banner(1280, 300, 1.45, 66, 14, 252, 104, rule=False, centre=False, dark=False))
