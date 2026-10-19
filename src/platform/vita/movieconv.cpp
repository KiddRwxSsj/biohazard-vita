// movieconv.cpp - makes a mod's movies playable on the Vita, on the Vita.
//
// The port plays .mp4 through the hardware decoder (H.264 Baseline/Main/High,
// 8-bit 4:2:0, AAC) and .avi through ffmpeg in software, which is only fast
// enough for the light codecs (Cinepak, MPEG-4 part 2). A mod movie is one of:
//
//   MOVIE_OK           already plays,
//   MOVIE_REMUX        hardware-friendly H.264 in the wrong container (TeamX's
//                      H.264-in-AVI): the video is copied into an .mp4 as it
//                      is and the audio re-encoded to AAC - quick,
//   MOVIE_TRANSCODE    H.264 the chip can't decode (High 4:4:4, 10-bit, or
//                      larger than 640x480: AvPlayer stops above 720 lines,
//                      sceVideodec refuses 1280x960, and software H.264 at
//                      that size runs at 1-7 fps), or
//                      any other codec ffmpeg can read: decoded in software
//                      and re-encoded to MPEG-4 part 2 + MP3 in an .avi, at
//                      most 640x480 so it plays back in real time - slow,
//   MOVIE_UNSUPPORTED  nothing here can decode it (HEVC).
//
// The H.264 software decoder is only there when the port is linked against
// tools/vita/build_ffmpeg.sh's FFmpeg; with the vdpm package, 4:4:4 movies
// come back MOVIE_UNSUPPORTED instead of MOVIE_TRANSCODE.
//
// Every conversion writes <name>.part.<ext> and renames it over the result
// only when complete, then deletes the source: a stopped run leaves the mod
// as it was for that movie.
#include "vita.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

bool EndsWithNoCase(const std::string& s, const char* ext)
{
    const size_t n = strlen(ext);
    return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, ext) == 0;
}

std::string Url(const std::string& path) { return "file:" + path; }

