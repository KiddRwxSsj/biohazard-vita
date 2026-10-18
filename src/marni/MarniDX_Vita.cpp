// MarniDX_Vita.cpp - vitaGL implementation of the MarniDX backend interface.
//
// The PS Vita counterpart of MarniDX_GL.cpp: same class, same opaque handle
// table, same draw semantics, so nothing above src/marni/ changes. Differences
// that matter:
//   - vitaGL is initialised by the platform entry point (vita/main.cpp); there
//     is no window or context object to bind to.
//   - Shaders are GLSL ES 1.00 so vitaGL's runtime translator can turn them
//     into CG for SceShaccCg. That needs libshacccg.suprx on the console.
//   - The game is drawn into a box on the 960x544 panel picked by config.ini
//     [Vita] Aspect (vita_game_box). The box is what GetBackBufferSize reports,
//     so MarniGetRenderScale and every sprite coordinate are computed against
//     it and the viewport offset does the rest.
//   - Vertices are passed as client arrays. vitaGL copies them into its own
//     per-frame pool at draw time, which is cheaper than re-specifying a VBO
//     for every sprite.
#include "MarniDX.h"
#include "../platform/vita/vita.h"

#include <vitaGL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../game/Types.h"
#include "../Globals.h"
#include "MarniSystem.h"

#define MARNI_MAX_TEXTURES 2048

enum { ATTR_POS = 0, ATTR_UV = 1, ATTR_COL = 2, ATTR_ZW = 3 };

struct MarniDX::Impl {
    int  width  = 0;
    int  height = 0;
    int  offX   = 0;
    int  offY   = 0;
    bool ready  = false;

    GLuint program = 0;
    GLint  uXform  = -1;
    GLint  uTex    = -1;
    GLint  uPersp  = -1;
    GLint  uGamma  = -1;

    MarniHandle whiteHandle = MARNI_NULL_HANDLE;
    MarniHandle fontHandle  = MARNI_NULL_HANDLE;
    int fontW = 0, fontH = 0;

    struct TexSlot {
        GLuint tex = 0;
        int w = 0, h = 0;
        bool smooth = false;    // always bilinear: an HD pack picture bigger than its box
    } slots[MARNI_MAX_TEXTURES];

    void ReleaseSlot(MarniHandle h) {
        if (h == MARNI_NULL_HANDLE || h >= MARNI_MAX_TEXTURES) return;
        if (slots[h].tex != 0) glDeleteTextures(1, &slots[h].tex);
        slots[h].tex = 0;
        slots[h].w = slots[h].h = 0;
        slots[h].smooth = false;
    }

    void ReleaseAll() {
        for (MarniHandle h = 1; h < MARNI_MAX_TEXTURES; ++h) ReleaseSlot(h);
        if (program) { glDeleteProgram(program); program = 0; }
        whiteHandle = MARNI_NULL_HANDLE;
        fontHandle  = MARNI_NULL_HANDLE;
    }
};

static MarniDX* g_pDX = nullptr;

#ifdef RE1_AUTODOOR
// Test build only: a CPU copy of every texture, so a frame's draws can be
// rasterised offline (Vita3K does not draw the TMD models).
static unsigned char* s_keepTex[MARNI_MAX_TEXTURES];
static int s_keepW[MARNI_MAX_TEXTURES], s_keepH[MARNI_MAX_TEXTURES];
static void MarniVita_KeepTexture(MarniHandle h, const void* rgba, int w, int h2)
{
    if (h >= MARNI_MAX_TEXTURES) return;
    free(s_keepTex[h]);
    s_keepTex[h] = (unsigned char*)malloc((size_t)w * h2 * 4);
    if (s_keepTex[h]) memcpy(s_keepTex[h], rgba, (size_t)w * h2 * 4);
    s_keepW[h] = w; s_keepH[h] = h2;
}
void MarniVita_DumpTexture(MarniHandle h, const char* path)
{
    if (h >= MARNI_MAX_TEXTURES || s_keepTex[h] == nullptr) return;
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(&s_keepW[h], 4, 1, f); fwrite(&s_keepH[h], 4, 1, f);
    fwrite(s_keepTex[h], 1, (size_t)s_keepW[h] * s_keepH[h] * 4, f);
    fclose(f);
}
#endif

MarniDX* Marni_DX() { return g_pDX; }

