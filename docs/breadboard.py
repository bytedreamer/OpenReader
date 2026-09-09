# -*- coding: utf-8 -*-
"""Generate docs/breadboard.svg - OpenReader bench build, breadboard view.

Full-size breadboard, 65 columns.  MB102 supply at the left end, ESP32-C6 at
the right, bottom row on columns 57-65 so its USB-C faces off the end of the
board.  The ESP32-C6's headers are 0.7 in apart, so it plugs into rows C and H
and leaves two usable hole rows on each side (A/B below, I/J above).

Waveshare's pinout drawing is a bottom view.  Seen from above with the USB-C to
the right, 5V/GND/3V3/GP0-GP5 are on the BOTTOM row and GP9..TX on the top, so
the RS-485 side and the divider live in the bottom half and the bottom rails
carry 5 V; the RC522 and the sounder hang off the top edge and take 3.3 V from
the top rails.

The power rails run the full length of this board, so the MB102 feeds all four
of them on its own.  One jumper at column 30 ties the two ground rails together.

Every connection is one jumper, routed round the modules rather than straight,
and coloured by the length you would pull from the 14-value kit to make it.
Geometry is on a 16 px = 0.1 in grid.
"""
import io, math, os, re

P, X0, NCOL = 16, 60, 65
def cx(c): return X0 + (c - 1) * P

BX0, BX1, BY0, BY1 = 30, 1114, 288, 656

ROW = {'J': 384, 'I': 400, 'H': 416, 'G': 432, 'F': 448,
       'E': 496, 'D': 512, 'C': 528, 'B': 544, 'A': 560}
RAIL = {('t', '-'): 320, ('t', '+'): 336, ('b', '-'): 608, ('b', '+'): 624}
RAIL_LINE = {('t', '-'): 308, ('t', '+'): 348, ('b', '-'): 596, ('b', '+'): 636}
RAIL_COLS = [c for g in range(11) for c in range(2 + 6 * g, 2 + 6 * g + 5)
             if c <= NCOL]

def h(row, col): return (cx(col), ROW[row])
def rl(half, sign, col): return (cx(col), RAIL[(half, sign)])

KIT = [(1, '2 mm', '#2A2C30', 'black'), (2, '5 mm', '#EDEBE3', 'white'),
       (3, '7 mm', '#9DA0A3', 'grey'), (4, '10 mm', '#8447C4', 'purple'),
       (5, '12 mm', '#2F6BD8', 'blue'), (6, '15 mm', '#1FA34A', 'green'),
       (7, '17 mm', '#E8C317', 'yellow'), (8, '20 mm', '#F0821E', 'orange'),
       (9, '22 mm', '#DE2F2A', 'red'), (10, '25 mm', '#8A5A2B', 'brown'),
       (20, '50 mm', '#2A2C30', 'black'), (30, '75 mm', '#EDEBE3', 'white'),
       (40, '100 mm', '#9DA0A3', 'grey'), (50, '125 mm', '#8447C4', 'purple')]

def kit_for(span):
    for k in KIT:
        if k[0] >= span - 0.02:
            return k
    return KIT[-1]

def darken(c, f=0.42):
    v = c.lstrip('#')
    return '#%02x%02x%02x' % tuple(int(int(v[i:i + 2], 16) * f) for i in (0, 2, 4))

O = io.StringIO()
def w(s): O.write(s + '\n')

MONO = 'ui-monospace, SFMono-Regular, Consolas, monospace'
W, H = 1200, 1000

w('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" '
  'font-family="Inter, Segoe UI, Helvetica, Arial, sans-serif">' % (W, H, W, H))
w('<defs>')
w('<linearGradient id="bb" x1="0" y1="0" x2="0" y2="1">'
  '<stop offset="0" stop-color="#F6F3EB"/><stop offset=".5" stop-color="#EDE9DD"/>'
  '<stop offset="1" stop-color="#E3DED0"/></linearGradient>')
w('<linearGradient id="chan" x1="0" y1="0" x2="0" y2="1">'
  '<stop offset="0" stop-color="#CDC6B4"/><stop offset=".35" stop-color="#E6E1D4"/>'
  '<stop offset="1" stop-color="#F2EFE7"/></linearGradient>')
w('<linearGradient id="lcd" x1="0" y1="0" x2="1" y2="1">'
  '<stop offset="0" stop-color="#1B2430"/><stop offset="1" stop-color="#0A0E14"/></linearGradient>')
w('<filter id="soft" x="-30%" y="-30%" width="160%" height="160%">'
  '<feDropShadow dx="0" dy="1.5" stdDeviation="1.6" flood-opacity="0.30"/></filter>')
