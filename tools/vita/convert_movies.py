#!/usr/bin/env python3
"""Makes a movie mod playable on the PS Vita port, automatically.

The Vita plays .mp4 movies with its hardware decoder, which only takes H.264
Baseline/Main/High in 4:2:0, and .avi movies with a small ffmpeg that has
Cinepak and MPEG-4 part 2 but no H.264. Packs exported as High 4:4:4, 10-bit,
HEVC, or H.264 inside AVI (TeamX's Japanese movies) are skipped by the port.

Point this at a mod, a whole mods folder, or a downloaded pack. It finds every
`movie` folder in it, checks each file with ffprobe, and writes a ready mod
tree to the output folder:

  * movies the Vita can already play are copied as they are,
  * an mp4 with the right video but other audio gets its audio re-encoded,
  * everything else is re-encoded to H.264 High 4:2:0 + AAC, no taller than
    --max-height, with the same name (.mp4), so the game finds it.

Copy the output folder's contents to ux0:data/RE1/mods/. Files that are
already done are skipped, so a run can be stopped and started again.

usage: convert_movies.py <mod, mods folder or pack> [-o OUT] [--max-height 720]
                         [--crf 20] [--preset medium] [--dry-run]

Needs ffmpeg and ffprobe (with libx264) on the PATH.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

MOVIE_EXT = (".mp4", ".m4v", ".mov", ".mkv", ".avi")
# What the port's own ffmpeg decodes in an .avi (src/platform/vita/video.cpp).
AVI_VIDEO_OK = {"cinepak", "mpeg4", "h263", "svq1"}
# Hardware decoder: Baseline, Main, Extended, High; 8-bit 4:2:0 only.
H264_PROFILES_OK = {"baseline", "constrained baseline", "main", "extended", "high"}
PIX_OK = {"yuv420p", "yuvj420p"}


def probe(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_streams", "-of", "json", path],
        capture_output=True, text=True)
    if out.returncode != 0:
        return None
    return json.loads(out.stdout).get("streams", [])


def verdict(path, max_height):
    """'copy', 'audio' (re-encode the audio only) or 'video' (re-encode all), and why."""
    streams = probe(path)
    if streams is None:
        return "video", "ffprobe could not read it"
    v = next((s for s in streams if s.get("codec_type") == "video"), None)
    a = next((s for s in streams if s.get("codec_type") == "audio"), None)
    if v is None:
        return None, "no video stream"
    ext = os.path.splitext(path)[1].lower()
    codec = v.get("codec_name", "")
    if ext == ".avi" and codec in AVI_VIDEO_OK:
        return "copy", "%s in AVI" % codec
    h = int(v.get("height") or 0)
    w = int(v.get("width") or 0)
    profile = (v.get("profile") or "").lower()
    pix = v.get("pix_fmt", "")
    why = "%s %s %s %dx%d" % (codec, v.get("profile", "?"), pix, w, h)
    if ext not in (".mp4", ".m4v") or codec != "h264":
        return "video", why
    if profile not in H264_PROFILES_OK or pix not in PIX_OK:
        return "video", why
    if h > max_height or w > 1280:
        return "video", why + " (too big)"
    if a is not None and a.get("codec_name") != "aac":
        return "audio", "audio is %s" % a.get("codec_name")
    return "copy", why


def movie_files(root):
    """Every movie directly inside a folder called `movie` (any case)."""
    for dirpath, dirnames, filenames in os.walk(root):
        if os.path.basename(dirpath).lower() != "movie":
            continue
        dirnames[:] = []            # "DM7 clips" and the like are not game movies
        for f in sorted(filenames):
            if f.lower().endswith(MOVIE_EXT):
                yield os.path.join(dirpath, f)


def encode(src, dst, mode, args):
    tmp = dst + ".part.mp4"
    cmd = ["ffmpeg", "-nostdin", "-loglevel", "error", "-stats", "-y", "-i", src]
    if mode == "audio":
        cmd += ["-c:v", "copy"]
    else:
        cmd += ["-vf", "scale=-2:'min(%d,ih)':flags=lanczos,format=yuv420p" % args.max_height,
                "-c:v", "libx264", "-profile:v", "high", "-level", "4.0",
                "-preset", args.preset, "-crf", str(args.crf)]
    cmd += ["-c:a", "aac", "-b:a", "160k", "-ac", "2", "-ar", "44100",
            "-movflags", "+faststart", tmp]
    if subprocess.run(cmd).returncode != 0:
        if os.path.exists(tmp):
            os.remove(tmp)
        return False
    os.replace(tmp, dst)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("src", help="a mod folder, a mods folder, or a downloaded pack")
    ap.add_argument("-o", "--out", help="output folder (default: <src>_vita)")
    ap.add_argument("--max-height", type=int, default=720,
                    help="scale taller movies down to this (default 720)")
    ap.add_argument("--crf", type=int, default=20, help="x264 quality, lower is better (default 20)")
    ap.add_argument("--preset", default="medium", help="x264 preset (default medium)")
    ap.add_argument("--dry-run", action="store_true", help="only say what would be done")
    args = ap.parse_args()

    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            sys.exit("%s is needed on the PATH (https://ffmpeg.org/download.html)" % tool)

    src = os.path.abspath(args.src)
    if not os.path.isdir(src):
        sys.exit("%s is not a folder" % src)
    out = os.path.abspath(args.out or src.rstrip("/\\") + "_vita")
    # The input folder itself becomes one mod in the output, unless it is a
    # mods folder (named "mods"): then its subfolders are the mods.
    base = src if os.path.basename(src).lower() == "mods" else os.path.dirname(src)

    files = list(movie_files(src))
    if not files:
        sys.exit("no movie/ folder with movies found in %s" % src)

    counts = {"copy": 0, "audio": 0, "video": 0, "skip": 0, "fail": 0}
    for i, f in enumerate(files, 1):
        rel = os.path.relpath(f, base)
        mode, why = verdict(f, args.max_height)
        name = os.path.splitext(rel)[0] + (os.path.splitext(rel)[1] if mode == "copy" else ".mp4")
        dst = os.path.join(out, name)
        tag = {"copy": "ok, copy", "audio": "re-encode audio", "video": "re-encode",
               None: "skip"}[mode]
        print("[%d/%d] %s: %s (%s)" % (i, len(files), rel, tag, why), flush=True)
        if mode is None:
            counts["skip"] += 1
            continue
        if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(f):
            counts["skip"] += 1
            continue
        if args.dry_run:
            counts[mode] += 1
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if mode == "copy":
            shutil.copy2(f, dst)
        elif not encode(f, dst, mode, args):
            print("    failed", file=sys.stderr)
            counts["fail"] += 1
            continue
        counts[mode] += 1

    print("\n%d copied, %d audio fixed, %d re-encoded, %d skipped, %d failed"
          % (counts["copy"], counts["audio"], counts["video"], counts["skip"], counts["fail"]))
    if not args.dry_run:
        print("Copy what is inside %s to ux0:data/RE1/mods/" % out)
    return 1 if counts["fail"] else 0


if __name__ == "__main__":
    sys.exit(main())
