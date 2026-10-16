#!/usr/bin/env python3
"""Gera assets/icon0.png (1024x1024, RGB) do atalho AMPR-LZ4. Só stdlib.
Uso: python3 -I tools/make_icon.py [saida.png]"""
import math, struct, sys, zlib

N = 1024
BLUE = (0x00, 0x70, 0xD1)
BG_TOP, BG_BOT = (0x08, 0x0D, 0x18), (0x12, 0x1F, 0x3A)

def seg(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = 0.0 if t < 0 else 1.0 if t > 1 else t
    return math.hypot(px - ax - t * dx, py - ay - t * dy)

# glifos "LZ4" como segmentos (capsulas) em coordenadas da imagem
T = 30.0                       # meia espessura do traço
Y0, Y1 = 330, 650              # topo/base das letras
SEGS = []
def L(x):  SEGS.extend([(x, Y0, x, Y1), (x, Y1, x + 150, Y1)])
def Z(x):  SEGS.extend([(x, Y0, x + 160, Y0), (x + 160, Y0, x, Y1), (x, Y1, x + 160, Y1)])
def F(x):  SEGS.extend([(x + 110, Y0, x + 110, Y1), (x + 110, Y0, x, Y1 - 90), (x, Y1 - 90, x + 190, Y1 - 90)])
L(177); Z(407); F(657)
# barra de "compressão": setas apontando ao centro
SEGS.extend([(230, 810, 450, 810), (590, 810, 810, 810)])
ARROWS = [(450, 810, 405, 775), (450, 810, 405, 845), (590, 810, 635, 775), (590, 810, 635, 845)]
SEGS.extend(ARROWS)

def clamp(v): return 0.0 if v < 0 else 1.0 if v > 1 else v
def mix(a, b, t): return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))

rows = []
for y in range(N):
    base = mix(BG_TOP, BG_BOT, y / (N - 1))
    row = bytearray([0])
    for x in range(N):
        # brilho azul radial atrás do símbolo
        d = math.hypot(x - 512, (y - 520) * 1.15) / 520.0
        g = clamp(1 - d) ** 1.6 * 0.55
        c = mix(base, BLUE, g)
        # faixa diagonal sutil no canto
        c = mix(c, BLUE, clamp((x + (N - y) - 1500) / 600.0) * 0.18)
        # glifos: cobertura anti-aliased pela distância ao traço mais próximo
        if 80 < x < 950 and 280 < y < 900:
            dm = min(seg(x, y, *s) for s in SEGS)
            cov = clamp(T - dm + 0.5)
            if cov > 0:
                # sombra azul embaixo do traço e branco no centro
                col = mix((255, 255, 255), (170, 215, 255), clamp((y - Y0) / (Y1 - Y0)))
                if y > 700: col = (255, 255, 255)
                c = mix(c, col, cov)
        row += bytes((int(c[0] + .5), int(c[1] + .5), int(c[2] + .5)))
    rows.append(bytes(row))

def chunk(t, d):
    c = struct.pack(">I", len(d)) + t + d
    return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
out = sys.argv[1] if len(sys.argv) > 1 else "assets/icon0.png"
with open(out, "wb") as f:
    f.write(png)
print("escrito", out, len(png), "bytes")