# objectBoundingBox filter regions collapse on a straight wire (zero-width or
# zero-height bbox) and the wire vanishes, so give this one the whole canvas.
w('<filter id="lift" filterUnits="userSpaceOnUse" x="0" y="0" width="%d" height="%d">' % (W, H) +
  '<feDropShadow dx="0" dy="2.5" stdDeviation="1.8" flood-opacity="0.28"/></filter>')
w('</defs>')
w('<rect width="%d" height="%d" fill="#FBFAF7"/>' % (W, H))

w('<text x="30" y="40" font-size="21" font-weight="700" fill="#14171A">'
  'OpenReader &#8212; breadboard wiring</text>')
w('<text x="30" y="61" font-size="12.5" fill="#5A6069">'
  'MB102 supply &#183; DSD TECH SH-U12 (RS-485) &#183; SunFounder RC522 '
  '&#183; Waveshare ESP32-C6-LCD-1.47 &#183; 1 k&#8486;/2 k&#8486; divider</text>')

# ------------------------------------------------------------------ the board
w('<g filter="url(#soft)"><rect x="%d" y="%d" width="%d" height="%d" rx="7" fill="url(#bb)" '
  'stroke="#C6BFAC" stroke-width="1.2"/></g>' % (BX0, BY0, BX1 - BX0, BY1 - BY0))
for y in (362, 370, 574, 582):
    w('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#D9D3C2" stroke-width="1"/>'
      % (BX0 + 8, y, BX1 - 8, y))
w('<rect x="%d" y="458" width="%d" height="28" rx="2" fill="url(#chan)"/>' % (BX0 + 8, BX1 - BX0 - 16))
w('<line x1="%d" y1="458" x2="%d" y2="458" stroke="#BCB5A2"/>' % (BX0 + 8, BX1 - 8))
w('<line x1="%d" y1="486" x2="%d" y2="486" stroke="#DAD4C4"/>' % (BX0 + 8, BX1 - 8))

for half in ('t', 'b'):
    for sign, colr in (('+', '#D0342C'), ('-', '#2B5FCB')):
        y = RAIL_LINE[(half, sign)]
        w('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="1.6" opacity=".85"/>'
          % (cx(2) - 14, y, cx(NCOL) + 14, y, colr))
        sym = '+' if sign == '+' else '&#8211;'
        for x in (cx(2) - 23, cx(NCOL) + 23):
            w('<text x="%d" y="%.1f" font-size="13" font-weight="700" fill="%s" text-anchor="middle">'
              '%s</text>' % (x, y + 4.5, colr, sym))

def hole(x, y):
    return ('<rect x="%.1f" y="%.1f" width="9" height="9" rx="1.4" fill="#D6D0BF"/>'
            '<rect x="%.1f" y="%.1f" width="7" height="7" rx="1" fill="#33322F"/>'
            % (x - 4.5, y - 4.5, x - 3.5, y - 3.5))

w('<g>')
for row in ROW:
    for c in range(1, NCOL + 1):
        w(hole(*h(row, c)))
for half in ('t', 'b'):
    for sign in ('+', '-'):
        for c in RAIL_COLS:
            w(hole(*rl(half, sign, c)))
w('</g>')

for row, y in ROW.items():
    for x in (BX0 + 13, BX1 - 13):
        w('<text x="%d" y="%.1f" font-size="9" fill="#8D8778" text-anchor="middle" '
          'font-family="%s">%s</text>' % (x, y + 3.4, MONO, row))
for c in [1] + list(range(5, NCOL + 1, 5)):
    w('<text x="%d" y="358" font-size="8.5" fill="#A69F8D" text-anchor="middle" '
      'font-family="%s">%d</text>' % (cx(c), MONO, c))

for half, sign, txt in (('t', '+', '3.3 V'), ('t', '-', 'GND'),
                        ('b', '+', '5 V'), ('b', '-', 'GND')):
    w('<text x="%d" y="%d" font-size="10.5" font-weight="700" fill="%s" font-family="%s">%s</text>'
      % (BX1 + 10, RAIL[(half, sign)] + 4, '#D0342C' if sign == '+' else '#2B5FCB', MONO, txt))

# ------------------------------------------------------------------- jumpers
# Each entry is one wire: a list of points, bent round whatever is in the way.
JUMPERS, LATE = [], []
def J(net, pts, tag=True, late=False):
    (LATE if late else JUMPERS).append((net, pts, tag))