// ---------------------------------------------------------------------------
// Shaders
//
// The ortho transform is a scale + offset uniform instead of a mat4 so the
// translator only ever sees plain vector math.
// ---------------------------------------------------------------------------

static const char* kVertexSrc =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "attribute vec4 aCol;\n"
    "attribute vec2 aZW;\n"
    "uniform vec4 uXform;\n"
    "uniform float uPersp;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main() {\n"
    "    vec4 p = vec4(aPos.x * uXform.x + uXform.z,\n"
    "                  aPos.y * uXform.y + uXform.w,\n"
    "                  aZW.x * 2.0 - 1.0, 1.0);\n"
    "    if (uPersp > 0.5) {\n"
    "        float w = max(aZW.y, 0.0001);\n"
    "        p = vec4(p.xyz * w, w);\n"
    "    }\n"
    "    gl_Position = p;\n"
    "    vUV = aUV;\n"
    "    vCol = aCol;\n"
    "}\n";

static const char* kFragmentSrc =
    "uniform sampler2D uTex;\n"
    "uniform float uGamma;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main() {\n"
    "    vec4 c = texture2D(uTex, vUV) * vCol;\n"
    "    if (uGamma != 1.0) {\n"
    "        c.rgb = pow(c.rgb, vec3(uGamma));\n"
    "    }\n"
    "    gl_FragColor = c;\n"
    "}\n";

static GLuint CompileShader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof(log) - 1, &n, log);
        log[n] = '\0';
        fprintf(stderr, "[GL] shader compile failed: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool BuildPipeline(MarniDX::Impl* p)
{
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentSrc);
    if (!vs || !fs) return false;

    p->program = glCreateProgram();
    glAttachShader(p->program, vs);
    glAttachShader(p->program, fs);
    glBindAttribLocation(p->program, ATTR_POS, "aPos");
    glBindAttribLocation(p->program, ATTR_UV,  "aUV");
    glBindAttribLocation(p->program, ATTR_COL, "aCol");
    glBindAttribLocation(p->program, ATTR_ZW,  "aZW");
    glLinkProgram(p->program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(p->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "[GL] program link failed\n");
        glDeleteProgram(p->program);
        p->program = 0;
        return false;
    }

    p->uXform = glGetUniformLocation(p->program, "uXform");
    p->uTex   = glGetUniformLocation(p->program, "uTex");
    p->uPersp = glGetUniformLocation(p->program, "uPersp");
    p->uGamma = glGetUniformLocation(p->program, "uGamma");

    glUseProgram(p->program);
    glUniform1i(p->uTex, 0);
    // The shader raises colour to this power, so the setting's "higher is
    // brighter" needs the reciprocal.
    glUniform1f(p->uGamma, 1.0f / g_vitaVideo.gamma);

    glEnableVertexAttribArray(ATTR_POS);
    glEnableVertexAttribArray(ATTR_UV);
    glEnableVertexAttribArray(ATTR_COL);
    glEnableVertexAttribArray(ATTR_ZW);
    return true;
}

// Draw coordinates are pixels with a top-left origin, mapped onto whatever
// viewport is current: x' = x * 2/w - 1, y' = y * -2/h + 1.
static void SetPixelSpace(MarniDX::Impl* p, int x, int y, int w, int h)
{
    glDisable(GL_SCISSOR_TEST);
    glViewport(x, y, w, h);
    glUseProgram(p->program);
    glUniform4f(p->uXform, 2.0f / (float)w, -2.0f / (float)h, -1.0f, 1.0f);
}

