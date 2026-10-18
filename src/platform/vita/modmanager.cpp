// modmanager.cpp - the mod manager and settings screen.
//
// Opened instead of the game when the app is started from the LiveArea's
// "Mod Manager & Settings" link (template.xml, psla:-modmanager), when it is
// relaunched with -modmanager (the in-game panel's MOD MANAGER row, or the
// LiveArea link tapped while the game is running), or when SELECT is held as
// the game boots. It runs before the game has read a single asset, because
// what it changes - which mods are loaded, in what order, and which region's
// tree is used - decides what everything after it loads.
//
// Pages, flipped with L and R:
//
//   MODS      every folder in ux0:data/RE1/mods/, on or off, in load order
//             (first wins), each with what it carries, which region it was
//             packaged for, and whether it does anything in this setup.
//   GAME      region (USA / JPN tree) and content mode (original / DC).
//   VIDEO     the in-game panel's video settings, plus the HD texture ones.
//   GAMEPLAY  the in-game panel's gameplay settings.
//
// START saves everything to config.ini and boots the game.
//
// Drawing is vitaGL's fixed-function path with its own font (menufont.h),
// since none of the game's renderer or art exists yet.
#include "../platform.h"
#include "../../Globals.h"
#include "../../system/AssetPath.h"
#include "../../system/ConfigFile.h"
#include "vita.h"
#include "menufont.h"

#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <png.h>
#include <vitaGL.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Launch request
// ---------------------------------------------------------------------------

bool s_appUtilReady = false;

void EnsureAppUtil(void)
{
    if (s_appUtilReady) return;
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);
    s_appUtilReady = true;
}

// A LiveArea liveitem with target psla:<args> arrives as app event 5.
bool LiveAreaAsked(void)
{
    EnsureAppUtil();
    SceAppUtilAppEventParam ev;
    memset(&ev, 0, sizeof(ev));
    if (sceAppUtilReceiveAppEvent(&ev) < 0 || ev.type != 0x05) return false;
    char buf[2048];
    memset(buf, 0, sizeof(buf));
    sceAppUtilAppEventParseLiveArea(&ev, buf);
    return strstr(buf, "-modmanager") != NULL;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

GLuint s_font = 0;
GLuint s_backdrop = 0;
GLuint s_white = 0;

const float kCharW = 12.0f;     // MENUFONT_CELL_W at scale 1
const float kLineH = 26.0f;

void MakeFont(void)
{
    const int w = MENUFONT_COLS * MENUFONT_CELL_W, h = MENUFONT_ROWS * MENUFONT_CELL_H;
    unsigned char* rgba = (unsigned char*)malloc((size_t)w * h * 4);
    if (rgba == NULL) return;
    for (int i = 0; i < w * h; ++i) {
        const unsigned char b = kMenuFont[i / 2];
        const unsigned char a = (unsigned char)(((i & 1) ? (b >> 4) : (b & 15)) * 17);
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = a;
    }
    const unsigned int white = 0xFFFFFFFFu;
    glGenTextures(1, &s_white);
    glBindTexture(GL_TEXTURE_2D, s_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);

    glGenTextures(1, &s_font);
    glBindTexture(GL_TEXTURE_2D, s_font);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
}

// The bubble's own 960x544 art, dimmed behind the menu.
void MakeBackdrop(void)
{
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, "app0:sce_sys/pic0.png")) return;
    img.format = PNG_FORMAT_RGBA;
    unsigned char* rgba = (unsigned char*)malloc(PNG_IMAGE_SIZE(img));
    if (rgba == NULL || !png_image_finish_read(&img, NULL, rgba, 0, NULL)) {
        png_image_free(&img);
        free(rgba);
        return;
    }
    glGenTextures(1, &s_backdrop);
    glBindTexture(GL_TEXTURE_2D, s_backdrop);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)img.width, (GLsizei)img.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
}

// Everything is drawn as textured triangles from client arrays, flushed when
// the texture changes and at the end of the frame. (vitaGL's glBegin path is
// not used: it is not reliable across builds.)
struct Vtx { float x, y, u, v, r, g, b, a; };
std::vector<Vtx> s_batch;
GLuint s_batchTex = 0;

void Flush(void)
{
    if (s_batch.empty()) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, s_batchTex);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, sizeof(Vtx), &s_batch[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(Vtx), &s_batch[0].u);
    glColorPointer(4, GL_FLOAT, sizeof(Vtx), &s_batch[0].r);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)s_batch.size());
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    s_batch.clear();
}

void Quad(GLuint tex, float x, float y, float w, float h,
          float u0, float v0, float u1, float v1, unsigned int argb)
{
    if (tex != s_batchTex) {
        Flush();
        s_batchTex = tex;
    }
    const float r = ((argb >> 16) & 255) / 255.0f, g = ((argb >> 8) & 255) / 255.0f;
    const float b = (argb & 255) / 255.0f, a = ((argb >> 24) & 255) / 255.0f;
    const Vtx c[4] = {
        { x,     y,     u0, v0, r, g, b, a },
        { x + w, y,     u1, v0, r, g, b, a },
        { x + w, y + h, u1, v1, r, g, b, a },
        { x,     y + h, u0, v1, r, g, b, a },
    };
    s_batch.push_back(c[0]); s_batch.push_back(c[1]); s_batch.push_back(c[2]);
    s_batch.push_back(c[0]); s_batch.push_back(c[2]); s_batch.push_back(c[3]);
}

void Rect(float x, float y, float w, float h, unsigned int argb)
{
    Quad(s_white, x, y, w, h, 0, 0, 1, 1, argb);
}

void Image(GLuint tex, float x, float y, float w, float h, unsigned int argb)
{
    Quad(tex, x, y, w, h, 0, 0, 1, 1, argb);
}

// Draws `text`, at most `maxChars` of it (ending in "..." when it had to be
// cut); returns the x after the last glyph.
float Text(float x, float y, const char* text, unsigned int argb, float scale = 1.0f, int maxChars = 999)
{
    if (s_font == 0) return x;
    const float aw = (float)(MENUFONT_COLS * MENUFONT_CELL_W);
    const float ah = (float)(MENUFONT_ROWS * MENUFONT_CELL_H);
    const float cw = MENUFONT_CELL_W * scale, chh = MENUFONT_CELL_H * scale;
    const int len = (int)strlen(text);
    const bool cut = len > maxChars && maxChars > 3;
    const int shown = cut ? maxChars - 3 : (len < maxChars ? len : maxChars);
    for (int n = 0; n < shown + (cut ? 3 : 0); ++n, x += cw) {
        const unsigned char c = n < shown ? (unsigned char)text[n] : '.';
        if (c <= 32 || c > 126) continue;
        const int i = c - 32;
        const float u0 = (float)((i % MENUFONT_COLS) * MENUFONT_CELL_W) / aw;
        const float v0 = (float)((i / MENUFONT_COLS) * MENUFONT_CELL_H) / ah;
        Quad(s_font, x, y, cw, chh, u0, v0, u0 + MENUFONT_CELL_W / aw, v0 + MENUFONT_CELL_H / ah, argb);
    }
    return x;
}

// How many characters at `scale` fit between x and `right`.
int CharsFit(float x, float right, float scale)
{
    return (int)((right - x) / (kCharW * scale));
}

// Word-wrapped `text`, `maxChars` a line, at most `maxLines` lines (the last
// one cut with "..." if there is more). Returns the number of lines drawn.
int TextWrap(float x, float y, const char* text, unsigned int argb, float scale,
             int maxChars, int maxLines, float lineH)
{
    int lines = 0;
    const char* p = text;
    while (*p != '\0' && lines < maxLines) {
        while (*p == ' ') ++p;
        const int left = (int)strlen(p);
        int n = left;
        if (left > maxChars) {
            n = maxChars;
            while (n > 0 && p[n] != ' ') --n;      // break at the last space that fits
            if (n == 0) n = maxChars;
        }
        char line[256];
        const bool last = (lines == maxLines - 1);
        if (last && left > n) {
            Text(x, y + lines * lineH, p, argb, scale, maxChars);   // ellipsised
        } else {
            snprintf(line, sizeof(line), "%.*s", n < 255 ? n : 255, p);
            Text(x, y + lines * lineH, line, argb, scale);
        }
        p += n;
        ++lines;
    }
    return lines;
}