# --- rails: tie the grounds ----------------------------------------------
J('GND', [rl('b', '-', 30), rl('t', '-', 30)], True, True)

# --- power ---------------------------------------------------------------
J('5V',  [h('A', 65), rl('b', '+', 65)], False, True)     # ESP32-C6 5V
J('GND', [h('B', 64), rl('b', '-', 64)], False, True)     # ESP32-C6 GND
J('5V',  [h('A', 15), rl('b', '+', 15)], False, True)     # SH-U12 VCC
J('GND', [h('A', 18), rl('b', '-', 18)], False, True)     # SH-U12 GND
J('3V3', [h('I', 23), (cx(23), 408), (cx(28), 408),
          rl('t', '+', 28)], False, True)                              # sounder VCC
J('GND', [h('I', 25), (cx(25), 392), (cx(29), 392),
          rl('t', '-', 29)], False, True)                              # sounder GND

# --- bottom bundle: six runs into the ESP32-C6's lower header ------------
J('GP0', [h('B', 17), (cx(17), 552), (cx(62), 552), h('A', 62)])       # SH-U12 RXD
J('GP5', [h('G', 24), (cx(24), 488), (cx(52), 488),
          (cx(52), 560), h('A', 57)])                                   # sounder IO
J('GP1', [h('A', 20), (cx(20), 568), (cx(61), 568), h('A', 61)])       # divider tap
J('GP2', [h('F', 34), (cx(34), 468), (cx(48), 468),
          (cx(48), 576), (cx(60), 576), h('A', 60)])                   # RC522 SCK
J('GP3', [h('F', 35), (cx(35), 478), (cx(47), 478),
          (cx(47), 584), (cx(59), 584), h('A', 59)])                   # RC522 MOSI

# --- RC522, top half -----------------------------------------------------
J('SDA',  [h('F', 33), (cx(33), 460), (cx(45), 460), (cx(45), 368),
           (cx(61), 368), h('J', 61)])
J('RST',  [h('H', 39), (cx(48), 416), (cx(48), 392), (cx(60), 392), h('J', 60)])
J('MISO', [h('G', 36), (cx(46), 432), (cx(46), 380), (cx(59), 380), h('J', 59)])
J('GND',  [h('I', 38), (cx(38), 392), (cx(44), 392), rl('t', '-', 44)], False, True)
J('3V3',  [h('I', 40), (cx(40), 408), (cx(47), 408), rl('t', '+', 47)], False, True)

used = {}
def rpath(pts, r=11):
    if len(pts) < 3:
        return 'M%.1f,%.1f L%.1f,%.1f' % (pts[0][0], pts[0][1], pts[1][0], pts[1][1])
    d = 'M%.1f,%.1f' % pts[0]
    for i in range(1, len(pts) - 1):
        p0, p1, p2 = pts[i - 1], pts[i], pts[i + 1]
        v1 = (p0[0] - p1[0], p0[1] - p1[1]); l1 = math.hypot(*v1) or 1.0
        v2 = (p2[0] - p1[0], p2[1] - p1[1]); l2 = math.hypot(*v2) or 1.0
        rr = min(r, l1 / 2, l2 / 2)
        a = (p1[0] + v1[0] / l1 * rr, p1[1] + v1[1] / l1 * rr)
        b = (p1[0] + v2[0] / l2 * rr, p1[1] + v2[1] / l2 * rr)
        d += ' L%.1f,%.1f Q%.1f,%.1f %.1f,%.1f' % (a[0], a[1], p1[0], p1[1], b[0], b[1])
    d += ' L%.1f,%.1f' % pts[-1]
    return d

def render(items):
    out, tg = [], []
    for net, pts, tag in items:
        run = sum(math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
                  for i in range(len(pts) - 1)) / float(P)
        stock, mm, colr, cname = kit_for(run)
        used[stock] = (mm, colr, cname)
        d = rpath(pts)
        out.append(
            '<g filter="url(#lift)">'
            '<path d="%s" fill="none" stroke="%s" stroke-width="7.4" stroke-linecap="round" stroke-linejoin="round"/>'
            '<path d="%s" fill="none" stroke="%s" stroke-width="5.0" stroke-linecap="round" stroke-linejoin="round"/>'
            '<path d="%s" fill="none" stroke="#FFF" stroke-width="1.1" opacity=".26" stroke-linecap="round" stroke-linejoin="round"/>'
            '</g>' % (d, darken(colr), d, colr, d))
        for px, py in (pts[0], pts[-1]):
            out.append('<circle cx="%.1f" cy="%.1f" r="2.6" fill="#BBBFC4" stroke="#6E7378" '
                       'stroke-width=".7"/>' % (px, py))
        if tag:
            # pill on the midpoint of the longest segment
            best, bl = 0, -1
            for i in range(len(pts) - 1):
                L = math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
                if L > bl: bl, best = L, i
            mx = (pts[best][0] + pts[best + 1][0]) / 2.0
            my = (pts[best][1] + pts[best + 1][1]) / 2.0
            tw = len(net) * 5.7 + 11
            tg.append('<g><rect x="%.1f" y="%.1f" width="%.1f" height="14" rx="7" fill="#FFF" '
                      'stroke="#C7CCD2" stroke-width=".8" opacity=".95"/>'
                      '<text x="%.1f" y="%.1f" font-size="8.5" font-family="%s" fill="#22262B" '
                      'text-anchor="middle">%s</text></g>'
                      % (mx - tw / 2, my - 8, tw, mx, my + 2.4, MONO, net))
    return '\n'.join(out), tg

