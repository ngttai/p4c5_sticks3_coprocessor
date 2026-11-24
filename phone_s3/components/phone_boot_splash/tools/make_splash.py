#!/usr/bin/env python3
"""Build splash.bin (boot splash frames) from the Lottie logo boot_logo.json.

Needs: pip install thorvg-python qoi numpy pillow

The logo's "Clockwise brush reveal" mask is drawn wrongly by both rlottie and ThorVG (the ring shows at
once instead of growing). So the ring is rendered without its mask and revealed here by a clockwise
wipe from the top (48 frames, ease out); the other layers render as they are. Frames are blended onto
the splash background, cropped to the logo and stored as QOI (RGB) at 15 frames per second.

splash.bin: "KSPL", u16 version=1, u16 frames, u16 width, u16 height, u16 x, u16 y (crop position in the
480 x 480 logo), u16 frame_ms, u8 bg_r, bg_g, bg_b, pad, then u32 offsets[frames + 1] from the start of
the data, then the QOI files. Little endian.
"""
import copy, json, os, struct, sys, tempfile
import numpy as np, qoi, thorvg_python as tvg

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'boot_logo.json')
OUT = os.path.join(HERE, '..', 'splash.bin')
SIZE = 480
BG = (14, 17, 22)               # COLOR_BG of phone_boot_splash.cpp
REVEAL = 48                     # frames of the ring reveal (30 fps source)
LAST = 62                       # nothing changes after this frame
STEP = 2                        # 30 fps -> 15 fps
CROP = (20, 14, 460, 466)       # x0, y0, x1, y1 around the logo

src = json.load(open(SRC))
tmp = tempfile.mkdtemp()


def only(keep, name):
    d = copy.deepcopy(src)
    d['assets'][0]['layers'] = [l for l in d['assets'][0]['layers'] if keep(l)]
    for l in d['assets'][0]['layers']:
        l.pop('masksProperties', None)
        l['hasMask'] = False
    path = os.path.join(tmp, name + '.json')
    json.dump(d, open(path, 'w'))
    return path


eng = tvg.Engine(threads=4)


def renderer(path):
    c = tvg.SwCanvas(eng)
    c.set_target(SIZE, SIZE)
    a = tvg.LottieAnimation(eng)
    p = a.get_picture()
    p.load(path)
    p.set_size(SIZE, SIZE)
    c.add(p)

    def render(frame):
        a.set_frame(frame)
        c.update()
        c.draw(True)
        c.sync()
        return np.asarray(c.get_pillow().convert('RGBA')).astype(np.float32) / 255
    return render


ring = renderer(only(lambda l: l.get('nm') == 'Brush ring', 'ring'))
rest = renderer(only(lambda l: l.get('nm') != 'Brush ring', 'rest'))

yy, xx = np.mgrid[0:SIZE, 0:SIZE]
ang = (np.degrees(np.arctan2(xx - SIZE / 2, -(yy - SIZE / 2))) + 360) % 360   # 0 at the top, clockwise
soft = np.degrees(1.5 / (np.hypot(xx - SIZE / 2, yy - SIZE / 2) + 1))        # ~1.5 px soft edge


def wipe(frame):
    u = min(1.0, frame / REVEAL)
    p = 1 - (1 - u) ** 3
    return np.clip((p * 360 - ang) / soft + 0.5, 0, 1)


bg = np.array(BG, np.float32) / 255
blobs = []
for f in range(0, LAST + 1, STEP):
    r = ring(f).copy()
    r[:, :, 3] *= wipe(f)
    t = rest(f)
    rgb = bg * (1 - r[:, :, 3:4]) + r[:, :, :3] * r[:, :, 3:4]
    rgb = rgb * (1 - t[:, :, 3:4]) + t[:, :, :3] * t[:, :, 3:4]
    img = (rgb * 255 + 0.5).astype(np.uint8)[CROP[1]:CROP[3], CROP[0]:CROP[2]]
    blobs.append(qoi.encode(np.ascontiguousarray(img)))
offsets = [0]
for b in blobs:
    offsets.append(offsets[-1] + len(b))
head = b'KSPL' + struct.pack('<7H4B', 1, len(blobs), CROP[2] - CROP[0], CROP[3] - CROP[1], CROP[0], CROP[1],
                              1000 * STEP // 30, BG[0], BG[1], BG[2], 0)
head += struct.pack('<%dI' % len(offsets), *offsets)
with open(OUT, 'wb') as f:
    f.write(head + b''.join(blobs))
print('%d frames, %d KB -> %s' % (len(blobs), os.path.getsize(OUT) // 1024, os.path.normpath(OUT)))
