#!/usr/bin/env python3
"""Builds the table that tells the PS Vita port which HD image replaces which
background or mask.

Classic REbirth texture packs (TeamX's Resident Evil HD mod, the Seamless HD
Project) name every image by a CRC32 their PC loader takes from the texture
as it sits in the game's DirectDraw surface. The Vita never has that surface,
so instead every room background (rcSRRC.pak) of the game is decoded and
matched against the pack's images by picture content, and each camera's mask
sheet (osp0SRRC.pak) is placed from the pack's own per-room cache lists. The
result is a plain list of names and hashes, no image data:

    rc1000 DD17F52F
    osp00001 4F09A1C2

usage: hd_map.py <game tree: the folder with Stage1..7, Objspr>
                 <hires folder> [<hires folder> ...] > hd_map.txt

Needs Pillow (with WebP) and numpy. The port ships the table it made from the
GOG release and both packs as vita/hd_map.txt.
"""
import os
import sys

import numpy as np
from PIL import Image


def unpak(src):
    """LZW, as unpack_pakfile_ (src/game/FileLoader.cpp)."""
    out = bytearray()
    pos = 0
    mask = 0x80
    cur = 0

    def rd(n):
        nonlocal pos, mask, cur
        r = 0
        bit = 1 << (n - 1)
        while bit:
            if mask == 0x80:
                cur = src[pos]
                pos += 1
            if cur & mask:
                r |= bit
            mask >>= 1
            bit >>= 1
            if mask == 0:
                mask = 0x80
        return r

    while True:
        prefix = {}
        ch = {}
        nxt = 0x103
        cs = 9
        c = rd(cs)
        if c == 0x100:
            return bytes(out)
        out.append(c)
        prev = c
        pfc = c
        while True:
            c = rd(cs)
            if c == 0x100:
                return bytes(out)
            if c == 0x102:
                break
            if c == 0x101:
                cs += 1
                continue
            buf = []
            look = c
            if nxt <= c:
                buf.append(pfc)
                look = prev
            code = look
            while code > 0xFF:
                buf.append(ch[code])
                code = prefix[code]
            buf.append(code)
            pfc = buf[-1]
            out.extend(reversed(buf))
            prefix[nxt] = prev
            ch[nxt] = pfc
            nxt += 1
            prev = c


def tim_rgb(data):
    """First image of a TIM as an HxWx3 float array (4, 8, 16 bpp)."""
    flags = int.from_bytes(data[4:8], "little")
    bpp = flags & 3
    off = 8
    clut = None
    if flags & 8:
        n = int.from_bytes(data[off:off + 4], "little")
        cw = int.from_bytes(data[off + 8:off + 10], "little")
        ch = int.from_bytes(data[off + 10:off + 12], "little")
        clut = np.frombuffer(data[off + 12:off + 12 + cw * ch * 2], dtype="<u2")
        off += n
    w16 = int.from_bytes(data[off + 8:off + 10], "little")
    h = int.from_bytes(data[off + 10:off + 12], "little")
    raw = np.frombuffer(data[off + 12:off + 12 + w16 * h * 2], dtype="<u2").reshape(h, w16)
    if bpp == 2:
        px = raw
    elif bpp == 1:
        b = raw.view(np.uint8).reshape(h, w16 * 2)
        px = clut[b]
    else:
        b = raw.view(np.uint8).reshape(h, w16 * 2)
        idx = np.stack([b & 15, b >> 4], -1).reshape(h, w16 * 4)
        px = clut[idx]
    px = px.astype(np.int32)
    rgb = np.stack([(px & 31) << 3, ((px >> 5) & 31) << 3, ((px >> 10) & 31) << 3], -1)
    return rgb.astype(np.float32)


def small(img, size):
    return np.asarray(img.convert("RGB").resize(size, Image.BILINEAR)).astype(np.float32)


def main():
    game = sys.argv[1]
    hires = sys.argv[2:]

    def find(dirname):
        for d in os.listdir(game):
            if d.lower() == dirname:
                return os.path.join(game, d)
        return None

    # Pack images, by kind.
    pack = {"bgd": {}, "mask": {}}
    for root in hires:
        for kind in pack:
            d = os.path.join(root, kind)
            if not os.path.isdir(d):
                continue
            for f in os.listdir(d):
                if f.lower().endswith(".webp"):
                    pack[kind][f[:8].upper()] = os.path.join(d, f)

    out = []

    # Backgrounds: 320x240 pictures against 1280x960 ones.
    names = sorted(pack["bgd"])
    if names:
        hd = np.stack([small(Image.open(pack["bgd"][n]), (80, 60)) for n in names])
        for s in range(1, 8):
            stage = find("stage%d" % s)
            if stage is None:
                continue
            for f in sorted(os.listdir(stage)):
                if not (f.lower().startswith("rc") and f.lower().endswith(".pak")):
                    continue
                t = unpak(open(os.path.join(stage, f), "rb").read())
                img = tim_rgb(t)
                if img.shape[:2] != (240, 320):
                    continue
                lo = small(Image.fromarray(img.astype(np.uint8)), (80, 60))
                err = ((hd - lo) ** 2).mean(axis=(1, 2, 3))
                best = int(np.argmin(err))
                second = float(np.partition(err, 1)[1]) if len(err) > 1 else 1e9
                # A real pair is an upscale of the same picture; anything near
                # the runner-up is a different room that happens to look alike.
                if err[best] < 600 and err[best] * 2.5 < second:
                    out.append("%s %s" % (f[:-4].lower(), names[best]))

    # Masks: the sheet of cut-outs each camera draws over the background.
    # They are mostly black, so pictures alone tell them apart badly. The
    # pack's own hires/cache/ROOMsrr0.dat lists, per room, the textures its PC
    # loader saw in the order it saw them, and a camera's mask sheet comes
    # right before that camera's background. With the backgrounds placed above,
    # that pins every mask to its osp0SRRC.pak exactly.
    bg_of = {}
    for line in out:
        name, h = line.split()
        if name.startswith("rc"):
            bg_of.setdefault(h, name)
    masks = set(pack["mask"])
    seen = set()
    for root in hires:
        cache = os.path.join(root, "cache")
        if not os.path.isdir(cache):
            continue
        for f in sorted(os.listdir(cache)):
            data = open(os.path.join(cache, f), "rb").read()
            hashes = ["%08X" % int.from_bytes(data[i:i + 4], "little")
                      for i in range(0, len(data) - 3, 4)]
            for i in range(len(hashes) - 1):
                m, bg = hashes[i], hashes[i + 1]
                if m not in masks or bg not in bg_of:
                    continue
                rc = bg_of[bg]                  # rcSRRC: stage 1-7, room hex, camera
                stage = int(rc[2]) - 1
                art = stage - 5 if stage > 4 else stage
                room = int(rc[3:5], 16)
                cam = rc[5:]
                osp = "osp0%d%02d%s" % (art, room, cam)
                if osp not in seen:
                    seen.add(osp)
                    out.append("%s %s" % (osp, m))

    print("# hd_map.txt - game file -> Classic REbirth texture pack hash")
    print("# made by tools/vita/hd_map.py")
    for line in out:
        print(line)


if __name__ == "__main__":
    main()