void TextRight(float right, float y, const char* text, unsigned int argb, float scale = 1.0f)
{
    Text(right - (float)strlen(text) * kCharW * scale, y, text, argb, scale);
}

// ---------------------------------------------------------------------------
// Mods
// ---------------------------------------------------------------------------

const unsigned int kWhite  = 0xFFE8E8E8;
const unsigned int kGrey   = 0xFF8A8A8A;
const unsigned int kGold   = 0xFFFFC840;
const unsigned int kGreen  = 0xFF60E070;
const unsigned int kRed    = 0xFFFF6060;
const unsigned int kYellow = 0xFFFFE060;
const unsigned int kAccent = 0xFFB0302A;

struct Mod {
    std::string name;
    bool enabled = false;

    // Where its tree is: at its root, and/or under a region folder.
    bool rootTree = false, usaTree = false, jpnTree = false;

    int rooms = 0, textures = 0, audio = 0, movies = 0, badMovies = 0, data = 0;
    int hdBg = 0, hdMask = 0, hdUsable = 0, dcHd = 0, pcOnly = 0, rebirthOnly = 0;
    bool optimized = false;     // OPTIMIZE has run on it (vita_optimized.txt)
    int replaces = 0;           // files it replaces in the active region's tree
    int shadowed = 0;           // of those, how many a mod above it replaces first
};

std::vector<Mod> s_mods;
std::unordered_set<std::string> s_hdHashes[2];  // the names hd_map.txt knows: [0] backgrounds, [1] masks

std::string s_dataDir = VITA_DATA_DIR;

bool DirExists(const std::string& path)
{
    SceIoStat st;
    return sceIoGetstat(path.c_str(), &st) >= 0 && SCE_S_ISDIR(st.st_mode);
}

bool FileExists(const std::string& path)
{
    SceIoStat st;
    return sceIoGetstat(path.c_str(), &st) >= 0 && !SCE_S_ISDIR(st.st_mode);
}

std::string Lower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

std::vector<std::string> List(const std::string& dir, bool wantDirs)
{
    std::vector<std::string> out;
    SceUID d = sceIoDopen(dir.c_str());
    if (d < 0) return out;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        const bool isDir = SCE_S_ISDIR(e.d_stat.st_mode);
        if (e.d_name[0] != '.' && isDir == wantDirs) out.push_back(e.d_name);
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    });
    return out;
}