body_wires, tags_a = render(JUMPERS)
late_wires, tags_b = render(LATE)
w(body_wires)

# ----------------------------------------------------------------- resistors
BROWN, BLACK, RED, GOLD = '#7A4A22', '#1C1C1C', '#C62828', '#C9A227'
def resistor(p1, p2, bands, value, lab_dx=0, lab_dy=-15):
    (x1, y1), (x2, y2) = p1, p2
    ang = math.degrees(math.atan2(y2 - y1, x2 - x1))
    L = math.hypot(x2 - x1, y2 - y1)
    mx, my = (x1 + x2) / 2.0, (y1 + y2) / 2.0
    bl = 40.0
    o = ['<g transform="translate(%.1f,%.1f) rotate(%.2f)" filter="url(#soft)">' % (mx, my, ang),
         '<line x1="%.1f" y1="0" x2="%.1f" y2="0" stroke="#9AA0A6" stroke-width="2.4" '
         'stroke-linecap="round"/>' % (-L / 2, L / 2),
         '<rect x="%.1f" y="-8" width="%.1f" height="16" rx="7" fill="#D9C18E" stroke="#A98F5C" '
         'stroke-width=".9"/>' % (-bl / 2, bl)]
    for i, bc in enumerate(bands):
        bx = -bl / 2 + 6 + i * 7.2
        if i == len(bands) - 1:
            bx = bl / 2 - 8.5
        o.append('<rect x="%.1f" y="-7.4" width="4" height="14.8" fill="%s"/>' % (bx, bc))
    o.append('</g>')
    lx, ly = mx + lab_dx, my + lab_dy
    plain = re.sub(r'&#\d+;', 'X', value)
    pw = len(plain) * 6.0 + 14
    o.append('<rect x="%.1f" y="%.1f" width="%.1f" height="16" rx="8" fill="#FFF" stroke="#C7CCD2" '
             'stroke-width=".9"/>' % (lx - pw / 2, ly - 12, pw))
    o.append('<text x="%.1f" y="%.1f" font-size="10" font-weight="600" font-family="%s" '
             'fill="#22262B" text-anchor="middle">%s</text>' % (lx, ly, MONO, value))
    for px, py in (p1, p2):
        o.append('<circle cx="%.1f" cy="%.1f" r="2.6" fill="#BBBFC4" stroke="#6E7378" '
                 'stroke-width=".7"/>' % (px, py))
    return '\n'.join(o)

w(resistor(h('D', 16), h('D', 20), [BROWN, BLACK, RED, GOLD], '1 k&#8486;', lab_dy=22))
w(resistor(h('C', 20), rl('b', '-', 22), [RED, BLACK, RED, GOLD], '2 k&#8486;', lab_dx=16, lab_dy=80))

# ------------------------------------------------------------------- modules
def pads(cols, row):
    return '\n'.join('<rect x="%.1f" y="%.1f" width="6" height="10" rx="1.5" fill="#C9A227" '
                     'stroke="#8A6E12" stroke-width=".7"/>' % (cx(c) - 3, ROW[row] - 5) for c in cols)

def vlab(cols, names, y, colour='#D9DDE1', size=6.8):
    return '\n'.join('<text transform="translate(%d,%d) rotate(-90)" font-size="%s" '
                     'font-family="%s" fill="%s">%s</text>' % (cx(c), y, size, MONO, colour, n)
                     for c, n in zip(cols, names))

MODULES = []

