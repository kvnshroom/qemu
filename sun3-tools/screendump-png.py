#!/usr/bin/env python3
"""shot.py SOCK NAME -- HMP screendump to /tmp/m38/NAME.ppm, convert to /tmp/m38/NAME.png
(pure python), print numeric stats: size, black pixels, ink rows/cols bounding box."""
import socket, sys, time, zlib, struct, os
sock, name = sys.argv[1], sys.argv[2]
ppm = f'/tmp/m38/{name}.ppm'; png = f'/tmp/m38/{name}.png'
try: os.unlink(ppm)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX); s.connect(sock); s.settimeout(0.3)
try: s.recv(4096)
except socket.timeout: pass
s.sendall(f'screendump {ppm}\n'.encode())
for _ in range(100):
    time.sleep(0.1)
    if os.path.exists(ppm) and os.path.getsize(ppm) > 100: break
time.sleep(0.3)
s.close()
data = open(ppm, 'rb').read()
# parse P6
parts = data.split(maxsplit=4)
assert parts[0] == b'P6', parts[0]
w, h, mx = int(parts[1]), int(parts[2]), int(parts[3])
hdr_len = len(data) - w * h * 3
pix = data[hdr_len:]
raw = b''.join(b'\x00' + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
out = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
open(png, 'wb').write(out)
# stats
black = 0; rows = set(); cols = set()
row_black = []
for y in range(h):
    line = pix[y * w * 3:(y + 1) * w * 3]
    nb = 0
    for x in range(w):
        if line[x * 3] < 128:
            nb += 1
    if nb:
        rows.add(y)
    row_black.append(nb)
    black += nb
ink_rows = sorted(rows)
print(f'{png}: {w}x{h} black={black} ({100.0*black/(w*h):.1f}%) ink rows={len(ink_rows)} range={ink_rows[0] if ink_rows else None}..{ink_rows[-1] if ink_rows else None}')
os.unlink(ppm)