// ux0:data/RE1/movieconv.log: one line per stage, for a conversion that fails
// or stalls on the console.
void Log(const char* fmt, ...)
{
    FILE* f = fopen("ux0:data/RE1/movieconv.log", "a");
    if (f == NULL) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

std::string StripExt(const std::string& path)
{
    const size_t dot = path.rfind('.');
    const size_t slash = path.rfind('/');
    return (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? path.substr(0, dot) : path;
}

// --- SPS: profile, chroma format, bit depth ---------------------------------

struct Bits {
    const unsigned char* p;
    size_t n, pos;
    int bit() { if (pos >= n * 8) return 0; const int b = (p[pos >> 3] >> (7 - (pos & 7))) & 1; ++pos; return b; }
    unsigned ue() {
        int zeros = 0;
        while (bit() == 0 && zeros < 31) ++zeros;
        unsigned v = 1;
        for (int i = 0; i < zeros; ++i) v = (v << 1) | (unsigned)bit();
        return v - 1;
    }
};

// `sps` starts at the NAL header byte (type 7). TRUE when the Vita's decoder
// takes it: Baseline/Main/Extended, or a High profile stream that is 8-bit 4:2:0.
bool SpsHardwareOk(const unsigned char* sps, size_t size)
{
    if (size < 4) return false;
    // Emulation prevention bytes only matter past the first few fields.
    unsigned char rbsp[64];
    size_t n = 0;
    for (size_t i = 1; i < size && n < sizeof(rbsp); ++i) {
        if (i + 2 < size && sps[i] == 0 && sps[i + 1] == 0 && sps[i + 2] == 3) {
            rbsp[n++] = 0; if (n < sizeof(rbsp)) rbsp[n++] = 0;
            i += 2;
            continue;
        }
        rbsp[n++] = sps[i];
    }
    const unsigned profile = rbsp[0];
    if (profile == 66 || profile == 77 || profile == 88) return true;
    Bits b{ rbsp, n, 24 };
    b.ue();                                     // seq_parameter_set_id
    unsigned chroma = 1, depthL = 0, depthC = 0;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135) {
        chroma = b.ue();
        if (chroma == 3) b.bit();
        depthL = b.ue();
        depthC = b.ue();
    } else {
        return false;
    }
    return chroma == 1 && depthL == 0 && depthC == 0;
}

// First SPS of an H.264 stream: from avcC extradata, or by reading Annex B
// packets (AVI has no extradata).
int H264HardwareOk(AVFormatContext* fmt, int vs)
{
    const AVCodecParameters* par = fmt->streams[vs]->codecpar;
    if (par->extradata_size > 8 && par->extradata[0] == 1) {
        const int len = (par->extradata[6] << 8) | par->extradata[7];
        if ((par->extradata[5] & 0x1F) > 0 && 8 + len <= par->extradata_size) {
            return SpsHardwareOk(par->extradata + 8, (size_t)len) ? 1 : 0;
        }
    }
    AVPacket* pkt = av_packet_alloc();
    int verdict = -1;
    for (int i = 0; i < 200 && verdict < 0 && av_read_frame(fmt, pkt) >= 0; ++i) {
        if (pkt->stream_index == vs) {
            const unsigned char* d = pkt->data;
            for (int k = 0; k + 4 < pkt->size; ++k) {
                if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1 && (d[k + 3] & 0x1F) == 7) {
                    verdict = SpsHardwareOk(d + k + 3, (size_t)(pkt->size - k - 3)) ? 1 : 0;
                    break;
                }
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return verdict;
}


// --- Display order of an H.264 stream with B-frames ------------------------
//
// AVI stores packets in decode order with no presentation time. Copied into
// an MP4 as they are, every frame would be shown in decode order - B-frames
// out of place. The picture order count in each slice header gives the true
// order; this parses just enough of the SPS, PPS and slice headers for it
// (pic_order_cnt_type 0 and 2, progressive streams).

struct Rbsp {
    unsigned char b[96];
    size_t n = 0;
    Rbsp(const unsigned char* p, size_t size) {
        for (size_t i = 0; i < size && n < sizeof(b); ++i) {
            if (i + 2 < size && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 3) {
                b[n++] = 0; if (n < sizeof(b)) b[n++] = 0;
                i += 2;
                continue;
            }
            b[n++] = p[i];
        }
    }
};

int Se(Bits& b) { const unsigned v = b.ue(); return (v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2); }

struct H264Params {
    bool haveSps = false, havePps = false;
    unsigned log2MaxFrameNum = 4, pocType = 0, log2MaxPocLsb = 4;
    bool frameMbsOnly = true, separateColour = false;
};

void ParseSps(const unsigned char* nal, size_t size, H264Params& hp)
{
    Rbsp r(nal + 1, size - 1);
    Bits b{ r.b, r.n, 24 };                         // after profile, constraints, level
    const unsigned prof = r.b[0];
    b.ue();                                         // sps id
    if (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 || prof == 83 ||
        prof == 86 || prof == 118 || prof == 128 || prof == 138 || prof == 139 || prof == 134 || prof == 135) {
        const unsigned chroma = b.ue();
        if (chroma == 3) hp.separateColour = b.bit() != 0;
        b.ue(); b.ue();                             // bit depths
        b.bit();                                    // qpprime_y_zero_transform_bypass
        if (b.bit()) {                              // seq_scaling_matrix_present
            for (int i = 0; i < (chroma != 3 ? 8 : 12); ++i) {
                if (!b.bit()) continue;
                const int size2 = i < 6 ? 16 : 64;
                int last = 8, next = 8;
                for (int j = 0; j < size2 && next != 0; ++j) {
                    next = (last + Se(b) + 256) % 256;
                    if (next != 0) last = next;
                }
            }
        }
    }
    hp.log2MaxFrameNum = b.ue() + 4;
    hp.pocType = b.ue();
    if (hp.pocType == 0) hp.log2MaxPocLsb = b.ue() + 4;
    else if (hp.pocType == 1) return;               // not handled: the caller re-encodes
    b.ue();                                         // max_num_ref_frames
    b.bit();                                        // gaps_in_frame_num_allowed
    b.ue(); b.ue();                                 // size in macroblocks
    hp.frameMbsOnly = b.bit() != 0;
    hp.haveSps = true;
}

// Display order for packets in decode order: out[i] = frame index at which
// packet i is shown. FALSE when the stream can't be ordered here.
bool H264DisplayOrder(AVFormatContext* fmt, int vs, std::vector<int64_t>& out)
{
    H264Params hp;
    std::vector<int> poc;            // per packet, INT32_MIN = no slice
    std::vector<int> group;          // IDR period the packet belongs to
    int prevLsb = 0, prevMsb = 0, g = -1;
    AVPacket* pkt = av_packet_alloc();
    bool ok = true;
    while (ok && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }
        int thisPoc = INT32_MIN;
        const unsigned char* d = pkt->data;
        const int n = pkt->size;
        for (int k = 0; k + 3 < n && thisPoc == INT32_MIN; ++k) {
            if (!(d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1)) continue;
            const unsigned char* nal = d + k + 3;
            const size_t left = (size_t)(n - k - 3);
            const int type = nal[0] & 0x1F, refIdc = (nal[0] >> 5) & 3;
            if (type == 7) ParseSps(nal, left, hp);
            else if (type == 8) hp.havePps = true;
            else if (type == 1 || type == 5) {
                if (!hp.haveSps || hp.pocType == 1 || !hp.frameMbsOnly) { ok = false; break; }
                Rbsp r(nal + 1, left - 1);
                Bits b{ r.b, r.n, 0 };
                b.ue(); b.ue(); b.ue();             // first_mb, slice_type, pps id
                if (hp.separateColour) { b.bit(); b.bit(); }
                for (unsigned i = 0; i < hp.log2MaxFrameNum; ++i) b.bit();
                if (type == 5) {
                    b.ue();                         // idr_pic_id
                    ++g;
                    prevLsb = prevMsb = 0;
                }
                if (g < 0) g = 0;
                if (hp.pocType == 0) {
                    int lsb = 0;
                    for (unsigned i = 0; i < hp.log2MaxPocLsb; ++i) lsb = (lsb << 1) | b.bit();
                    const int maxLsb = 1 << hp.log2MaxPocLsb;
                    int msb = prevMsb;
                    if (lsb < prevLsb && prevLsb - lsb >= maxLsb / 2) msb = prevMsb + maxLsb;
                    else if (lsb > prevLsb && lsb - prevLsb > maxLsb / 2) msb = prevMsb - maxLsb;
                    thisPoc = msb + lsb;
                    if (refIdc != 0) { prevMsb = msb; prevLsb = lsb; }
                } else {
                    thisPoc = (int)poc.size() * 2;  // type 2: display order = decode order
                }
            }
        }
        poc.push_back(thisPoc);
        group.push_back(g < 0 ? 0 : g);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (!ok || poc.empty()) return false;

    // Rank by POC inside each IDR period; periods follow each other.
    out.assign(poc.size(), 0);
    size_t start = 0;
    int64_t base = 0;
    while (start < poc.size()) {
        size_t end = start;
        while (end < poc.size() && group[end] == group[start]) ++end;
        std::vector<size_t> idx;
        for (size_t i = start; i < end; ++i) idx.push_back(i);
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            const int pa = poc[a] == INT32_MIN ? INT32_MIN : poc[a];
            const int pb = poc[b] == INT32_MIN ? INT32_MIN : poc[b];
            return pa < pb;
        });
        for (size_t r = 0; r < idx.size(); ++r) out[idx[r]] = base + (int64_t)r;
        base += (int64_t)(end - start);
        start = end;
    }
    return true;
}

bool LightAviCodec(AVCodecID id)
{
    return id == AV_CODEC_ID_CINEPAK || id == AV_CODEC_ID_MPEG4 ||
           id == AV_CODEC_ID_H263 || id == AV_CODEC_ID_SVQ1;
}

// --- Audio: any input -> the output encoder, through a FIFO -----------------

struct AudioOut {
    AVCodecContext* dec = nullptr;
    AVCodecContext* enc = nullptr;
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVStream* st = nullptr;
    int64_t pts = 0;

    ~AudioOut() {
        avcodec_free_context(&dec);
        avcodec_free_context(&enc);
        swr_free(&swr);
        if (fifo) av_audio_fifo_free(fifo);
    }

    bool Open(const AVStream* in, AVFormatContext* out, AVCodecID codec, int bitrate) {
        const AVCodec* d = avcodec_find_decoder(in->codecpar->codec_id);
        const AVCodec* e = avcodec_find_encoder(codec);
        if (!d || !e) return false;
        dec = avcodec_alloc_context3(d);
        if (avcodec_parameters_to_context(dec, in->codecpar) < 0 || avcodec_open2(dec, d, NULL) < 0) return false;
        enc = avcodec_alloc_context3(e);
        enc->sample_rate = 44100;
        av_channel_layout_default(&enc->ch_layout, 2);
        enc->sample_fmt = e->sample_fmts ? e->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        enc->bit_rate = bitrate;
        enc->time_base = AVRational{ 1, 44100 };
        if (out->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(enc, e, NULL) < 0) return false;
        if (swr_alloc_set_opts2(&swr, &enc->ch_layout, enc->sample_fmt, 44100,
                                &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0, NULL) < 0 ||
            swr_init(swr) < 0) return false;
        fifo = av_audio_fifo_alloc(enc->sample_fmt, 2, 4096);
        st = avformat_new_stream(out, NULL);
        if (!fifo || !st) return false;
        avcodec_parameters_from_context(st->codecpar, enc);
        st->time_base = enc->time_base;
        return true;
    }

    bool Drain(AVFormatContext* out, bool flush) {
        const int frameSize = enc->frame_size > 0 ? enc->frame_size : 1024;
        while (av_audio_fifo_size(fifo) >= frameSize || (flush && av_audio_fifo_size(fifo) > 0)) {
            const int n = FFMIN(frameSize, av_audio_fifo_size(fifo));
            AVFrame* f = av_frame_alloc();
            f->nb_samples = n;
            f->format = enc->sample_fmt;
            f->sample_rate = 44100;
            av_channel_layout_copy(&f->ch_layout, &enc->ch_layout);
            if (av_frame_get_buffer(f, 0) < 0) { av_frame_free(&f); return false; }
            av_audio_fifo_read(fifo, (void**)f->data, n);
            f->pts = pts;
            pts += n;
            const bool ok = Encode(out, f);
            av_frame_free(&f);
            if (!ok) return false;
        }
        if (flush) return Encode(out, NULL);
        return true;
    }

    bool Encode(AVFormatContext* out, AVFrame* f) {
        if (avcodec_send_frame(enc, f) < 0 && f != NULL) return false;
        AVPacket* p = av_packet_alloc();
        while (avcodec_receive_packet(enc, p) >= 0) {
            av_packet_rescale_ts(p, enc->time_base, st->time_base);
            p->stream_index = st->index;
            if (av_interleaved_write_frame(out, p) < 0) { av_packet_free(&p); return false; }
        }
        av_packet_free(&p);
        return true;
    }

    bool Feed(AVFormatContext* out, AVPacket* pkt) {
        if (avcodec_send_packet(dec, pkt) < 0 && pkt != NULL) return true;    // skip a bad packet
        AVFrame* f = av_frame_alloc();
        bool ok = true;
        while (ok && avcodec_receive_frame(dec, f) >= 0) {
            const int max = swr_get_out_samples(swr, f->nb_samples);
            uint8_t** buf = NULL;
            if (av_samples_alloc_array_and_samples(&buf, NULL, 2, max, enc->sample_fmt, 0) < 0) { ok = false; break; }
            const int got = swr_convert(swr, buf, max, (const uint8_t**)f->extended_data, f->nb_samples);
            if (got > 0) av_audio_fifo_write(fifo, (void**)buf, got);
            av_freep(&buf[0]);
            av_freep(&buf);
            av_frame_unref(f);
            ok = Drain(out, false);
        }
        av_frame_free(&f);
        return ok;
    }
};

double Progress(const AVPacket* pkt, const AVStream* st, double duration)
{
    if (duration <= 0 || pkt->pts == AV_NOPTS_VALUE) return -1;
    const double t = pkt->pts * av_q2d(st->time_base);
    return t / duration;
}

void Report(VitaMovieProgress cb, void* user, double frac)
{
    if (cb && frac >= 0) cb(user, frac > 1 ? 1 : frac);
}

bool Finish(const std::string& tmp, const std::string& dst, const std::string& src)
{
    sceIoRemove(dst.c_str());
    if (sceIoRename(tmp.c_str(), dst.c_str()) < 0) { sceIoRemove(tmp.c_str()); return false; }
    if (strcasecmp(src.c_str(), dst.c_str()) != 0) sceIoRemove(src.c_str());
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// vita.h
// ---------------------------------------------------------------------------

int vita_movie_classify(const char* path)
{
    const std::string p = path;
    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, Url(p).c_str(), NULL, NULL) < 0) return MOVIE_UNSUPPORTED;
    int verdict = MOVIE_UNSUPPORTED;
    int vs = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vs = (int)i; break; }
    }
    if (vs >= 0) {
        const AVCodecID id = fmt->streams[vs]->codecpar->codec_id;
        const bool mp4 = EndsWithNoCase(p, ".mp4") || EndsWithNoCase(p, ".m4v");
        const bool avi = EndsWithNoCase(p, ".avi");
        if (id == AV_CODEC_ID_H264) {
            const AVCodecParameters* par = fmt->streams[vs]->codecpar;
            // The movie pack's 640x480 H.264 is the size known to play.
            const bool small = par->width <= 640 && par->height <= 480;
            const int hw = small ? H264HardwareOk(fmt, vs) : 0;
            if (hw == 1 && mp4) {
                verdict = MOVIE_OK;
            } else if (hw == 1) {
                // Copyable only if its display order can be rebuilt.
                std::vector<int64_t> order;
                av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
                verdict = H264DisplayOrder(fmt, vs, order) ? MOVIE_REMUX
                        : (avcodec_find_decoder(id) != NULL ? MOVIE_TRANSCODE : MOVIE_UNSUPPORTED);
            }
            if (hw != 1 && avcodec_find_decoder(id) != NULL) verdict = MOVIE_TRANSCODE;
        } else if (avi && LightAviCodec(id)) {
            verdict = MOVIE_OK;
        } else if (avcodec_find_decoder(id) != NULL) {
            verdict = MOVIE_TRANSCODE;
        }
    }
    avformat_close_input(&fmt);
    return verdict;
}

