#!/bin/bash
# Builds FFmpeg for the PS Vita with the H.264 decoder, which the vdpm package
# leaves out. With it, the mod manager can convert movie packs the Vita's
# hardware decoder can't play (High 4:4:4, 10-bit 4:2:0) on the console.
# Without it the port still builds and runs; those packs just stay unplayable.
#
# usage: build_ffmpeg.sh <install prefix> [ffmpeg-X.Y.Z.tar.xz]
#        EXTRA_CONFIGURE="--disable-asm" for an emulator test build (Vita3K
#        does not implement every NEON instruction FFmpeg uses).
# then:  cmake ... -DRE1_FFMPEG_PREFIX=<install prefix>
#
# The flags are vitasdk/packages' ffmpeg VITABUILD plus h264 (decoder and
# parser) and libswscale. Version 8.1.3 matches the vdpm headers (libavcodec 62.28).
set -e
prefix=$(mkdir -p "${1:?usage: build_ffmpeg.sh <prefix> [tarball]}" && cd "$1" && pwd)
ver=8.1.3
tarball=${2:-}
: "${VITASDK:=/usr/local/vitasdk}"
export PATH=$VITASDK/bin:$PATH
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [ -z "$tarball" ]; then
    tarball=$work/ffmpeg-$ver.tar.xz
    curl -sSL "https://ffmpeg.org/releases/ffmpeg-$ver.tar.xz" -o "$tarball"
fi
tar xf "$tarball" -C "$work"
cd "$work"/ffmpeg-*/

./configure --prefix="$prefix" \
    --enable-cross-compile \
    --cross-prefix=$VITASDK/bin/arm-vita-eabi- \
    --disable-shared --enable-static \
    --disable-runtime-cpudetect --disable-armv5te --disable-armv6t2 \
    --disable-programs --disable-doc --disable-network \
    --enable-libmp3lame \
    --disable-everything \
    --enable-decoder=aac,adpcm_ima_qt,alac,ape,ac3,cinepak,flac,h264,mp3,mpeg4,opus,pcm_s16le,pcm_s24le,pcm_s32le,pcm_s8,pcm_u8,svq1,tta,vorbis,wmalossless,wavpack \
    --enable-demuxer=aac,ac3,aiff,ape,asf,avi,caf,cine,flac,h264,matroska,mov,mp3,pcm_s16le,pcm_s24le,pcm_s32le,pcm_s8,pcm_u8,ogg,tta,wav,wv \
    --enable-muxer=aac,ac3,aiff,asf,avi,caf,flac,matroska,mov,mp3,mp4,ipod,pcm_s16le,pcm_s24le,pcm_s32le,pcm_s8,pcm_u8,ogg,opus,tta,wav,webm,wv \
    --enable-encoder=aac,alac,ac3,flac,libmp3lame,mpeg4,opus,pcm_s16le,pcm_s24le,pcm_s32le,pcm_s8,pcm_u8,tta,vorbis,wavpack \
    --enable-parser=aac,ac3,flac,h263,h264,mpeg4video,mpegaudio,opus,vorbis \
    --enable-bsf=h264_mp4toannexb,aac_adtstoasc \
    --enable-protocol=file \
    --enable-swscale --enable-swresample \
    --disable-debug \
    --arch=armv7-a --cpu=cortex-a9 --enable-neon \
    --target-os=none \
    --extra-cflags="-std=gnu11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -O2 -ftree-vectorize -fomit-frame-pointer -ffast-math -D_BSD_SOURCE" \
    --extra-ldflags="-L$VITASDK/arm-vita-eabi/lib" \
    --extra-libs=-lmpg123 \
    --disable-bzlib --disable-iconv --disable-lzma --disable-sdl2 \
    --disable-securetransport --disable-xlib \
    --enable-pthreads ${EXTRA_CONFIGURE:-}
make -j"$(nproc)"
make install
echo "FFmpeg $ver with H.264 installed to $prefix"