# --- MB102 breadboard power supply
MX0, MX1, MY0, MY1 = 44, 252, 302, 640
mbpins = []
for half, sign in (('t', '+'), ('t', '-'), ('b', '+'), ('b', '-')):
    x, y = rl(half, sign, 6)
    mbpins.append('<rect x="%.1f" y="%.1f" width="6" height="10" rx="1.5" fill="#C9A227" '
                  'stroke="#8A6E12" stroke-width=".7"/>' % (x - 3, y - 5))
mb = ['<g filter="url(#soft)"><rect x="%d" y="%d" width="%d" height="%d" rx="6" fill="#1E6E3C" '
      'stroke="#124826" stroke-width=".9"/></g>' % (MX0, MY0, MX1 - MX0, MY1 - MY0)] + mbpins + [
      '<rect x="%d" y="400" width="34" height="42" rx="4" fill="#141414" stroke="#000"/>' % (MX0 - 26),
      '<circle cx="%d" cy="421" r="11" fill="#2A2A2A" stroke="#000"/>' % (MX0 - 9),
      '<circle cx="%d" cy="421" r="4" fill="#C9A227"/>' % (MX0 - 9),
      '<text x="%d" y="466" font-size="9" font-family="%s" fill="#5A6069" text-anchor="middle">'
      '7&#8211;12 V</text>' % (MX0 - 8, MONO),
      '<rect x="%d" y="%d" width="26" height="30" rx="2" fill="#B7BCC2" stroke="#7C838A"/>'
      % (MX1 - 42, MY1 - 48),
      '<rect x="%d" y="%d" width="30" height="18" rx="3" fill="#101010" stroke="#000"/>' % (MX0 + 16, MY0 + 16),
      '<rect x="%d" y="%d" width="12" height="12" rx="2" fill="#D9DDE1"/>' % (MX0 + 19, MY0 + 19),
      '<text x="%d" y="%d" font-size="8" font-family="%s" fill="#CFE8D8">ON</text>' % (MX0 + 52, MY0 + 29, MONO)]
for label, jy in (('3.3 V', 356), ('5 V', 566)):
    jx = MX0 + 116
    mb += ['<rect x="%d" y="%d" width="56" height="22" rx="3" fill="#101010" stroke="#000"/>' % (jx, jy),
           '<rect x="%d" y="%d" width="6" height="10" rx="1" fill="#C9A227"/>' % (jx + 10, jy + 6),
           '<rect x="%d" y="%d" width="6" height="10" rx="1" fill="#C9A227"/>' % (jx + 26, jy + 6),
           '<rect x="%d" y="%d" width="6" height="10" rx="1" fill="#C9A227"/>' % (jx + 42, jy + 6),
           '<rect x="%d" y="%d" width="26" height="16" rx="3" fill="#2F6BD8" opacity=".92"/>' % (jx + 6, jy + 3),
           '<text x="%d" y="%d" font-size="9.5" font-weight="700" font-family="%s" fill="#FFF">'
           '%s</text>' % (jx - 60, jy + 16, MONO, label)]
mb += ['<text transform="translate(%d,%d) rotate(-90)" font-size="12" font-weight="700" '
       'font-family="%s" fill="#DDF0E5">MB102</text>' % (MX1 - 18, MY1 - 62, MONO),
       '<text transform="translate(%d,%d) rotate(-90)" font-size="8" font-family="%s" '
       'fill="#A8CEB8">breadboard power supply</text>' % (MX1 - 32, MY1 - 62, MONO)]
MODULES.append(mb)

# --- DSD TECH SH-U12, pins row E cols 17-20, body reaching up
scols = [15, 16, 17, 18]
SX0, SX1, SY0, SY1 = 260, 364, 252, 496
shu = ['<g filter="url(#soft)"><rect x="%d" y="%d" width="%d" height="%d" rx="5" fill="#12508F" '
       'stroke="#0B3A69" stroke-width=".9"/></g>' % (SX0, SY0, SX1 - SX0, SY1 - SY0),
       pads(scols, 'E'),
       '<rect x="%d" y="%d" width="%d" height="30" rx="3" fill="#1F6B3A" stroke="#124826"/>'
       % (SX0 + 8, SY0 + 10, SX1 - SX0 - 16)]
for i, nm in enumerate(['A', 'B', 'G']):
    sx = SX0 + 22 + i * 30
    shu += ['<circle cx="%d" cy="%d" r="6.5" fill="#C8CDD3" stroke="#7C838A"/>' % (sx, SY0 + 25),
            '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#5A6068" stroke-width="1.6"/>'
            % (sx - 4, SY0 + 25, sx + 4, SY0 + 25),
            '<text x="%d" y="%d" font-size="8.5" font-family="%s" fill="#DCE6F2" text-anchor="middle">'
            '%s</text>' % (sx, SY0 + 52, MONO, nm)]