// H.264 copied into an .mp4 beside it, audio re-encoded to AAC.
BOOL vita_movie_remux(const char* path, VitaMovieProgress cb, void* user, volatile int* stop)
{
    const std::string src = path;
    const std::string dst = StripExt(src) + ".mp4";
    const std::string tmp = StripExt(src) + ".part.mp4";

    AVFormatContext* in = NULL;
    if (avformat_open_input(&in, Url(src).c_str(), NULL, NULL) < 0) return FALSE;
    in->flags |= AVFMT_FLAG_GENPTS;
    int vs = -1, as = -1;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const int t = in->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO && vs < 0) vs = (int)i;
        if (t == AVMEDIA_TYPE_AUDIO && as < 0) as = (int)i;
    }
    AVFormatContext* out = NULL;
    bool ok = vs >= 0 && avformat_alloc_output_context2(&out, NULL, "mp4", Url(tmp).c_str()) >= 0;
    AVStream* ov = NULL;
    AudioOut audio;
    const bool copyAudio = as >= 0 && in->streams[as]->codecpar->codec_id == AV_CODEC_ID_AAC;
    AVStream* oaCopy = NULL;
    if (ok) {
        ov = avformat_new_stream(out, NULL);
        ok = ov && avcodec_parameters_copy(ov->codecpar, in->streams[vs]->codecpar) >= 0;
        if (ok) {
            ov->codecpar->codec_tag = 0;
            ov->time_base = in->streams[vs]->time_base;
        }
    }
    if (ok && as >= 0) {
        if (copyAudio) {
            oaCopy = avformat_new_stream(out, NULL);
            ok = oaCopy && avcodec_parameters_copy(oaCopy->codecpar, in->streams[as]->codecpar) >= 0;
            if (ok) { oaCopy->codecpar->codec_tag = 0; oaCopy->time_base = in->streams[as]->time_base; }
        } else {
            ok = audio.Open(in->streams[as], out, AV_CODEC_ID_AAC, 160000);
        }
    }
    ok = ok && avio_open(&out->pb, Url(tmp).c_str(), AVIO_FLAG_WRITE) >= 0;
    ok = ok && avformat_write_header(out, NULL) >= 0;

    // Pass 1: the display position of every video packet.
    std::vector<int64_t> order;
    if (ok) {
        AVFormatContext* scan = NULL;
        ok = avformat_open_input(&scan, Url(src).c_str(), NULL, NULL) >= 0;
        ok = ok && H264DisplayOrder(scan, vs, order);
        if (scan) avformat_close_input(&scan);
        Log("remux %s: %d frames ordered ok=%d", src.c_str(), (int)order.size(), (int)ok);
    }
    int64_t delay = 0;
    for (size_t i = 0; i < order.size(); ++i) delay = FFMAX(delay, (int64_t)i - order[i]);

    const double duration = in->duration > 0 ? in->duration / (double)AV_TIME_BASE : 0;
    AVPacket* pkt = av_packet_alloc();
    int64_t lastDts = AV_NOPTS_VALUE;
    size_t videoIndex = 0;
    // One tick of the video stream's time base per frame, as AVI counts them.
    const AVRational frameTb = in->streams[vs >= 0 ? vs : 0]->time_base;
    while (ok && av_read_frame(in, pkt) >= 0) {
        if (stop && *stop) { ok = false; av_packet_unref(pkt); break; }
        if (pkt->stream_index == vs) {
            Report(cb, user, Progress(pkt, in->streams[vs], duration));
            // Decode-order index as dts, the rebuilt display slot as pts.
            const size_t k = videoIndex++;
            if (k < order.size()) {
                pkt->dts = (int64_t)k;
                pkt->pts = order[k] + delay;
            } else {
                if (pkt->pts == AV_NOPTS_VALUE) pkt->pts = pkt->dts;
                if (pkt->dts == AV_NOPTS_VALUE) pkt->dts = pkt->pts;
            }
            if (lastDts != AV_NOPTS_VALUE && pkt->dts <= lastDts) { av_packet_unref(pkt); continue; }
            lastDts = pkt->dts;
            (void)frameTb;
            av_packet_rescale_ts(pkt, in->streams[vs]->time_base, ov->time_base);
            pkt->stream_index = ov->index;
            pkt->pos = -1;
            ok = av_interleaved_write_frame(out, pkt) >= 0;
        } else if (pkt->stream_index == as && oaCopy) {
            av_packet_rescale_ts(pkt, in->streams[as]->time_base, oaCopy->time_base);
            pkt->stream_index = oaCopy->index;
            pkt->pos = -1;
            ok = av_interleaved_write_frame(out, pkt) >= 0;
        } else if (pkt->stream_index == as) {
            ok = audio.Feed(out, pkt);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (ok && as >= 0 && !copyAudio) ok = audio.Feed(out, NULL) && audio.Drain(out, true);
    if (ok) ok = av_write_trailer(out) >= 0;
    if (out) { if (out->pb) avio_closep(&out->pb); avformat_free_context(out); }
    avformat_close_input(&in);
    if (!ok) { sceIoRemove(tmp.c_str()); return FALSE; }
    return Finish(tmp, dst, src) ? TRUE : FALSE;
}

// Decoded in software and re-encoded to MPEG-4 part 2 + MP3 in an .avi.
BOOL vita_movie_transcode(const char* path, VitaMovieProgress cb, void* user, volatile int* stop)
{
    const std::string src = path;
    const std::string dst = StripExt(src) + ".avi";
    const std::string tmp = StripExt(src) + ".part.avi";

    AVFormatContext* in = NULL;
    if (avformat_open_input(&in, Url(src).c_str(), NULL, NULL) < 0) return FALSE;
    in->flags |= AVFMT_FLAG_GENPTS;
    int vs = -1, as = -1;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const int t = in->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO && vs < 0) vs = (int)i;
        if (t == AVMEDIA_TYPE_AUDIO && as < 0) as = (int)i;
    }
    const AVCodec* vd = vs >= 0 ? avcodec_find_decoder(in->streams[vs]->codecpar->codec_id) : NULL;
    const AVCodec* ve = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    AVCodecContext* dec = NULL;
    AVCodecContext* enc = NULL;
    SwsContext* sws = NULL;
    AVFormatContext* out = NULL;
    AVStream* ov = NULL;
    AudioOut audio;
    bool ok = vd && ve;

    if (ok) {
        dec = avcodec_alloc_context3(vd);
        ok = avcodec_parameters_to_context(dec, in->streams[vs]->codecpar) >= 0;
        // No FFmpeg worker threads: they get the Vita's small default pthread
        // stack and the H.264 decoder overflows it. The mod manager converts
        // several movies at once on its own big-stack threads instead.
        dec->thread_count = 1;
        ok = ok && avcodec_open2(dec, vd, NULL) >= 0;
    }
    // At most 640x480, even sizes: the playback side decodes this in software.
    int w = ok ? dec->width : 0, h = ok ? dec->height : 0;
    if (ok && (w <= 0 || h <= 0)) ok = false;
    if (ok && (w > 640 || h > 480)) {
        const double s = FFMIN(640.0 / w, 480.0 / h);
        w = (int)(w * s) & ~1;
        h = (int)(h * s) & ~1;
    }
    AVRational fps = in->streams[vs >= 0 ? vs : 0]->avg_frame_rate;
    if (fps.num <= 0 || fps.den <= 0) fps = in->streams[vs >= 0 ? vs : 0]->r_frame_rate;
    if (fps.num <= 0 || fps.den <= 0 || av_q2d(fps) > 120) fps = AVRational{ 30, 1 };

    ok = ok && avformat_alloc_output_context2(&out, NULL, "avi", Url(tmp).c_str()) >= 0;
    if (ok) {
        enc = avcodec_alloc_context3(ve);
        enc->width = w;
        enc->height = h;
        enc->pix_fmt = AV_PIX_FMT_YUV420P;
        enc->time_base = av_inv_q(fps);
        enc->framerate = fps;
        enc->gop_size = 30;
        enc->max_b_frames = 0;
        enc->flags |= AV_CODEC_FLAG_QSCALE;
        enc->global_quality = FF_QP2LAMBDA * 3;
        enc->thread_count = 1;
        enc->codec_tag = MKTAG('X', 'V', 'I', 'D');
        if (out->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ok = avcodec_open2(enc, ve, NULL) >= 0;
        ov = ok ? avformat_new_stream(out, NULL) : NULL;
        ok = ok && ov && avcodec_parameters_from_context(ov->codecpar, enc) >= 0;
        if (ok) ov->time_base = enc->time_base;
    }
    if (ok && as >= 0) ok = audio.Open(in->streams[as], out, AV_CODEC_ID_MP3, 192000);
    Log("transcode %s: open ok=%d %dx%d -> %dx%d fps %d/%d audio=%d", src.c_str(), (int)ok,
        dec ? dec->width : 0, dec ? dec->height : 0, w, h, fps.num, fps.den, as);
    ok = ok && avio_open(&out->pb, Url(tmp).c_str(), AVIO_FLAG_WRITE) >= 0;
    ok = ok && avformat_write_header(out, NULL) >= 0;
    Log("  header ok=%d", (int)ok);

    AVFrame* scaled = av_frame_alloc();
    if (ok) {
        scaled->format = AV_PIX_FMT_YUV420P;
        scaled->width = w;
        scaled->height = h;
        ok = av_frame_get_buffer(scaled, 0) >= 0;
    }
    int64_t frameNo = 0;
    auto encode = [&](AVFrame* f) -> bool {
        if (avcodec_send_frame(enc, f) < 0 && f != NULL) return false;
        AVPacket* p = av_packet_alloc();
        bool good = true;
        while (good && avcodec_receive_packet(enc, p) >= 0) {
            av_packet_rescale_ts(p, enc->time_base, ov->time_base);
            p->stream_index = ov->index;
            good = av_interleaved_write_frame(out, p) >= 0;
        }
        av_packet_free(&p);
        return good;
    };
    auto video = [&](AVPacket* pk) -> bool {
        if (avcodec_send_packet(dec, pk) < 0 && pk != NULL) return true;    // skip a bad packet
        AVFrame* f = av_frame_alloc();
        bool good = true;
        while (good && avcodec_receive_frame(dec, f) >= 0) {
            sws = sws_getCachedContext(sws, f->width, f->height, (AVPixelFormat)f->format,
                                       w, h, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
            if (!sws || av_frame_make_writable(scaled) < 0) { good = false; break; }
            sws_scale(sws, f->data, f->linesize, 0, f->height, scaled->data, scaled->linesize);
            scaled->pts = frameNo++;
            if ((frameNo % 100) == 1) Log("  frame %lld", (long long)frameNo);
            good = encode(scaled);
            av_frame_unref(f);
        }
        av_frame_free(&f);
        return good;
    };

    const double duration = in->duration > 0 ? in->duration / (double)AV_TIME_BASE : 0;
    AVPacket* pkt = av_packet_alloc();
    while (ok && av_read_frame(in, pkt) >= 0) {
        if (stop && *stop) { ok = false; av_packet_unref(pkt); break; }
        if (pkt->stream_index == vs) {
            Report(cb, user, Progress(pkt, in->streams[vs], duration));
            ok = video(pkt);
        } else if (pkt->stream_index == as) {
            ok = audio.Feed(out, pkt);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    Log("  demux end ok=%d frames=%lld", (int)ok, (long long)frameNo);
    if (ok) ok = video(NULL) && encode(NULL);
    if (ok && as >= 0) ok = audio.Feed(out, NULL) && audio.Drain(out, true);
    if (ok) ok = av_write_trailer(out) >= 0;
    Log("  done ok=%d", (int)ok);

    av_frame_free(&scaled);
    sws_freeContext(sws);
    avcodec_free_context(&dec);
    avcodec_free_context(&enc);
    if (out) { if (out->pb) avio_closep(&out->pb); avformat_free_context(out); }
    avformat_close_input(&in);
    if (!ok) { sceIoRemove(tmp.c_str()); return FALSE; }
    // Finish deletes the source: a movie pack's unplayable .mp4 would
    // otherwise still be preferred over the new .avi.
    return Finish(tmp, dst, src) ? TRUE : FALSE;
}