// Every file under `dir`, as paths relative to it. Capped: an HD pack has a
// couple of thousand, nothing legitimate has fifty thousand.
void Walk(const std::string& dir, const std::string& rel, std::vector<std::string>& out, int depth = 0)
{
    if (depth > 8 || out.size() > 50000) return;
    SceUID d = sceIoDopen(dir.c_str());
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    std::vector<std::string> dirs;
    while (sceIoDread(d, &e) > 0) {
        if (e.d_name[0] != '.') {
            if (SCE_S_ISDIR(e.d_stat.st_mode)) dirs.push_back(e.d_name);
            else out.push_back(rel + e.d_name);
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    for (const std::string& sub : dirs) Walk(dir + "/" + sub, rel + sub + "/", out, depth + 1);
}

bool EndsWith(const std::string& s, const char* ext)
{
    const size_t n = strlen(ext);
    return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, ext) == 0;
}

std::string FirstPart(const std::string& rel)
{
    const size_t slash = rel.find('/');
    return Lower(slash == std::string::npos ? std::string() : rel.substr(0, slash));
}

// Fill in what a mod carries and what it does for `region` ("USA"/"JPN").
// `claimed` collects the files mods above it already replace.
void Analyse(Mod& m, const std::string& region, std::unordered_set<std::string>& claimed)
{
    const std::string root = s_dataDir + "/mods/" + m.name;
    const std::string base = s_dataDir + "/" + region + "/";
    m = Mod{ m.name, m.enabled };

    std::vector<std::string> files;
    Walk(root, "", files);

    for (const std::string& f0 : files) {
        std::string f = f0;
        std::string top = FirstPart(f);

        // A region folder holds a whole tree; look inside it.
        bool inRegion = true;
        if (top == "usa" || top == "jpn") {
            if (top == "usa") m.usaTree = true; else m.jpnTree = true;
            inRegion = (top == Lower(region));
            f = f.substr(4);
            top = FirstPart(f);
            // Sound and voice are shared by both releases (AssetPath.cpp).
            if (top == "sound" || top == "voice") inRegion = true;
        } else if (top.empty()) {
            if (strcasecmp(f.c_str(), "vita_optimized.txt") == 0) m.optimized = true;
            if (EndsWith(f, ".dll") || EndsWith(f, ".asi") || EndsWith(f, ".exe") ||
                EndsWith(f, ".ini")) {
                ++m.pcOnly;
            }
            continue;
        } else if (top != "hires" && top != "bio1dc") {
            m.rootTree = true;
        }

        if (top == "hires") {
            const std::string kind = Lower(f.substr(6, f.find('/', 6) - 6));
            const bool pic = EndsWith(f, ".webp") || EndsWith(f, ".jpg") || EndsWith(f, ".rhd");
            if (pic && (kind == "bgd" || kind == "mask")) {
                (kind == "bgd" ? m.hdBg : m.hdMask)++;
                const size_t slash = f.rfind('/');
                std::string hash = f.substr(slash + 1, 8);
                for (char& c : hash) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
                if (s_hdHashes[kind == "mask" ? 1 : 0].count(hash) != 0) ++m.hdUsable;
            }
            continue;
        }
        if (top == "bio1dc") {
            if (EndsWith(f, ".webp") || EndsWith(f, ".jpg") || EndsWith(f, ".rhd")) ++m.dcHd;
            continue;
        }

        bool counted = true;
        if (top == "sound" || top == "voice") ++m.audio;
        else if (top == "movie") {
            ++m.movies;
            if (!plat_video_can_play((root + "/" + f0).c_str())) {
                ++m.badMovies;
                continue;       // the game plays its own movie instead
            }
        } else if (top.compare(0, 5, "stage") == 0) ++m.rooms;
        else if (top == "enemy" || top == "players" || top == "item_m1" || top == "item_m2" ||
                 top == "objspr" || top == "effspr") ++m.textures;
        else if (top == "data" || top == "text") ++m.data;
        else { counted = false; ++m.rebirthOnly; }       // Mod_SHDP/ and the like

        if (!counted || !inRegion) continue;

        // What would it replace? A movie mp4 stands in for the .avi of the
        // same name; everything else for the file at the same path.
        std::string target = f;
        if (top == "movie" && EndsWith(target, ".mp4")) target = target.substr(0, target.size() - 4) + ".avi";
        if (!plat_file_exists((base + target).c_str())) {
            if (top == "movie" && FileExists(base + f)) target = f; else continue;
        }
        ++m.replaces;
        const std::string key = Lower(target);
        if (m.enabled) {
            if (claimed.count(key) != 0) ++m.shadowed; else claimed.insert(key);
        }
    }
}

std::string s_region = "USA";
std::string s_mode = "OG";
bool s_hasUsa = false, s_hasJpn = false, s_hasDc = false;

void AnalyseAll(void)
{
    std::unordered_set<std::string> claimed;
    for (Mod& m : s_mods) Analyse(m, s_region, claimed);
}

void LoadMods(void)
{
    // hd_map.txt hashes, for the "usable" count of a texture pack.
    FILE* f = fopen("app0:hd_map.txt", "r");
    if (f != NULL) {
        char line[96], name[32], hash[32];
        while (fgets(line, sizeof(line), f) != NULL) {
            if (line[0] != '#' && sscanf(line, "%31s %31s", name, hash) == 2) {
                for (char* c = hash; *c; ++c) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
                s_hdHashes[strncasecmp(name, "osp", 3) == 0 ? 1 : 0].insert(hash);
            }
        }
        fclose(f);
    }

    // [Mods] Load order first, then everything else alphabetically, off.
    char list[512] = "";
    ConfigFile_ReadString("Mods", "Load", list, sizeof(list));
    const std::vector<std::string> folders = List(s_dataDir + "/mods", true);
    std::vector<std::string> order;
    for (char* tok = strtok(list, ","); tok != NULL; tok = strtok(NULL, ",")) {
        while (*tok == ' ' || *tok == '\t') ++tok;
        std::string n = tok;
        while (!n.empty() && (n.back() == ' ' || n.back() == '\t' || n.back() == '\r')) n.pop_back();
        for (const std::string& d : folders) {
            if (strcasecmp(d.c_str(), n.c_str()) == 0) {
                if (std::find(order.begin(), order.end(), d) == order.end()) order.push_back(d);
            }
        }
    }
    for (const std::string& d : order) { Mod m; m.name = d; m.enabled = true; s_mods.push_back(m); }
    for (const std::string& d : folders) {
        if (std::find(order.begin(), order.end(), d) != order.end()) continue;
        Mod m; m.name = d; s_mods.push_back(m);
    }
}

// One line on what the mod is, and one on what it does here.
void Describe(const Mod& m, char* what, size_t whatSize, char* status, size_t statusSize,
              unsigned int* statusColor)
{
    std::string w;
    auto add = [&](int n, const char* label) {
        if (n <= 0) return;
        char b[48];
        snprintf(b, sizeof(b), "%s%d %s", w.empty() ? "" : ", ", n, label);
        w += b;
    };
    add(m.hdBg, "HD backgrounds");
    add(m.hdMask, "HD masks");
    add(m.dcHd, "DC HD images");
    add(m.rooms, "room files");
    add(m.textures, "textures/models");
    add(m.audio, "sounds");
    add(m.movies, "movies");
    add(m.data, "data files");
    if (w.empty()) w = "nothing the port can use";
    if (m.optimized) w = "optimized for Vita: " + w;
    snprintf(what, whatSize, "%s", w.c_str());

    const bool otherRegionOnly = !m.rootTree &&
        ((s_region == "USA" && m.jpnTree && !m.usaTree) || (s_region == "JPN" && m.usaTree && !m.jpnTree));
    const bool hd = m.hdBg + m.hdMask > 0;

    if (!m.enabled) {
        snprintf(status, statusSize, "OFF");
        *statusColor = kGrey;
    } else if (otherRegionOnly && !hd && m.replaces == 0) {
        snprintf(status, statusSize, "INACTIVE: made for the %s version (GAME page)",
                 m.jpnTree ? "JPN" : "USA");
        *statusColor = kRed;
    } else if (hd && !g_vitaVideo.hdTextures && m.replaces == 0) {
        snprintf(status, statusSize, "INACTIVE: HD TEXTURES is off (VIDEO page)");
        *statusColor = kRed;
    } else if (m.replaces == 0 && m.badMovies > 0 && !hd) {
        snprintf(status, statusSize, "INACTIVE: %d movies the Vita cannot play. Convert them on a PC with tools/vita/convert_movies (docs/MODS.md)",
                 m.badMovies);
        *statusColor = kRed;
    } else if (m.replaces == 0 && (!hd || m.hdUsable == 0)) {
        snprintf(status, statusSize, "INACTIVE: replaces nothing in the %s files", s_region.c_str());
        *statusColor = kRed;
    } else {
        char b[160] = "ACTIVE";
        size_t n = strlen(b);
        if (hd) n += snprintf(b + n, sizeof(b) - n, " - %d HD images in use", m.hdUsable);
        if (m.replaces > 0) n += snprintf(b + n, sizeof(b) - n, "%s%d files replaced",
                                          hd ? ", " : " - ", m.replaces - m.shadowed);
        if (m.shadowed > 0) n += snprintf(b + n, sizeof(b) - n, " (%d taken by a mod above)", m.shadowed);
        *statusColor = kGreen;
        if (m.badMovies > 0) {
            snprintf(b + n, sizeof(b) - n, " - %d movies need converting (convert_movies)", m.badMovies);
            *statusColor = kYellow;
        }
        snprintf(status, statusSize, "%s", b);
    }
}

const char* RegionTag(const Mod& m)
{
    if (m.hdBg + m.hdMask > 0 && !m.usaTree && !m.jpnTree) return "ANY";
    if (m.rootTree && !m.usaTree && !m.jpnTree) return "ANY";
    if (m.usaTree && m.jpnTree) return "USA+JPN";
    if (m.usaTree) return m.rootTree ? "ANY+USA" : "USA";
    if (m.jpnTree) return m.rootTree ? "ANY+JPN" : "JPN";
    return "-";
}

// ---------------------------------------------------------------------------
// Settings rows
// ---------------------------------------------------------------------------

enum Page { PAGE_MODS, PAGE_GAME, PAGE_VIDEO, PAGE_GAMEPLAY, PAGE_COUNT };
const char* const kPageNames[PAGE_COUNT] = { "MODS", "GAME", "VIDEO", "GAMEPLAY" };

enum Row {
    ROW_REGION, ROW_MODE,
    ROW_ASPECT, ROW_FILTER, ROW_GAMMA, ROW_MSAA, ROW_FPS, ROW_HD, ROW_HD_SIZE,
    ROW_AUTORUN, ROW_WEAPON, ROW_DOORS, ROW_STATUS_HUD, ROW_WEAPON_HUD, ROW_HUD_HIDE,
};

const std::vector<Row> kPageRows[PAGE_COUNT] = {
    {},
    { ROW_REGION, ROW_MODE },
    { ROW_ASPECT, ROW_FILTER, ROW_GAMMA, ROW_MSAA, ROW_FPS, ROW_HD, ROW_HD_SIZE },
    { ROW_AUTORUN, ROW_WEAPON, ROW_DOORS, ROW_STATUS_HUD, ROW_WEAPON_HUD, ROW_HUD_HIDE },
};

const char* RowLabel(Row r)
{
    switch (r) {
    case ROW_REGION:     return "GAME VERSION";
    case ROW_MODE:       return "CONTENT";
    case ROW_ASPECT:     return "ASPECT";
    case ROW_FILTER:     return "FILTER";
    case ROW_GAMMA:      return "GAMMA";
    case ROW_MSAA:       return "ANTIALIASING";
    case ROW_FPS:        return "FRAME RATE";
    case ROW_HD:         return "HD TEXTURES";
    case ROW_HD_SIZE:    return "HD TEXTURE SIZE";
    case ROW_AUTORUN:    return "AUTO RUN";
    case ROW_WEAPON:     return "WEAPON SWAP";
    case ROW_DOORS:      return "SKIP DOORS";
    case ROW_STATUS_HUD: return "STATUS HUD";
    case ROW_WEAPON_HUD: return "WEAPON HUD";
    case ROW_HUD_HIDE:   return "HUD AUTO HIDE";
    }
    return "";
}

const char* RowHelp(Row r)
{
    switch (r) {
    case ROW_REGION:     return "Which install the game runs from: USA/ or JPN/ (Biohazard).";
    case ROW_MODE:       return "Original PC game, or the Director's Cut (needs a DC/ folder).";
    case ROW_ASPECT:     return "4:3, an exact 2x of the original (640x480), or full screen.";
    case ROW_FILTER:     return "Original sharp pixels, or smoothed.";
    case ROW_GAMMA:      return "Above 1.0 brightens the dark areas.";
    case ROW_MSAA:       return "Smooths the edges of the 3D models. Costs GPU time.";
    case ROW_FPS:        return "60: an extra frame between the game's 30 ticks. Logic stays at 30.";
    case ROW_HD:         return "Use the hires/ folder of texture pack mods (TeamX HD, Seamless HD).";
    case ROW_HD_SIZE:    return "HD pictures are scaled to this. Bigger is sharper and uses more memory.";
    case ROW_AUTORUN:    return "Push forward to run, hold the run button to walk.";
    case ROW_WEAPON:     return "Flick the right stick to change weapon.";
    case ROW_DOORS:      return "No door animation: straight into the next room.";
    case ROW_STATUS_HUD: return "Portrait and ECG in play. Move and size it with EDIT HUD in game.";
    case ROW_WEAPON_HUD: return "Equipped weapon and rounds in play.";
    case ROW_HUD_HIDE:   return "Fade the HUD out when idle, back in on damage, shots or aim.";
    }
    return "";
}

const char* OnOff(BOOL b) { return b ? "ON" : "OFF"; }

void RowValue(Row r, char* out, size_t n)
{
    static const char* const kAspect[3] = { "4:3", "INTEGER 2X", "STRETCH" };
    switch (r) {
    case ROW_REGION:     snprintf(out, n, "%s", s_region == "JPN" ? "JPN (BIOHAZARD)" : "USA"); break;
    case ROW_MODE:       snprintf(out, n, "%s", s_mode == "DC" ? "DIRECTOR'S CUT" : "ORIGINAL"); break;
    case ROW_ASPECT:     snprintf(out, n, "%s", kAspect[g_vitaVideo.aspect]); break;
    case ROW_FILTER:     snprintf(out, n, "%s", g_vitaVideo.linearFilter ? "LINEAR" : "NEAREST"); break;
    case ROW_GAMMA:      snprintf(out, n, "%.1f", g_vitaVideo.gamma); break;
    case ROW_MSAA:       if (g_vitaVideo.msaa) snprintf(out, n, "%dX", g_vitaVideo.msaa); else snprintf(out, n, "OFF"); break;
    case ROW_FPS:        snprintf(out, n, "%d", g_vitaVideo.fps60 ? 60 : 30); break;
    case ROW_HD:         snprintf(out, n, "%s", OnOff(g_vitaVideo.hdTextures)); break;
    case ROW_HD_SIZE:    snprintf(out, n, "%d LINES", g_vitaVideo.hdMaxHeight); break;
    case ROW_AUTORUN:    snprintf(out, n, "%s", OnOff(g_vitaGameplay.autoRun)); break;
    case ROW_WEAPON:     snprintf(out, n, "%s", OnOff(g_vitaGameplay.weaponSwitch)); break;
    case ROW_DOORS:      snprintf(out, n, "%s", OnOff(g_vitaGameplay.skipDoors)); break;
    case ROW_STATUS_HUD: snprintf(out, n, "%s", OnOff(g_vitaGameplay.healthHud)); break;
    case ROW_WEAPON_HUD: snprintf(out, n, "%s", OnOff(g_vitaGameplay.ammoHud)); break;
    case ROW_HUD_HIDE:   snprintf(out, n, "%s", OnOff(g_vitaGameplay.hudAutoHide)); break;
    }
}

void RowChange(Row r, int dir)
{
    switch (r) {
    case ROW_REGION:
        if (s_hasUsa && s_hasJpn) {
            s_region = (s_region == "USA") ? "JPN" : "USA";
            AnalyseAll();
        }
        break;
    case ROW_MODE:
        if (s_hasDc) s_mode = (s_mode == "DC") ? "OG" : "DC";
        break;
    case ROW_ASPECT:  g_vitaVideo.aspect = (g_vitaVideo.aspect + dir + 3) % 3; break;
    case ROW_FILTER:  g_vitaVideo.linearFilter = !g_vitaVideo.linearFilter; break;
    case ROW_GAMMA: {
        int t = (int)(g_vitaVideo.gamma * 10.0f + 0.5f) + dir;
        g_vitaVideo.gamma = (float)(t < 5 ? 5 : t > 25 ? 25 : t) / 10.0f;
        break;
    }
    case ROW_MSAA:
        g_vitaVideo.msaa = dir > 0 ? (g_vitaVideo.msaa == 0 ? 2 : 4) : (g_vitaVideo.msaa == 4 ? 2 : 0);
        break;
    case ROW_FPS:        g_vitaVideo.fps60 = !g_vitaVideo.fps60; break;
    case ROW_HD:         g_vitaVideo.hdTextures = !g_vitaVideo.hdTextures; break;
    case ROW_HD_SIZE: {
        static const int kSizes[3] = { 480, 720, 960 };
        int i = g_vitaVideo.hdMaxHeight >= 960 ? 2 : g_vitaVideo.hdMaxHeight >= 720 ? 1 : 0;
        i = (i + dir + 3) % 3;
        g_vitaVideo.hdMaxHeight = kSizes[i];
        break;
    }
    case ROW_AUTORUN:    g_vitaGameplay.autoRun = !g_vitaGameplay.autoRun; break;
    case ROW_WEAPON:     g_vitaGameplay.weaponSwitch = !g_vitaGameplay.weaponSwitch; break;
    case ROW_DOORS:      g_vitaGameplay.skipDoors = !g_vitaGameplay.skipDoors; break;
    case ROW_STATUS_HUD: g_vitaGameplay.healthHud = !g_vitaGameplay.healthHud; break;
    case ROW_WEAPON_HUD: g_vitaGameplay.ammoHud = !g_vitaGameplay.ammoHud; break;
    case ROW_HUD_HIDE:   g_vitaGameplay.hudAutoHide = !g_vitaGameplay.hudAutoHide; break;
    }
}

// ---------------------------------------------------------------------------
// The screen
// ---------------------------------------------------------------------------

void BeginFrame(void)
{
    glViewport(0, 0, VITA_SCREEN_W, VITA_SCREEN_H);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.03f, 0.02f, 0.02f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, VITA_SCREEN_W, VITA_SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void Draw(int page, int cursor, int scroll, const char* toast)
{
    BeginFrame();
    if (s_backdrop) Image(s_backdrop, 0, 0, VITA_SCREEN_W, VITA_SCREEN_H, 0xFF505050);
    Rect(0, 0, VITA_SCREEN_W, VITA_SCREEN_H, 0x99000000);

    // Header and tabs.
    Rect(0, 0, VITA_SCREEN_W, 64, 0xE0100808);
    Rect(0, 64, VITA_SCREEN_W, 2, kAccent);
    Text(24, 10, "RESIDENT EVIL", kAccent, 1.3f);
    Text(24, 38, "MOD MANAGER & SETTINGS", kGrey, 0.8f);
    float tx = 400;
    Text(tx - 28, 22, "L", kGrey);
    for (int p = 0; p < PAGE_COUNT; ++p) {
        const float w = (float)strlen(kPageNames[p]) * kCharW + 24;
        if (p == page) Rect(tx - 6, 16, w, 34, 0xFF2A1210);
        if (p == page) Rect(tx - 6, 48, w, 3, kAccent);
        Text(tx + 6, 22, kPageNames[p], p == page ? kWhite : kGrey);
        tx += w + 8;
    }
    Text(tx + 4, 22, "R", kGrey);

    const float listY = 82, rowH = 40;
    char help[200] = "";
    unsigned int helpColor = kGrey;

    if (page == PAGE_MODS) {
        if (s_mods.empty()) {
            Text(40, listY + 20, "No mods installed.", kWhite);
            Text(40, listY + 56, "Put each mod in its own folder in ux0:data/RE1/mods/, for example", kGrey, 0.85f);
            Text(40, listY + 80, "mods/HQ_Audio/sound/...  or  mods/Seamless_HD/hires/bgd/...", kGrey, 0.85f);
            Text(40, listY + 104, "and open this screen again.", kGrey, 0.85f);
        }
        const int visible = 8;
        for (int i = scroll; i < (int)s_mods.size() && i < scroll + visible; ++i) {
            const Mod& m = s_mods[i];
            const float y = listY + (float)(i - scroll) * rowH;
            const bool sel = (i == cursor);
            if (sel) Rect(16, y - 4, VITA_SCREEN_W - 32, rowH - 2, 0x60B0302A);

            // Checkbox and load position.
            Rect(28, y + 6, 20, 20, 0xFF303030);
            if (m.enabled) Rect(32, y + 10, 12, 12, kGreen);
            char pos[8] = "";
            if (m.enabled) {
                int n = 0;
                for (int j = 0; j <= i; ++j) if (s_mods[j].enabled) ++n;
                snprintf(pos, sizeof(pos), "%d", n);
            }
            Text(58, y + 2, pos, kGold, 0.8f);
            Text(82, y + 2, m.name.c_str(), m.enabled ? kWhite : kGrey, 1.0f, CharsFit(82, 450, 1.0f));

            char what[160], status[200];
            unsigned int sc;
            Describe(m, what, sizeof(what), status, sizeof(status), &sc);
            Text(460, y + 2, RegionTag(m), kGold, 0.8f);
            // Status, shortened to the column.
            Text(560, y + 2, status, sc, 0.8f, CharsFit(560, VITA_SCREEN_W - 20, 0.8f));
            if (sel) {
                snprintf(help, sizeof(help), "%s", what);
                helpColor = kWhite;
            }
        }
        if ((int)s_mods.size() > visible) {
            const float trackH = rowH * visible;
            const float thumb = trackH * visible / (float)s_mods.size();
            const float off = trackH * scroll / (float)s_mods.size();
            Rect(VITA_SCREEN_W - 12, listY, 4, trackH, 0x40FFFFFF);
            Rect(VITA_SCREEN_W - 12, listY + off, 4, thumb, kAccent);
        }
        // Full status of the selected mod.
        if (cursor < (int)s_mods.size()) {
            char what[160], status[200];
            unsigned int sc;
            Describe(s_mods[cursor], what, sizeof(what), status, sizeof(status), &sc);
            Rect(16, 412, VITA_SCREEN_W - 32, 64, 0xC0000000);
            const int perLine = CharsFit(28, VITA_SCREEN_W - 28, 0.8f);
            const int used = TextWrap(28, 415, status, sc, 0.8f, perLine, 2, 19);
            char extra[240];
            const Mod& m = s_mods[cursor];
            snprintf(extra, sizeof(extra), "%s%s", what,
                     m.pcOnly ? "  (its PC .dll/.asi files are not needed here)" : "");
            TextWrap(28, 415 + used * 19, extra, kGrey, 0.8f, perLine, 3 - used, 19);
        }
    } else {
        const std::vector<Row>& rows = kPageRows[page];
        for (size_t i = 0; i < rows.size(); ++i) {
            const float y = listY + 8 + (float)i * rowH;
            const bool sel = ((int)i == cursor);
            if (sel) Rect(16, y - 6, VITA_SCREEN_W - 32, rowH - 2, 0x60B0302A);
            bool locked = (rows[i] == ROW_REGION && !(s_hasUsa && s_hasJpn)) ||
                          (rows[i] == ROW_MODE && !s_hasDc);
            Text(40, y, RowLabel(rows[i]), sel ? kWhite : 0xFFC0C0C0);
            char v[48], line[64];
            RowValue(rows[i], v, sizeof(v));
            snprintf(line, sizeof(line), locked ? "%s" : "< %s >", v);
            Text(520, y, line, locked ? kGrey : (sel ? kGold : kWhite));
            if (sel) {
                if (rows[i] == ROW_REGION && !(s_hasUsa && s_hasJpn)) {
                    snprintf(help, sizeof(help), "Only %s/ is installed. Add a %s/ folder beside it to switch.",
                             s_hasJpn ? "JPN" : "USA", s_hasJpn ? "USA" : "JPN");
                } else if (rows[i] == ROW_MODE && !s_hasDc) {
                    snprintf(help, sizeof(help), "No DC/ folder found. See the README for the Director's Cut mode.");
                } else if (rows[i] == ROW_MSAA) {
                    snprintf(help, sizeof(help), "%s Applies when the game starts.", RowHelp(rows[i]));
                } else {
                    snprintf(help, sizeof(help), "%s", RowHelp(rows[i]));
                }
            }
        }
        Rect(16, 412, VITA_SCREEN_W - 32, 64, 0xC0000000);
        TextWrap(28, 415, help, kGrey, 0.8f, CharsFit(28, VITA_SCREEN_W - 28, 0.8f), 3, 19);
        help[0] = '\0';
    }

    // Footer.
    Rect(0, VITA_SCREEN_H - 62, VITA_SCREEN_W, 62, 0xE0100808);
    Rect(0, VITA_SCREEN_H - 62, VITA_SCREEN_W, 2, kAccent);
    if (toast != NULL && toast[0] != '\0') {
        Text(24, VITA_SCREEN_H - 44, toast, kYellow, 0.85f, CharsFit(24, VITA_SCREEN_W - 246, 0.85f));
    } else if (page == PAGE_MODS) {
        Text(24, VITA_SCREEN_H - 44, "X ON/OFF   SQUARE UP   TRIANGLE DOWN   SELECT OPTIMIZE", kGrey, 0.8f);
    } else {
        Text(24, VITA_SCREEN_H - 44, "LEFT/RIGHT CHANGE", kGrey, 0.8f);
    }
    Rect(VITA_SCREEN_W - 236, VITA_SCREEN_H - 52, 216, 42, kAccent);
    Text(VITA_SCREEN_W - 222, VITA_SCREEN_H - 44, "START  PLAY", kWhite, 1.0f);

    Flush();
    vglSwapBuffers(GL_FALSE);
}


// ---------------------------------------------------------------------------
// OPTIMIZE: destructive, on-console conversion of a mod (SELECT on MODS)
//
// Pictures: every hires/bgd|mask .webp -> .jpg / .rhd at HdMaxHeight
// (hdpack.cpp), the .webp deleted. Movies: remuxed or transcoded so the Vita
// plays them (movieconv.cpp), the source deleted. Then everything the port
// never reads goes: hires/skin, skin2, misc, .dll/.asi, and bio1dc/ when no
// DC/ is installed. Each file is replaced on its own, so CIRCLE (or a crash)
// leaves a mod that still works and a second run finishes the rest.
// ---------------------------------------------------------------------------

struct PlanPic { std::string path; bool mask; };
struct PlanMovie { std::string path; int kind; };

struct Plan {
    std::vector<PlanPic> pics;
    std::vector<PlanMovie> movies;
    std::vector<std::string> junkFiles;     // deleted outright
    std::vector<std::string> junkDirs;      // emptied, then removed (deepest first)
    long long junkBytes = 0, picBytes = 0;
    int remux = 0, transcode = 0, unsupported = 0;
};

std::unordered_map<std::string, long long> s_sizes;     // filled by WalkAll

long long FileSize(const std::string& path)
{
    auto it = s_sizes.find(path);
    return it != s_sizes.end() ? it->second : 0;
}

// Every file and folder under `dir` (absolute paths), folders deepest first.
void WalkAll(const std::string& dir, std::vector<std::string>& files, std::vector<std::string>& dirs,
             long long* bytes, int depth = 0)
{
    if (depth > 10) return;
    SceUID d = sceIoDopen(dir.c_str());
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    std::vector<std::string> subs;
    while (sceIoDread(d, &e) > 0) {
        if (e.d_name[0] != '.') {
            const std::string p = dir + "/" + e.d_name;
            if (SCE_S_ISDIR(e.d_stat.st_mode)) subs.push_back(p);
            else {
                files.push_back(p);
                s_sizes[p] = (long long)e.d_stat.st_size;
                if (bytes) *bytes += (long long)e.d_stat.st_size;
            }
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    for (const std::string& sub : subs) WalkAll(sub, files, dirs, bytes, depth + 1);
    dirs.push_back(dir);
}

std::string ParentName(const std::string& path)
{
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return std::string();
    const size_t prev = path.rfind('/', slash - 1);
    return Lower(path.substr(prev == std::string::npos ? 0 : prev + 1, slash - (prev == std::string::npos ? 0 : prev + 1)));
}

void MakePlan(const Mod& m, Plan& plan)
{
    const std::string root = s_dataDir + "/mods/" + m.name;
    std::vector<std::string> files, dirs;
    s_sizes.clear();
    WalkAll(root, files, dirs, NULL);

    // Folders the port never reads.
    std::vector<std::string> deadRoots;
    for (const std::string& d : dirs) {
        const std::string rel = Lower(d.substr(root.size()));
        if (rel == "/bio1dc" && !s_hasDc) deadRoots.push_back(d);
        else if (rel == "/hires/skin" || rel == "/hires/skin2" || rel == "/hires/misc" ||
                 rel == "/bio1dc/hires/skin" || rel == "/bio1dc/hires/skin2" || rel == "/bio1dc/hires/misc") {
            deadRoots.push_back(d);
        }
    }
    auto underDead = [&](const std::string& p) {
        for (const std::string& r : deadRoots) {
            if (p.size() > r.size() && strncasecmp(p.c_str(), r.c_str(), r.size()) == 0 && p[r.size()] == '/') return true;
            if (strcasecmp(p.c_str(), r.c_str()) == 0) return true;
        }
        return false;
    };
    for (const std::string& r : deadRoots) {
        std::vector<std::string> f2;
        WalkAll(r, f2, plan.junkDirs, &plan.junkBytes);
        plan.junkFiles.insert(plan.junkFiles.end(), f2.begin(), f2.end());
    }

    for (const std::string& f : files) {
        if (underDead(f)) continue;
        const std::string rel = Lower(f.substr(root.size()));
        if (EndsWith(f, ".dll") || EndsWith(f, ".asi")) {
            plan.junkFiles.push_back(f);
            plan.junkBytes += FileSize(f);
            continue;
        }
        if (EndsWith(f, ".webp")) {
            const std::string parent = ParentName(f);
            const bool inHires = rel.find("/hires/") != std::string::npos;
            if (inHires && (parent == "bgd" || parent == "mask")) {
                plan.pics.push_back(PlanPic{ f, parent == "mask" });
                plan.picBytes += FileSize(f);
            }
            continue;
        }
        if (ParentName(f) == "movie" &&
            (EndsWith(f, ".mp4") || EndsWith(f, ".avi") || EndsWith(f, ".mkv") ||
             EndsWith(f, ".mov") || EndsWith(f, ".m4v"))) {
            const int kind = vita_movie_classify(f.c_str());
            if (kind == MOVIE_REMUX) ++plan.remux;
            else if (kind == MOVIE_TRANSCODE) ++plan.transcode;
            else if (kind == MOVIE_UNSUPPORTED) ++plan.unsupported;
            if (kind == MOVIE_REMUX || kind == MOVIE_TRANSCODE) plan.movies.push_back(PlanMovie{ f, kind });
        }
    }
}

void ProgressScreen(const char* title, const char* line1, const char* line2, int done, int total,
                    double sub, const char* footer)
{
    BeginFrame();
    if (s_backdrop) Image(s_backdrop, 0, 0, VITA_SCREEN_W, VITA_SCREEN_H, 0xFF505050);
    Rect(0, 0, VITA_SCREEN_W, VITA_SCREEN_H, 0x99000000);
    Rect(0, 0, VITA_SCREEN_W, 64, 0xE0100808);
    Rect(0, 64, VITA_SCREEN_W, 2, kAccent);
    Text(24, 10, "RESIDENT EVIL", kAccent, 1.3f);
    Text(24, 38, title, kGrey, 0.8f, CharsFit(24, VITA_SCREEN_W - 24, 0.8f));

    const float barX = 60, barY = 230, barW = VITA_SCREEN_W - 120, barH = 24;
    char count[64];
    snprintf(count, sizeof(count), "%d / %d", done, total);
    Text(barX, barY - 40, count, kWhite, 1.0f);
    Rect(barX, barY, barW, barH, 0xFF303030);
    if (total > 0) Rect(barX, barY, barW * (float)done / (float)total, barH, kAccent);
    if (sub >= 0) {
        Rect(barX, barY + barH + 6, barW, 6, 0xFF303030);
        Rect(barX, barY + barH + 6, barW * (float)sub, 6, kGold);
    }
    const int fit = CharsFit(barX, barX + barW, 0.8f);
    TextWrap(barX, barY + 52, line1, kWhite, 0.8f, fit, 2, 19);
    TextWrap(barX, barY + 96, line2, kGrey, 0.8f, fit, 3, 19);

    Rect(0, VITA_SCREEN_H - 62, VITA_SCREEN_W, 62, 0xE0100808);
    Rect(0, VITA_SCREEN_H - 62, VITA_SCREEN_W, 2, kAccent);
    Text(24, VITA_SCREEN_H - 44, footer, kGrey, 0.8f, CharsFit(24, VITA_SCREEN_W - 24, 0.8f));
    Flush();
    vglSwapBuffers(GL_FALSE);
}

unsigned int PadPressed(unsigned int* prev)
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    const unsigned int pressed = pad.buttons & ~*prev;
    *prev = pad.buttons;
    return pressed;
}

// The warning. TRUE once X has been held for three seconds.
bool ConfirmOptimize(const Mod& m, const Plan& plan)
{
    char what[6][160];
    int n = 0;
    snprintf(what[n++], sizeof(what[0]), "%d HD pictures converted to fast-loading files at %d lines (%d MB of WebP replaced)",
             (int)plan.pics.size(), g_vitaVideo.hdMaxHeight, (int)(plan.picBytes >> 20));
    snprintf(what[n++], sizeof(what[0]), "%d movies converted on the Vita (%d quick re-packs, %d full re-encodes)",
             plan.remux + plan.transcode, plan.remux, plan.transcode);
    if (plan.unsupported > 0) {
        snprintf(what[n++], sizeof(what[0]), "%d movies can't be converted here and are left as they are",
                 plan.unsupported);
    }
    snprintf(what[n++], sizeof(what[0]), "%d unused files deleted (%d MB): hires/skin, skin2, misc, .dll, .asi%s",
             (int)plan.junkFiles.size(), (int)(plan.junkBytes >> 20), s_hasDc ? "" : ", bio1dc/");

    unsigned int prev = 0xFFFFFFFF;
    SceUInt64 heldSince = 0;
    float held = 0;
    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (prev == 0xFFFFFFFF) prev = pad.buttons;
        const unsigned int pressed = pad.buttons & ~prev;
        prev = pad.buttons;
        if (pressed & SCE_CTRL_CIRCLE) return false;
        // Seconds, not frames: the draw rate is not fixed.
        const SceUInt64 now = sceKernelGetProcessTimeWide();
        if (pad.buttons & SCE_CTRL_CROSS) {
            if (heldSince == 0) heldSince = now;
            held = (float)(now - heldSince) / 3e6f;
            if (held >= 1.0f) return true;
        } else {
            heldSince = 0;
            held = 0;
        }

        BeginFrame();
        if (s_backdrop) Image(s_backdrop, 0, 0, VITA_SCREEN_W, VITA_SCREEN_H, 0xFF505050);
        Rect(0, 0, VITA_SCREEN_W, VITA_SCREEN_H, 0xC0000000);
        Rect(0, 0, VITA_SCREEN_W, 64, 0xE0100808);
        Rect(0, 64, VITA_SCREEN_W, 2, kAccent);
        Text(24, 10, "OPTIMIZE FOR VITA", kAccent, 1.3f);
        Text(24, 38, m.name.c_str(), kGrey, 0.8f, CharsFit(24, VITA_SCREEN_W - 24, 0.8f));

        const int fit = CharsFit(40, VITA_SCREEN_W - 40, 0.8f);
        float y = 84;
        Rect(24, y, VITA_SCREEN_W - 48, 92, 0x80601010);
        y += 6;
        y += 19 * TextWrap(40, y, "WARNING: this overwrites the mod's own files and CANNOT be undone. "
                                  "Keep a copy on a PC if you may want the originals back (or another HD size).",
                           kYellow, 0.8f, fit, 2, 19);
        y += 19 * TextWrap(40, y, "It takes a long time - up to an hour or more for a big pack with movies. "
                                  "Keep the Vita on the charger and don't close the game.",
                           kYellow, 0.8f, fit, 2, 19);
        y = 196;
        for (int i = 0; i < n; ++i) {
            Text(40, y, "-", kWhite, 0.8f);
            y += 19 * TextWrap(58, y, what[i], kWhite, 0.8f, CharsFit(58, VITA_SCREEN_W - 40, 0.8f), 2, 19) + 6;
        }

        Rect(0, VITA_SCREEN_H - 62, VITA_SCREEN_W, 62, 0xE0100808);
        Rect(0, VITA_SCREEN_H - 62, VITA_SCREEN_W, 2, kAccent);
        Text(24, VITA_SCREEN_H - 44, "HOLD X 3 SECONDS TO START    CIRCLE CANCEL", kGrey, 0.8f);
        if (held > 0) Rect(24, VITA_SCREEN_H - 14, (VITA_SCREEN_W - 48) * held, 6, kAccent);
        Flush();
        vglSwapBuffers(GL_FALSE);
    }
}

// Shared with the worker threads.
Plan s_plan;
volatile int s_optNext = 0, s_optDone = 0, s_optFailed = 0, s_optStop = 0, s_optRunning = 0;
const int kMovieWorkers = 3;
volatile double s_movieFrac[kMovieWorkers];
volatile int s_movieCur[kMovieWorkers];       // plan index being converted, -1 idle

int PicThread(SceSize, void*)
{
    for (;;) {
        if (s_optStop) break;
        const int i = __atomic_fetch_add(&s_optNext, 1, __ATOMIC_SEQ_CST);
        if (i >= (int)s_plan.pics.size()) break;
        if (!vita_hd_optimize_picture(s_plan.pics[i].path.c_str(), s_plan.pics[i].mask ? TRUE : FALSE)) {
            __atomic_fetch_add(&s_optFailed, 1, __ATOMIC_SEQ_CST);
        }
        __atomic_fetch_add(&s_optDone, 1, __ATOMIC_SEQ_CST);
    }
    __atomic_fetch_sub(&s_optRunning, 1, __ATOMIC_SEQ_CST);
    return sceKernelExitDeleteThread(0);
}

void MovieProgress(void* user, double f) { s_movieFrac[(int)(intptr_t)user] = f; }

// One movie at a time per worker; FFmpeg itself runs single-threaded here.
int MovieThread(SceSize, void* argp)
{
    const int me = *(const int*)argp;
    for (;;) {
        if (s_optStop) break;
        const int i = __atomic_fetch_add(&s_optNext, 1, __ATOMIC_SEQ_CST);
        if (i >= (int)s_plan.movies.size()) break;
        s_movieCur[me] = i;
        s_movieFrac[me] = 0;
        const PlanMovie& mv = s_plan.movies[i];
        void* user = (void*)(intptr_t)me;
        const BOOL ok = mv.kind == MOVIE_REMUX
            ? vita_movie_remux(mv.path.c_str(), MovieProgress, user, &s_optStop)
            : vita_movie_transcode(mv.path.c_str(), MovieProgress, user, &s_optStop);
        if (!ok && !s_optStop) __atomic_fetch_add(&s_optFailed, 1, __ATOMIC_SEQ_CST);
        if (!s_optStop) __atomic_fetch_add(&s_optDone, 1, __ATOMIC_SEQ_CST);
        s_movieCur[me] = -1;
    }
    s_movieCur[me] = -1;
    __atomic_fetch_sub(&s_optRunning, 1, __ATOMIC_SEQ_CST);
    return sceKernelExitDeleteThread(0);
}

std::string BaseName(const std::string& p)
{
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// Runs the plan with a progress screen. Returns a summary for the toast.
void RunOptimize(const Mod& m, char* summary, size_t summarySize)
{
    const int clockBefore = scePowerGetArmClockFrequency();
    scePowerSetArmClockFrequency(444);
    s_optStop = s_optFailed = 0;
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    unsigned int prev = 0xFFFFFFFF;
    PadPressed(&prev);
    const char* footer = "CIRCLE  STOP (WHAT IS DONE STAYS DONE - RUN OPTIMIZE AGAIN TO FINISH)";
    char title[96];
    snprintf(title, sizeof(title), "OPTIMIZING %s", m.name.c_str());

    // 1. Unused files: quick.
    int deleted = 0;
    for (const std::string& f : s_plan.junkFiles) if (sceIoRemove(f.c_str()) >= 0) ++deleted;
    for (const std::string& d : s_plan.junkDirs) sceIoRmdir(d.c_str());

    // 2. Pictures, two cores.
    const int pics = (int)s_plan.pics.size();
    s_optNext = s_optDone = 0;
    s_optRunning = 2;
    const int cores[2] = { 0x20000, 0x40000 };      // SCE_KERNEL_CPU_MASK_USER_1 / _2
    for (int i = 0; i < 2; ++i) {
        const SceUID t = sceKernelCreateThread("re1_optpic", PicThread, 0x10000100, 0x20000, 0, cores[i], NULL);
        if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) {
            if (t >= 0) sceKernelDeleteThread(t);
            __atomic_fetch_sub(&s_optRunning, 1, __ATOMIC_SEQ_CST);
        }
    }
    while (s_optRunning > 0) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        const int done = s_optDone;
        const double secs = (double)(sceKernelGetProcessTimeWide() - start) / 1e6;
        char line2[160] = "";
        if (s_optStop) snprintf(line2, sizeof(line2), "Stopping after the pictures being converted now...");
        else if (done > 0) {
            const int left = (int)(secs / done * (pics - done) / 60.0 + 0.5);
            if (left < 1) snprintf(line2, sizeof(line2), "Less than a minute left for the pictures.");
            else snprintf(line2, sizeof(line2), "About %d min left for the pictures.", left);
        }
        ProgressScreen(title, "Converting HD pictures", line2, done, pics, -1, footer);
        if (PadPressed(&prev) & SCE_CTRL_CIRCLE) s_optStop = 1;
        sceKernelDelayThread(30000);
    }
    const int picsDone = s_optDone;

    // 3. Movies, one at a time (ffmpeg uses the cores itself).
    const int movies = (int)s_plan.movies.size();
    int moviesDone = 0;
    if (!s_optStop && movies > 0) {
        s_optNext = s_optDone = 0;
        static int ids[kMovieWorkers];
        const int cores[kMovieWorkers] = { 0x20000, 0x40000, 0x10000 };    // USER_1, USER_2, USER_0
        const int workers = movies < kMovieWorkers ? movies : kMovieWorkers;
        s_optRunning = workers;
        for (int i = 0; i < kMovieWorkers; ++i) { s_movieCur[i] = -1; s_movieFrac[i] = 0; }
        for (int i = 0; i < workers; ++i) {
            ids[i] = i;
            const SceUID t = sceKernelCreateThread("re1_optmov", MovieThread, 0x10000100, 0x100000, 0,
                                                   cores[i], NULL);
            if (t < 0 || sceKernelStartThread(t, sizeof(int), &ids[i]) < 0) {
                if (t >= 0) sceKernelDeleteThread(t);
                __atomic_fetch_sub(&s_optRunning, 1, __ATOMIC_SEQ_CST);
            }
        }
        const SceUInt64 mstart = sceKernelGetProcessTimeWide();
        while (s_optRunning > 0) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
            const int done = s_optDone;
            // The movies in flight: names, and how far along they are together.
            std::string names;
            double inFlight = 0;
            int active = 0;
            for (int i = 0; i < kMovieWorkers; ++i) {
                const int cur = s_movieCur[i];
                if (cur < 0 || cur >= movies) continue;
                if (!names.empty()) names += ", ";
                names += BaseName(s_plan.movies[cur].path);
                inFlight += s_movieFrac[i];
                ++active;
            }
            char line1[200], line2[160] = "";
            snprintf(line1, sizeof(line1), "Converting %s", names.empty() ? "movies" : names.c_str());
            const double secs = (double)(sceKernelGetProcessTimeWide() - mstart) / 1e6;
            const double progress = done + inFlight;
            if (s_optStop) {
                snprintf(line2, sizeof(line2), "Stopping; the movies being converted are left as they were.");
            } else if (progress > 0.05) {
                const int left = (int)(secs / progress * (movies - progress) / 60.0 + 0.5);
                if (left < 1) snprintf(line2, sizeof(line2), "Less than a minute left.");
                else snprintf(line2, sizeof(line2), "About %d min left for the movies.", left);
            }
            ProgressScreen(title, line1, line2, done, movies, active > 0 ? inFlight / active : -1, footer);
            if (PadPressed(&prev) & SCE_CTRL_CIRCLE) s_optStop = 1;
            sceKernelDelayThread(30000);
        }
        moviesDone = s_optDone;
    }

    if (!s_optStop) {
        // A note in the mod of what was done, for whoever looks later.
        FILE* f = fopen((s_dataDir + "/mods/" + m.name + "/vita_optimized.txt").c_str(), "w");
        if (f) {
            fprintf(f, "Optimized for the PS Vita port by its mod manager.\n"
                       "HD pictures at %d lines. This folder no longer holds the PC originals.\n",
                    g_vitaVideo.hdMaxHeight);
            fclose(f);
        }
    }
    s_plan = Plan();
    scePowerSetArmClockFrequency(clockBefore > 0 ? clockBefore : 333);

    const int failed = s_optFailed;
    snprintf(summary, summarySize, "%s: %d pictures, %d movies, %d files deleted%s",
             s_optStop ? "Stopped (SELECT again to finish)" : "Optimized", picsDone, moviesDone, deleted,
             failed ? ", some failed" : "");
}