shu += ['<rect x="%d" y="%d" width="52" height="34" rx="2" fill="#141414" stroke="#000"/>' % (SX0 + 26, SY0 + 112),
        '<text x="%d" y="%d" font-size="6.5" font-family="%s" fill="#8E959C" text-anchor="middle">'
        'MAX13487</text>' % (SX0 + 52, SY0 + 132, MONO),
        '<text transform="translate(%d,%d) rotate(-90)" font-size="10" font-family="%s" '
        'fill="#CFE0F2">SH-U12</text>' % (SX0 + 16, SY0 + 200, MONO),
        vlab(scols, ['VCC', 'TXD', 'RXD', 'GND'], SY1 - 8, '#DCE6F2')]
MODULES.append(shu)

# --- active sounder, 3-pin plug-in module, pins row J cols 23-25
bcols = [23, 24, 25]
BZX0, BZX1, BZY0, BZY1 = 378, 478, 272, 384
snd = ['<g filter="url(#soft)"><rect x="%d" y="%d" width="%d" height="%d" rx="5" fill="#1B4F8A" '
       'stroke="#123661" stroke-width=".9"/></g>' % (BZX0, BZY0, BZX1 - BZX0, BZY1 - BZY0),
       pads(bcols, 'J'),
       '<circle cx="%d" cy="%d" r="26" fill="#141414" stroke="#000"/>' % ((BZX0 + BZX1) / 2, BZY0 + 36),
       '<circle cx="%d" cy="%d" r="4.5" fill="#3A3A3A"/>' % ((BZX0 + BZX1) / 2, BZY0 + 36),
       vlab(bcols, ['VCC', 'IO', 'GND'], BZY1 - 6, '#CFE0F2', 8),
       '<text x="%d" y="%d" font-size="8" font-family="%s" fill="#5A6069" text-anchor="middle">'
       'active sounder</text>' % ((BZX0 + BZX1) / 2, BZY0 - 8, MONO)]
MODULES.append(snd)

# --- SunFounder RC522, pins row J cols 35-42, body off the top edge
rcols = list(range(33, 41))
RX0, RX1, RY0, RY1 = 534, 723, 100, 384
rc = ['<g filter="url(#soft)"><rect x="%d" y="%d" width="%d" height="%d" rx="5" fill="#A62C2A" '
      'stroke="#6E1B1A" stroke-width=".9"/></g>' % (RX0, RY0, RX1 - RX0, RY1 - RY0),
      pads(rcols, 'J'),
      '<rect x="%d" y="%d" width="%d" height="146" rx="4" fill="none" stroke="#E8CFCE" '
      'stroke-width="3" opacity=".85"/>' % (RX0 + 16, RY0 + 16, RX1 - RX0 - 32),
      '<rect x="%d" y="%d" width="%d" height="124" rx="3" fill="none" stroke="#E8CFCE" '
      'stroke-width="3" opacity=".55"/>' % (RX0 + 27, RY0 + 27, RX1 - RX0 - 54),
      '<text x="%.1f" y="%d" font-size="9.5" font-family="%s" fill="#F3DAD9" text-anchor="middle">'
      'antenna</text>' % ((RX0 + RX1) / 2.0, RY0 + 94, MONO),
      '<rect x="%d" y="%d" width="46" height="46" rx="3" fill="#151515" stroke="#000"/>' % (RX0 + 50, RY0 + 186),
      '<text x="%d" y="%d" font-size="6.5" font-family="%s" fill="#8E959C" text-anchor="middle">'
      'MFRC522</text>' % (RX0 + 73, RY0 + 212, MONO),
      '<rect x="%d" y="%d" width="30" height="16" rx="3" fill="#B7BCC2" stroke="#7C838A"/>' % (RX0 + 112, RY0 + 194),
      '<text x="%d" y="%d" font-size="10" font-family="%s" fill="#F3DAD9">RC522</text>' % (RX0 + 16, RY0 + 248, MONO),
      vlab(rcols, ['SDA', 'SCK', 'MOSI', 'MISO', 'IRQ', 'GND', 'RST', '3.3V'], RY1 - 8, '#F3DAD9')]
MODULES.append(rc)

