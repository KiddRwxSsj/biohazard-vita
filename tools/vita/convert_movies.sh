#!/bin/bash
# Re-encodes a movie mod so the PS Vita's hardware decoder can play it.
#
# The Vita decodes H.264 Baseline/Main/High with 4:2:0 chroma. Packs exported
# as High 4:4:4 (yuv444p), 10-bit or HEVC are skipped by the port, which then
# plays the original .avi instead. This writes a copy of every .mp4 (and any
# .avi/.mkv/.mov) as 4:2:0 High profile with AAC audio, same names, so the
# result drops straight into ux0:data/RE1/mods/<name>/movie/.
#
# usage: convert_movies.sh <folder with the movies> <output folder> [max height]
#        max height defaults to 960 (anything taller is scaled down).
set -e
src=${1:?usage: convert_movies.sh <in> <out> [max height]}
dst=${2:?usage: convert_movies.sh <in> <out> [max height]}
maxh=${3:-960}
command -v ffmpeg >/dev/null || { echo "ffmpeg is needed"; exit 1; }
mkdir -p "$dst"
shopt -s nullglob nocaseglob
for f in "$src"/*.mp4 "$src"/*.avi "$src"/*.mkv "$src"/*.mov; do
    name=$(basename "${f%.*}").mp4
    echo "$name"
    ffmpeg -nostdin -loglevel error -y -i "$f" \
        -vf "scale=-2:'min($maxh,ih)':flags=lanczos,format=yuv420p" \
        -c:v libx264 -profile:v high -level 4.0 -preset slow -crf 18 \
        -c:a aac -b:a 160k -ac 2 -movflags +faststart "$dst/$name"
done