// SELECT on the MODS page.
void OptimizeFlow(Mod& m, char* toast, size_t toastSize)
{
    ProgressScreen("OPTIMIZE FOR VITA", "Looking at the mod...", "Checking every picture and movie.", 0, 0, -1, "");
    s_plan = Plan();
    MakePlan(m, s_plan);
    if (s_plan.pics.empty() && s_plan.movies.empty() && s_plan.junkFiles.empty()) {
        snprintf(toast, toastSize, "%s has nothing left to optimize.", m.name.c_str());
        return;
    }
    if (!ConfirmOptimize(m, s_plan)) {
        s_plan = Plan();
        snprintf(toast, toastSize, "Optimize cancelled. Nothing was changed.");
        return;
    }
    RunOptimize(m, toast, toastSize);
}

void ReleaseGl(void)
{
    if (s_font) glDeleteTextures(1, &s_font);
    if (s_backdrop) glDeleteTextures(1, &s_backdrop);
    if (s_white) glDeleteTextures(1, &s_white);
    s_font = s_backdrop = s_white = s_batchTex = 0;
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

void Save(void)
{
    // Mods, in list order.
    std::string load;
    for (const Mod& m : s_mods) {
        if (!m.enabled) continue;
        if (!load.empty()) load += ",";
        load += m.name;
    }
    const char* modKeys[] = { "Load" };
    const char* modValues[] = { load.c_str() };
    vita_config_write("Mods", modKeys, modValues, 1);

    const char* assetKeys[] = { "Version" };
    const char* assetValues[] = { s_region.c_str() };
    vita_config_write("Assets", assetKeys, assetValues, 1);

    const char* gameKeys[] = { "Mode" };
    const char* gameValues[] = { s_mode.c_str() };
    vita_config_write("Game", gameKeys, gameValues, 1);

    vita_save_video_settings();
}

}  // namespace