# --- Waveshare ESP32-C6-LCD-1.47, seen from above, USB-C off the end
dcols = list(range(57, 66))
DX0, DX1, DY0, DY1 = 911, 1129, 408, 536
dtop = ['GP9', 'GP18', 'GP19', 'GP20', 'GP23', 'GP12', 'GP13', 'RX', 'TX']
dbot = ['GP5', 'GP4', 'GP3', 'GP2', 'GP1', 'GP0', '3V3', 'GND', '5V']
dev = ['<g filter="url(#soft)"><rect x="%d" y="%d" width="%d" height="%d" rx="6" fill="#17181A" '
       'stroke="#000" stroke-width=".8"/></g>' % (DX0, DY0, DX1 - DX0, DY1 - DY0),
       pads(dcols, 'H'), pads(dcols, 'C'),
       '<rect x="%d" y="%d" width="12" height="22" rx="5" fill="#C6CBD1" stroke="#8A9098"/>'
       % (DX1 - 5, (DY0 + DY1) / 2 - 11),
       '<path d="M%d,%d h44" stroke="#5A6069" stroke-width="7" stroke-linecap="round"/>'
       % (DX1 + 7, (DY0 + DY1) / 2),
       '<text x="%d" y="%d" font-size="8" font-family="%s" fill="#7A828A" text-anchor="middle">'
       'USB-C</text>' % (DX1 + 22, DY0 - 8, MONO),
       '<rect x="%d" y="436" width="146" height="62" rx="3" fill="url(#lcd)" stroke="#33383F"/>' % (DX0 + 36),
       '<text x="%d" y="452" font-size="8" font-family="%s" fill="#8FE3A8">OSDP  ONLINE</text>' % (DX0 + 44, MONO),
       '<text x="%d" y="465" font-size="8" font-family="%s" fill="#7FA8D8">9600  ADDR 0</text>' % (DX0 + 44, MONO),
       '<text x="%d" y="478" font-size="8" font-family="%s" fill="#D8C77F">SC  ACTIVE</text>' % (DX0 + 44, MONO),
       '<text x="%d" y="491" font-size="8" font-family="%s" fill="#5C6672">present a card</text>' % (DX0 + 44, MONO),
       '<rect x="%d" y="%d" width="11" height="11" rx="2" fill="#EFF2F5" stroke="#9AA0A6"/>' % (DX0 + 12, DY1 - 18),
       '<circle cx="%.1f" cy="%.1f" r="3" fill="#3DDC6A"/>' % (DX0 + 17.5, DY1 - 12.5),
       vlab(dcols, dtop, DY0 + 26),
       vlab(dcols, dbot, DY1 - 5),
       '<text x="%d" y="%d" font-size="7.5" font-family="%s" fill="#6E757D">ESP32-C6-LCD-1.47</text>'
       % (DX0 + 36, DY0 + 22, MONO)]
MODULES.append(dev)

for part in MODULES:
    w('\n'.join(x for x in part if x))
w(late_wires)
w('\n'.join(tags_a + tags_b))

# ---------------------------------------------------- tamper, flexible leads
def lead(p1, p2, colr, bow=40):
    (x1, y1), (x2, y2) = p1, p2
    return ('<path d="M%.1f,%.1f C%.1f,%.1f %.1f,%.1f %.1f,%.1f" fill="none" stroke="%s" '
            'stroke-width="3.4" stroke-linecap="round"/>'
            '<circle cx="%.1f" cy="%.1f" r="2.4" fill="#BBBFC4" stroke="#6E7378" stroke-width=".6"/>'
            % (x1, y1, x1, y1 + bow, x2, y2 - bow, x2, y2, colr, x2, y2))

TSX, TSY = 990, 700
w('<g filter="url(#soft)"><rect x="%d" y="%d" width="70" height="40" rx="4" fill="#E9ECEF" '
  'stroke="#9AA0A6"/></g>' % (TSX, TSY))
w('<rect x="%d" y="%d" width="52" height="8" rx="4" fill="#B7BCC2" stroke="#7C838A"/>' % (TSX + 9, TSY + 25))
w('<circle cx="%d" cy="%d" r="5" fill="#6E7378"/>' % (TSX + 35, TSY + 13))
w('<text x="%d" y="%d" font-size="8" font-family="%s" fill="#5A6069" text-anchor="middle">'
  'tamper (N.C.) &#8594; GP4</text>' % (TSX + 35, TSY + 54, MONO))
w(lead((TSX + 14, TSY), h('A', 58), '#9DA0A3', -66))
w(lead((TSX + 56, TSY), rl('b', '-', 56), '#2A2C30', -34))

# ---------------------------------------------------------- RS-485 field wires
for dx, colr, ey in ((22, '#1FA34A', 176), (52, '#EDEBE3', 190), (82, '#2A2C30', 204)):
    w('<path d="M%d,%d C%d,%d %d,%d %d,%d" fill="none" stroke="%s" stroke-width="4" '
      'stroke-linecap="round"/>' % (SX0 + dx, SY0 + 8, SX0 + dx, 196, 300, ey - 10, 262, ey, colr))