// The viewport alone does not keep the game inside the box: GXM rasterises
// with a guard band, so a model that runs off the edge of the 320x240 picture
// was drawn on into the pillarbox. The scissor is what clips it.
static void ApplyGameViewport(MarniDX::Impl* p)
{
    SetPixelSpace(p, p->offX, p->offY, p->width, p->height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(p->offX, p->offY, p->width, p->height);
}

// ---------------------------------------------------------------------------
// Internal draw
// ---------------------------------------------------------------------------

static void ApplyBlend(MarniBlend blend)
{
    switch (blend) {
    case MARNI_BLEND_ADD:
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        break;
    case MARNI_BLEND_DISABLE:
        glDisable(GL_BLEND);
        break;
    case MARNI_BLEND_ALPHA:
    default:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    }
}

static void ApplySampler(MarniSampler sampler, bool smooth = false)
{
    GLint filter = (sampler == MARNI_SAMPLER_POINT && !g_vitaVideo.linearFilter && !smooth)
                 ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

static void DrawBatchNow(MarniDX::Impl* p, const float* verts, int vertexCount,
                         MarniHandle tex, MarniSampler sampler, MarniBlend blend,
                         bool depthTest, bool depthWrite, bool persp)
{
    if (p == nullptr || !p->ready || p->program == 0 || vertexCount <= 0) return;

    GLuint texture = p->whiteHandle != MARNI_NULL_HANDLE
                   ? p->slots[p->whiteHandle].tex : 0;
    if (tex != MARNI_NULL_HANDLE && tex < MARNI_MAX_TEXTURES && p->slots[tex].tex != 0) {
        texture = p->slots[tex].tex;
    }

    glUseProgram(p->program);
    glUniform1f(p->uPersp, persp ? 1.0f : 0.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    ApplySampler(sampler, tex != MARNI_NULL_HANDLE && tex < MARNI_MAX_TEXTURES && p->slots[tex].smooth);
    ApplyBlend(blend);

    if (depthTest) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(depthWrite ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }

    // Internal layout: x, y, z, w, u, v, r, g, b, a.
    const GLsizei stride = 10 * sizeof(float);
    glVertexAttribPointer(ATTR_POS, 2, GL_FLOAT, GL_FALSE, stride, verts + 0);
    glVertexAttribPointer(ATTR_ZW,  2, GL_FLOAT, GL_FALSE, stride, verts + 2);
    glVertexAttribPointer(ATTR_UV,  2, GL_FLOAT, GL_FALSE, stride, verts + 4);
    glVertexAttribPointer(ATTR_COL, 4, GL_FLOAT, GL_FALSE, stride, verts + 6);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
}

// ---------------------------------------------------------------------------
// 60 FPS
//
// The game still ticks at 30 Hz. While [Vita] FrameRate=60 is on and the game
// is in play, a tick's draws are recorded instead of drawn, and Present plays
// the list back twice: once with every 3D triangle halfway between where it
// was last tick and where it is now (TmdRenderer hands the old corners over in
// g_portInterpPrev), then as the game drew it. Sprites, backgrounds and the
// HUD are the same in both. The in-between picture costs half a tick of lag,
// the same trade REuncap makes on the PC.
// ---------------------------------------------------------------------------

struct InterpOp {
    bool  clear;
    float rgba[4];
    int   first;        // float offset in s_recVerts
    int   prevFirst;    // -1: nothing to blend
    int   count;        // vertices
    MarniHandle  tex;
    MarniSampler sampler;
    MarniBlend   blend;
    bool  depthTest, depthWrite, persp;
};

static bool   s_recording = false;
static DWORD  s_recFrame  = 0;
static InterpOp* s_recOps = nullptr;
static int    s_recOpCount = 0, s_recOpCap = 0;
static float* s_recVerts  = nullptr;
static int    s_recVertCount = 0, s_recVertCap = 0;   // in floats
static float* s_lerpVerts = nullptr;
static int    s_lerpCap   = 0;

static InterpOp* RecordOp(void)
{
    if (s_recOpCount == s_recOpCap) {
        int cap = s_recOpCap ? s_recOpCap * 2 : 256;
        InterpOp* grown = (InterpOp*)realloc(s_recOps, (size_t)cap * sizeof(InterpOp));
        if (grown == nullptr) return nullptr;
        s_recOps = grown;
        s_recOpCap = cap;
    }
    InterpOp* op = &s_recOps[s_recOpCount++];
    memset(op, 0, sizeof(*op));
    op->prevFirst = -1;
    return op;
}

static int RecordVerts(const float* v, int floats)
{
    if (s_recVertCount + floats > s_recVertCap) {
        int cap = s_recVertCap ? s_recVertCap : 65536;
        while (cap < s_recVertCount + floats) cap *= 2;
        float* grown = (float*)realloc(s_recVerts, (size_t)cap * sizeof(float));
        if (grown == nullptr) return -1;
        s_recVerts = grown;
        s_recVertCap = cap;
    }
    int at = s_recVertCount;
    memcpy(s_recVerts + at, v, (size_t)floats * sizeof(float));
    s_recVertCount += floats;
    return at;
}

static void DrawBatch(MarniDX::Impl* p, const float* verts, int vertexCount,
                      MarniHandle tex, MarniSampler sampler, MarniBlend blend,
                      bool depthTest, bool depthWrite, bool persp)
{
    if (!s_recording) {
        DrawBatchNow(p, verts, vertexCount, tex, sampler, blend, depthTest, depthWrite, persp);
        return;
    }
    InterpOp* op = RecordOp();
    if (op == nullptr) return;
    op->first = RecordVerts(verts, vertexCount * 10);
    if (op->first < 0) { --s_recOpCount; return; }
    if (persp && g_portInterpPrev != nullptr)
        op->prevFirst = RecordVerts(g_portInterpPrev, vertexCount * 10);
    op->count = vertexCount;
    op->tex = tex;
    op->sampler = sampler;
    op->blend = blend;
    op->depthTest = depthTest;
    op->depthWrite = depthWrite;
    op->persp = persp;
}

static void ClearNow(MarniDX::Impl* p, float r, float g, float b, float a);

// Blend each triangle's corners toward last tick by (1 - t). A triangle that
// moved further than a believable step in one tick teleported (a cut, a warp,
// a respawn), so it is drawn where it is now.
static const float* LerpOp(MarniDX::Impl* p, const InterpOp* op, float t)
{
    const float* cur = s_recVerts + op->first;
    if (op->prevFirst < 0 || t >= 1.0f) return cur;
    const float* prev = s_recVerts + op->prevFirst;

    const int floats = op->count * 10;
    if (floats > s_lerpCap) {
        float* grown = (float*)realloc(s_lerpVerts, (size_t)floats * sizeof(float));
        if (grown == nullptr) return cur;
        s_lerpVerts = grown;
        s_lerpCap = floats;
    }
    memcpy(s_lerpVerts, cur, (size_t)floats * sizeof(float));

    const float maxStep = (float)p->width * 0.15f;
    for (int tri = 0; tri + 3 <= op->count; tri += 3) {
        bool jump = false;
        for (int c = 0; c < 3 && !jump; ++c) {
            const float* a = cur + (tri + c) * 10;
            const float* b = prev + (tri + c) * 10;
            jump = fabsf(a[0] - b[0]) > maxStep || fabsf(a[1] - b[1]) > maxStep ||
                   fabsf(a[3] - b[3]) > 2500.0f;
        }
        if (jump) continue;
        for (int c = 0; c < 3; ++c) {
            float* d = s_lerpVerts + (tri + c) * 10;
            const float* b = prev + (tri + c) * 10;
            for (int k = 0; k < 4; ++k) d[k] = b[k] + (d[k] - b[k]) * t;
        }
    }
    return s_lerpVerts;
}

static void Replay(MarniDX::Impl* p, float t)
{
    for (int i = 0; i < s_recOpCount; ++i) {
        const InterpOp* op = &s_recOps[i];
        if (op->clear) {
            ClearNow(p, op->rgba[0], op->rgba[1], op->rgba[2], op->rgba[3]);
            continue;
        }
        DrawBatchNow(p, LerpOp(p, op, t), op->count, op->tex, op->sampler, op->blend,
                     op->depthTest, op->depthWrite, op->persp);
    }
}

static void ResetRecording(void)
{
    s_recOpCount = 0;
    s_recVertCount = 0;
}

static inline void ExpandVertex(float* dst, const float* src, bool hasZ, bool hasW)
{
    if (hasZ && hasW) {
        memcpy(dst, src, 10 * sizeof(float));
        return;
    }

    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = hasZ ? src[2] : 0.0f;
    dst[3] = 1.0f;

    const float* rest = src + (hasZ ? 3 : 2);
    for (int i = 0; i < 6; ++i) dst[4 + i] = rest[i];
}

static float* ExpandBatch(const float* src, int vertexCount, bool hasZ, bool hasW)
{
    static float* s_buf = nullptr;
    static int s_cap = 0;
    int need = vertexCount * 10;
    if (need > s_cap) {
        float* grown = (float*)realloc(s_buf, (size_t)need * sizeof(float));
        if (grown == nullptr) return nullptr;
        s_buf = grown;
        s_cap = need;
    }
    int inStride = hasZ && hasW ? 10 : (hasZ ? 9 : 8);
    for (int v = 0; v < vertexCount; ++v) {
        ExpandVertex(s_buf + v * 10, src + v * inStride, hasZ, hasW);
    }
    return s_buf;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

MarniDX::MarniDX() : m_pImpl(new Impl()) {}
MarniDX::~MarniDX() { Destroy(); delete m_pImpl; m_pImpl = nullptr; }

BOOL MarniDX::Create(HWND, int, int, BOOL, int* outWidth, int* outHeight)
{
    Impl* p = m_pImpl;
    if (p == nullptr || !vita_gl_ready()) return FALSE;

    vita_game_box(&p->offX, &p->offY, &p->width, &p->height);

    if (!BuildPipeline(p)) return FALSE;

    const DWORD white = 0xFFFFFFFFu;
    p->whiteHandle = CreateTexture(1, 1, 32, &white, nullptr, nullptr);
    if (p->whiteHandle == MARNI_NULL_HANDLE) return FALSE;

    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glClearDepthf(1.0f);
    ApplyGameViewport(p);

    p->ready = true;
    if (outWidth)  *outWidth  = p->width;
    if (outHeight) *outHeight = p->height;
    return TRUE;
}

void MarniDX::Destroy()
{
    Impl* p = m_pImpl;
    if (p == nullptr) return;
    p->ReleaseAll();
    p->ready = false;
}

BOOL MarniDX::IsReady() const
{
    return (m_pImpl != nullptr && m_pImpl->ready) ? TRUE : FALSE;
}

void MarniDX::GetBackBufferSize(DWORD* outWidth, DWORD* outHeight) const
{
    Impl* p = m_pImpl;
    if (outWidth)  *outWidth  = (p != nullptr) ? (DWORD)p->width : 0;
    if (outHeight) *outHeight = (p != nullptr) ? (DWORD)p->height : 0;
}

// The panel has one mode. Accept the request so the options screen does not
// report a failure, but keep drawing into the same box.
int MarniDX::ChangeDisplayMode(DWORD, DWORD, BOOL)
{
    return (m_pImpl != nullptr && m_pImpl->ready) ? 1 : 0;
}

int MarniDX::HandleWindowMessage(HWND, UINT, WPARAM, LPARAM)
{
    return 1;
}

unsigned int MarniDX::QueryVideoMemory(BOOL) const
{
    return 128u * 1024u * 1024u;
}

void MarniDX::EnumerateDisplayModes(DisplayModeInfo* outModes, int maxModes,
                                    int* outCount)
{
    int count = 0;
    if (outModes != nullptr && maxModes > 0 && m_pImpl != nullptr) {
        outModes[0].dwWidth       = (DWORD)m_pImpl->width;
        outModes[0].dwHeight      = (DWORD)m_pImpl->height;
        outModes[0].dwBPP         = 32;
        outModes[0].dwRefreshRate = 60;
        outModes[0].dwFlags       = 0;
        count = 1;
    }
    if (outCount) *outCount = count;
}

void MarniDX::EnumerateAdapters(D3DRendererInfo* outRenderers, int max,
                                int* outCount)
{
    int count = 0;
    if (outRenderers != nullptr && max > 0) {
        strncpy(outRenderers[0].name, "SGX543MP4+ (vitaGL)", sizeof(outRenderers[0].name) - 1);
        outRenderers[0].name[sizeof(outRenderers[0].name) - 1] = '\0';
        outRenderers[0].flags = 0;
        count = 1;
    }
    if (outCount) *outCount = count;
}

void MarniDX::Clear(float r, float g, float b, float a)
{
    Impl* p = m_pImpl;
    if (p == nullptr || !p->ready) return;

    if (s_recording) {
        InterpOp* op = RecordOp();
        if (op != nullptr) {
            op->clear = true;
            op->rgba[0] = r; op->rgba[1] = g; op->rgba[2] = b; op->rgba[3] = a;
        }
        return;
    }
    ClearNow(p, r, g, b, a);
}

static void ClearNow(MarniDX::Impl* p, float r, float g, float b, float a)
{
    // Clear the whole panel so the pillarbox bars stay black, then scissor the
    // game colour into the box. Depth mask must be on or the depth clear is
    // silently skipped (see the note in MarniDX_GL.cpp).
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, VITA_SCREEN_W, VITA_SCREEN_H);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (r != 0.0f || g != 0.0f || b != 0.0f) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(p->offX, p->offY, p->width, p->height);
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    ApplyGameViewport(p);
}

void MarniDX::Present()
{
    Impl* p = m_pImpl;
    if (p == nullptr || !p->ready) return;

    if (s_recording) {
        // Off before anything is drawn: the panel's own draws go through
        // DrawBatch too, and while recording they were appended to the list
        // instead of drawn, so the in-between frame had no panel (flicker).
        s_recording = false;
        Replay(p, 0.5f);
        SetPixelSpace(p, 0, 0, VITA_SCREEN_W, VITA_SCREEN_H);
        vita_overlay_draw(this);
        ApplyGameViewport(p);
        vglSwapBuffers(GL_FALSE);
        Replay(p, 1.0f);
        ResetRecording();
    }

    ++g_vitaPresentCount;
    vita_overlay_before_draw(this);
    SetPixelSpace(p, 0, 0, VITA_SCREEN_W, VITA_SCREEN_H);
    vita_overlay_draw(this);
    ApplyGameViewport(p);
    vglSwapBuffers(GL_FALSE);

    // Only frames game_loop drew get the in-between picture; menus, movies and
    // the title present as they always did.
    s_recording = g_portFrameInterp != 0 && g_portGameplayFrame != s_recFrame;
    s_recFrame = g_portGameplayFrame;

    // Between frames is the one place a box change cannot split a picture:
    // the next frame's sprites are scaled against the new backbuffer size.
    if (g_vitaVideoDirty) MarniVita_ApplyVideoSettings();
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

MarniHandle MarniDX::CreateTexture(int width, int height, int bpp,
                                   const void* pixelData,
                                   int* outWidth, int* outHeight)
{
    Impl* p = m_pImpl;
    if (p == nullptr || width <= 0 || height <= 0 || pixelData == nullptr) {
        return MARNI_NULL_HANDLE;
    }

    const unsigned char* src = (const unsigned char*)pixelData;
    const unsigned char* upload = src;
    unsigned char* converted = nullptr;

    if (bpp == 24) {
        converted = (unsigned char*)malloc((size_t)width * height * 4);
        if (converted == nullptr) return MARNI_NULL_HANDLE;
        for (int i = 0; i < width * height; ++i) {
            converted[i * 4 + 0] = src[i * 3 + 2];
            converted[i * 4 + 1] = src[i * 3 + 1];
            converted[i * 4 + 2] = src[i * 3 + 0];
            converted[i * 4 + 3] = 0xFF;
        }
        upload = converted;
    } else if (bpp != 32) {
        return MARNI_NULL_HANDLE;
    }

    MarniHandle handle = MARNI_NULL_HANDLE;
    for (MarniHandle h = 1; h < MARNI_MAX_TEXTURES; ++h) {
        if (p->slots[h].tex == 0) { handle = h; break; }
    }
    if (handle == MARNI_NULL_HANDLE) {
        free(converted);
        return MARNI_NULL_HANDLE;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, upload);
    glBindTexture(GL_TEXTURE_2D, 0);

#ifdef RE1_AUTODOOR
    MarniVita_KeepTexture(handle, upload, width, height);
#endif
    free(converted);

    p->slots[handle].tex = tex;
    p->slots[handle].w = width;
    p->slots[handle].h = height;

    if (outWidth)  *outWidth  = width;
    if (outHeight) *outHeight = height;
    return handle;
}

MarniHandle MarniDX::CreateTextureFromBits(CMarniBits* bits)
{
    if (bits == nullptr || bits->m_pPixelData == nullptr) return MARNI_NULL_HANDLE;
    return CreateTexture((int)bits->m_width, (int)bits->m_height,
                         (int)bits->m_bitDepth, bits->m_pPixelData, nullptr, nullptr);
}

BOOL MarniDX::UpdateTexturePixels(MarniHandle tex, const void* pixelData,
                                  int width, int height, int bpp)
{
    Impl* p = m_pImpl;
    if (p == nullptr || tex == MARNI_NULL_HANDLE || tex >= MARNI_MAX_TEXTURES) return FALSE;
    if (p->slots[tex].tex == 0 || pixelData == nullptr) return FALSE;
    if (width != p->slots[tex].w || height != p->slots[tex].h) return FALSE;
    if (bpp != 32) return FALSE;

    glBindTexture(GL_TEXTURE_2D, p->slots[tex].tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixelData);
    glBindTexture(GL_TEXTURE_2D, 0);
    return TRUE;
}

void MarniDX::GetTextureSize(MarniHandle tex, int* outWidth, int* outHeight) const
{
    Impl* p = m_pImpl;
    if (outWidth)  *outWidth  = 0;
    if (outHeight) *outHeight = 0;
    if (p == nullptr || tex == MARNI_NULL_HANDLE || tex >= MARNI_MAX_TEXTURES) return;
    if (outWidth)  *outWidth  = p->slots[tex].w;
    if (outHeight) *outHeight = p->slots[tex].h;
}

// Port: HD pack textures (platform/vita/hdpack.cpp) are drawn bilinear
// whatever FILTER says, since they are scaled down into the game box.
void MarniVita_SetTextureSmooth(MarniHandle tex)
{
    MarniDX* dx = Marni_DX();
    if (dx == nullptr || dx->m_pImpl == nullptr) return;
    if (tex == MARNI_NULL_HANDLE || tex >= MARNI_MAX_TEXTURES) return;
    dx->m_pImpl->slots[tex].smooth = true;
}

void MarniDX::DestroyTexture(MarniHandle tex)
{
    if (m_pImpl != nullptr) m_pImpl->ReleaseSlot(tex);
}

MarniHandle MarniDX::WhiteTexture() const
{
    return (m_pImpl != nullptr) ? m_pImpl->whiteHandle : MARNI_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void MarniDX::DrawSprite(float x, float y, float w, float h,
                         float u0, float v0, float u1, float v1,
                         DWORD color, MarniHandle tex,
                         MarniSampler sampler, MarniBlend blend)
{
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8)  & 0xFF) / 255.0f;
    float b = ( color        & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;

    float x1 = x + w, y1 = y + h;
    const float verts[6 * 8] = {
        x,  y,  u0, v0, r, g, b, a,
        x1, y,  u1, v0, r, g, b, a,
        x,  y1, u0, v1, r, g, b, a,
        x,  y1, u0, v1, r, g, b, a,
        x1, y,  u1, v0, r, g, b, a,
        x1, y1, u1, v1, r, g, b, a,
    };
    const float* expanded = ExpandBatch(verts, 6, false, false);
    if (expanded != nullptr) {
        DrawBatch(m_pImpl, expanded, 6, tex, sampler, blend, false, false, false);
    }
}

void MarniDX::DrawRect(int x, int y, int w, int h, DWORD color)
{
    DrawSprite((float)x, (float)y, (float)w, (float)h,
               0.0f, 0.0f, 1.0f, 1.0f, color,
               (m_pImpl != nullptr) ? m_pImpl->whiteHandle : MARNI_NULL_HANDLE,
               MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA);
}

void MarniDX::DrawLine(float x0, float y0, float x1, float y1,
                       float thickness, DWORD color)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0f) return;

    float nx = -dy / len * (thickness * 0.5f);
    float ny =  dx / len * (thickness * 0.5f);

    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8)  & 0xFF) / 255.0f;
    float b = ( color        & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;

    float ax = x0 + nx, ay = y0 + ny;
    float bx = x1 + nx, by = y1 + ny;
    float cx = x1 - nx, cy = y1 - ny;
    float ex = x0 - nx, ey = y0 - ny;

    const float verts[6 * 8] = {
        ax, ay, 0.0f, 0.0f, r, g, b, a,
        bx, by, 1.0f, 0.0f, r, g, b, a,
        ex, ey, 0.0f, 1.0f, r, g, b, a,
        ex, ey, 0.0f, 1.0f, r, g, b, a,
        bx, by, 1.0f, 0.0f, r, g, b, a,
        cx, cy, 1.0f, 1.0f, r, g, b, a,
    };
    const float* expanded = ExpandBatch(verts, 6, false, false);
    if (expanded != nullptr) {
        DrawBatch(m_pImpl, expanded, 6,
                  (m_pImpl != nullptr) ? m_pImpl->whiteHandle : MARNI_NULL_HANDLE,
                  MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA,
                  false, false, false);
    }
}

void MarniDX::DrawTriangles(const float* verts, int triCount, MarniHandle tex,
                            MarniSampler sampler, MarniBlend blend)
{
    if (verts == nullptr || triCount <= 0) return;
    const float* expanded = ExpandBatch(verts, triCount * 3, false, false);
    if (expanded != nullptr) {
        DrawBatch(m_pImpl, expanded, triCount * 3, tex, sampler, blend,
                  false, false, false);
    }
}

void MarniDX::DrawTrianglesPersp(const float* verts, int triCount, MarniHandle tex,
                                 MarniSampler sampler, MarniBlend blend,
                                 bool depthTest)
{
    if (verts == nullptr || triCount <= 0) return;
    DrawBatch(m_pImpl, verts, triCount * 3, tex, sampler, blend,
              depthTest, false, true);
}

void MarniDX::DrawTriangles3D(const float* verts, int triCount, MarniHandle tex,
                              MarniSampler sampler, MarniBlend blend,
                              bool depthWrite)
{
    if (verts == nullptr || triCount <= 0) return;
    DrawBatch(m_pImpl, verts, triCount * 3, tex, sampler, blend,
              true, depthWrite, true);
}

// ---------------------------------------------------------------------------
// Readback
// ---------------------------------------------------------------------------

BOOL MarniDX::CaptureBackbufferToRGBA(void** outPixels, DWORD* outWidth, DWORD* outHeight)
{
    Impl* p = m_pImpl;
    if (p == nullptr || !p->ready || outPixels == nullptr) return FALSE;

    // Mid-frame while recording, the backbuffer holds nothing of this tick yet.
    if (s_recording) {
        Replay(p, 1.0f);
        s_recording = false;
        ResetRecording();
    }

    const size_t pitch = (size_t)p->width * 4;
    unsigned char* buf = (unsigned char*)malloc(pitch * p->height);
    if (buf == nullptr) return FALSE;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(p->offX, p->offY, p->width, p->height, GL_RGBA, GL_UNSIGNED_BYTE, buf);

    unsigned char* row = (unsigned char*)malloc(pitch);
    if (row != nullptr) {
        for (int y = 0; y < p->height / 2; ++y) {
            unsigned char* a = buf + (size_t)y * pitch;
            unsigned char* b = buf + (size_t)(p->height - 1 - y) * pitch;
            memcpy(row, a, pitch);
            memcpy(a, b, pitch);
            memcpy(b, row, pitch);
        }
        free(row);
    }

    *outPixels = buf;
    if (outWidth)  *outWidth  = (DWORD)p->width;
    if (outHeight) *outHeight = (DWORD)p->height;
    return TRUE;
}

BOOL MarniDX::ReadTextureRGBA(MarniHandle, void**, int*, int*)
{
    // Same gap as the Linux backend: only the character-select card mask uses
    // this, and it already copes with a FALSE.
    return FALSE;
}

// ---------------------------------------------------------------------------
// Font texture
// ---------------------------------------------------------------------------

void MarniDX::SetFontTexture(MarniHandle tex, int width, int height)
{
    if (m_pImpl == nullptr) return;
    m_pImpl->fontHandle = tex;
    m_pImpl->fontW = width;
    m_pImpl->fontH = height;
}

MarniHandle MarniDX::FontTexture() const
{
    return (m_pImpl != nullptr) ? m_pImpl->fontHandle : MARNI_NULL_HANDLE;
}

int MarniDX::FontTextureWidth() const
{
    return (m_pImpl != nullptr) ? m_pImpl->fontW : 0;
}

int MarniDX::FontTextureHeight() const
{
    return (m_pImpl != nullptr) ? m_pImpl->fontH : 0;
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

MarniDX* MarniDX_Create()
{
    if (g_pDX != nullptr) return g_pDX;
    g_pDX = new MarniDX();
    return g_pDX;
}

void MarniDX_DestroyGlobal()
{
    if (g_pDX != nullptr) {
        g_pDX->Destroy();
        delete g_pDX;
        g_pDX = nullptr;
    }
}

// Re-read g_vitaVideo after the overlay changed it. MSAA is not here: it is a
// render-target property that vitaGL only takes at init.
void MarniVita_ApplyVideoSettings(void)
{
    g_vitaVideoDirty = FALSE;
    if (g_pDX == nullptr || g_pDX->m_pImpl == nullptr || !g_pDX->m_pImpl->ready) return;
    MarniDX::Impl* p = g_pDX->m_pImpl;

    vita_game_box(&p->offX, &p->offY, &p->width, &p->height);
    ApplyGameViewport(p);
    glUniform1f(p->uGamma, 1.0f / g_vitaVideo.gamma);
    // Two pictures per tick only pace out evenly on the vblank.
    vglWaitVblankStart((g_vitaVideo.vsync || g_portFrameInterp) ? GL_TRUE : GL_FALSE);

    // Same bookkeeping the WM_SIZE path does on the PC builds.
    CMarniDirect3D* d3d = (CMarniDirect3D*)g_pMarniDirect3D;
    if (d3d != nullptr) {
        d3d->m_width  = (DWORD)p->width;
        d3d->m_height = (DWORD)p->height;
    }
    g_dwScreenWidth  = (DWORD)p->width;
    g_dwScreenHeight = (DWORD)p->height;
}