// ---------------------------------------------------------------------------
// vita.h
// ---------------------------------------------------------------------------

BOOL vita_mod_manager_requested(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != NULL && strstr(argv[i], "-modmanager") != NULL) return TRUE;
    }
    if (LiveAreaAsked()) return TRUE;
#ifdef RE1_AUTODOOR
    // Vita3K test build: the pad is read before the window has focus there.
    if (FileExists(s_dataDir + "/modmanager.flag")) {
        sceIoRemove((s_dataDir + "/modmanager.flag").c_str());
        return TRUE;
    }
#endif

    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    return (pad.buttons & SCE_CTRL_SELECT) ? TRUE : FALSE;
}

// The LiveArea link tapped while the game is already running: no relaunch
// happens by itself, the running app just gets the event. Polled from input.
BOOL vita_mod_manager_event(void)
{
    static int s_tick = 0;
    if (++s_tick < 30) return FALSE;    // twice a second is plenty
    s_tick = 0;
    return LiveAreaAsked() ? TRUE : FALSE;
}

void vita_mod_manager_relaunch(void)
{
    vita_save_video_settings();
    char arg[] = "-modmanager";
    char* argv[] = { arg, NULL };
    sceAppMgrLoadExec("app0:eboot.bin", argv, NULL);
}