# centred on the middle field lead, so the three wires run into the box
ACU_Y = (176 + 204) // 2 - 29
w('<rect x="86" y="%d" width="176" height="58" rx="8" fill="#FFF" stroke="#C7CCD2"/>' % ACU_Y)
w('<text x="174" y="%d" font-size="11" font-weight="700" fill="#22262B" text-anchor="middle">'
  'to the ACU</text>' % (ACU_Y + 22))
w('<text x="174" y="%d" font-size="9.5" font-family="%s" fill="#5A6069" text-anchor="middle">'
  'A &#183; B &#183; GND</text>' % (ACU_Y + 39, MONO))
w('<text x="174" y="%d" font-size="8.5" fill="#8A9098" text-anchor="middle">'
  'shielded twisted pair</text>' % (ACU_Y + 52))

# ------------------------------------------------------------------- callouts
def callout(x, y, wd, ht, title, body, accent='#B4700B', bg='#FFF6E5'):
    o = ['<rect x="%d" y="%d" width="%d" height="%d" rx="6" fill="%s" stroke="%s" stroke-width="1"/>'
         % (x, y, wd, ht, bg, accent),
         '<text x="%d" y="%d" font-size="10.5" font-weight="700" fill="%s">%s</text>'
         % (x + 12, y + 20, accent, title)]
    for i, line in enumerate(body):
        o.append('<text x="%d" y="%d" font-size="9.5" fill="#4A4F55">%s</text>' % (x + 12, y + 37 + i * 13, line))
    return '\n'.join(o)

w('<line x1="300" y1="700" x2="352" y2="580" stroke="#B4700B" stroke-width="1.2" stroke-dasharray="4 3"/>')
w(callout(30, 700, 300, 86, '&#9888;  5 V &#8594; 3.3 V divider',
          ['SH-U12 TXD idles at 5 V; the ESP32-C6 is not',
           '5 V tolerant. 1 k&#8486; in series from the TXD column,',
           '2 k&#8486; down to ground, tap along row A to GP1.',
           'Nothing else on the SH-U12 needs shifting.']))
w(callout(346, 700, 222, 86, 'Waveshare pinout',
          ['The wiki drawing is a bottom',
           'view. Seen from above with the',
           'USB-C to the right, 5V/GND/3V3',
           'and GP0&#8211;GP5 are the bottom row.'],
          accent='#5A6069', bg='#F2F4F6'))

# --------------------------------------------------------------------- legend
LY = 820
w('<text x="30" y="%d" font-size="12" font-weight="700" fill="#14171A">'
  'Jumper colour = length (14-value pre-formed kit)</text>' % LY)
for i, (span, mm, colr, cname) in enumerate(KIT):
    x = 30 + (i % 7) * 148
    y = LY + 22 + (i // 7) * 34
    inuse = span in used
    w('<rect x="%d" y="%d" width="34" height="9" rx="4.5" fill="%s" stroke="%s" stroke-width="1.2"/>'
      % (x, y, colr, darken(colr)))
    w('<text x="%d" y="%d" font-size="9.5" font-family="%s" fill="%s">%s</text>'
      % (x + 42, y + 8, MONO, '#22262B' if inuse else '#9AA0A6', mm))
    w('<text x="%d" y="%d" font-size="8.5" font-family="%s" fill="%s">%d hole%s%s</text>'
      % (x, y + 22, MONO, '#5A6069' if inuse else '#B4B9BE', span, '' if span == 1 else 's',
         '  &#183; used' if inuse else ''))
for i, line in enumerate([
        'The kit is length-coded, not signal-coded &#8212; colour tells you which compartment to reach '
        'into, not what the wire carries. Check the code against your own box.',
        'Each connection is one jumper. Solid core bends, so the runs go round the modules rather than '
        'through them; the colour is the length you need for the whole bent path, not the straight line.',
        'Only the tamper switch and the RS-485 field wiring use flexible leads. Every other connection, '
        'the sounder included, is a jumper from the kit.']):
    w('<text x="30" y="%d" font-size="9.5" fill="#5A6069">%s</text>' % (LY + 108 + i * 15, line))

w('</svg>')

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'breadboard.svg')
with open(OUT, 'w', encoding='utf-8') as f:
    f.write(O.getvalue())
print('wrote %s (%d bytes)' % (OUT, len(O.getvalue())))