void vita_mod_manager_run(void)
{
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    s_hasUsa = DirExists(s_dataDir + "/USA");
    s_hasJpn = DirExists(s_dataDir + "/JPN");
    s_hasDc  = DirExists(s_dataDir + "/DC");
    s_region = GetAssetVersion() ? "JPN" : "USA";
    if (s_region == "JPN" && !s_hasJpn && s_hasUsa) s_region = "USA";
    if (s_region == "USA" && !s_hasUsa && s_hasJpn) s_region = "JPN";
    s_mode = (strcasecmp(GetAssetModeName(), "DC") == 0) ? "DC" : "OG";

    MakeFont();
    MakeBackdrop();

    // First frame up before the scan, which can take a moment on a big pack.
    Draw(PAGE_MODS, 0, 0, "Looking at the mods folder...");
    LoadMods();
    AnalyseAll();

    int page = PAGE_MODS, cursor = 0, scroll = 0;
    unsigned int prev = 0xFFFFFFFF;     // swallow whatever was held at boot
    char toast[120] = "";
    int toastFrames = 0;
    unsigned int repeatButton = 0;
    int repeatFrames = 0;

    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int held = pad.buttons;
        if (pad.ly < 40) held |= SCE_CTRL_UP;
        if (pad.ly > 216) held |= SCE_CTRL_DOWN;
        if (pad.lx < 40) held |= SCE_CTRL_LEFT;
        if (pad.lx > 216) held |= SCE_CTRL_RIGHT;
        if (prev == 0xFFFFFFFF) { prev = held; }
        unsigned int pressed = held & ~prev;
        prev = held;

        // Held d-pad repeats.
        const unsigned int dirs = held & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
        if (dirs != 0 && dirs == repeatButton) {
            if (++repeatFrames > 20 && (repeatFrames % 5) == 0) pressed |= dirs;
        } else {
            repeatButton = dirs;
            repeatFrames = 0;
        }

        const int count = page == PAGE_MODS ? (int)s_mods.size() : (int)kPageRows[page].size();

        if (pressed & SCE_CTRL_START) break;
        if (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
            page = (page + ((pressed & SCE_CTRL_RTRIGGER) ? 1 : PAGE_COUNT - 1)) % PAGE_COUNT;
            cursor = 0;
            scroll = 0;
        }
        if (count > 0) {
            if (pressed & SCE_CTRL_UP)   cursor = (cursor + count - 1) % count;
            if (pressed & SCE_CTRL_DOWN) cursor = (cursor + 1) % count;
        }
        if (cursor < scroll) scroll = cursor;
        if (cursor >= scroll + 8) scroll = cursor - 7;

        if (page == PAGE_MODS && count > 0) {
            bool changed = false;
            if (pressed & SCE_CTRL_CROSS) {
                s_mods[cursor].enabled = !s_mods[cursor].enabled;
                changed = true;
            }
            if ((pressed & SCE_CTRL_SQUARE) && cursor > 0) {
                std::swap(s_mods[cursor], s_mods[cursor - 1]);
                --cursor;
                changed = true;
            }
            if ((pressed & SCE_CTRL_TRIANGLE) && cursor + 1 < count) {
                std::swap(s_mods[cursor], s_mods[cursor + 1]);
                ++cursor;
                changed = true;
            }
            if (pressed & SCE_CTRL_SELECT) {
                OptimizeFlow(s_mods[cursor], toast, sizeof(toast));
                toastFrames = 600;
                changed = true;
                prev = 0xFFFFFFFF;          // the buttons held through it
            }
            if (changed) AnalyseAll();
        } else if (count > 0) {
            const Row r = kPageRows[page][cursor];
            if (pressed & SCE_CTRL_LEFT) RowChange(r, -1);
            if (pressed & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) RowChange(r, +1);
        }

        if (toastFrames > 0 && --toastFrames == 0) toast[0] = '\0';
        Draw(page, cursor, scroll, toast);
    }

    Draw(page, cursor, scroll, "Saving and starting the game...");
    Save();
    ReleaseGl();
}

