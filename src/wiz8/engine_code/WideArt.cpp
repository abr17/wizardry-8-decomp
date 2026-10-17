/* Wide full-screen backgrounds (mod, 2026-10-12). Not part of the original game.
   What and why - in include/wiz8/wide_art.h and wiz8-widescreen docs/wide-canvas.md. */
#include "wiz8/wide_art.h"
#include "wiz8/canvas.h"
#include "wiz8/layouts/screen_state.h"

#include "wiz8/wiz8_windows.h"
#include "Types.h"
#include "himage.h"
#include "FileMan.h"

#include <stdio.h>
#include <string.h>

/* Video2.cpp: the whole canvas surface and marking of the side bars. */
extern void* LockPrimarySurfaceFull(unsigned int* pitch);
extern void UnlockPrimarySurface(void);
extern void InvalidateCanvasSideBars(unsigned int flags);

namespace {

enum {
    WIDE_ART_PATH = 96,     /* an original path from the catalog is shorter than 0x30 */
    WIDE_ART_ENTRIES = 64,  /* how many different paths we remember */
    WIDE_ART_DECODED = 16   /* how many images we keep in memory (~0.8 MB each);
                               with stretched panels (2026-10-16) there are ~10 on screen */
};

/* Cache entry. A JS analogy: Map<path, {state, pixels}>, where "no pair"
   is also a value, so the file is not searched for again. */
struct WideArtEntry {
    char path[WIDE_ART_PATH]; /* original path; empty - the entry is free */
    int state;                /* 0 - not searched yet, 1 - pair exists, -1 - none/unsuitable */
    unsigned short* pixels;   /* W x 480 or 0 (unloaded to save memory - will be read again) */
    unsigned int last_use;    /* for evicting the least recently used */
};

WideArtEntry s_entries[WIDE_ART_ENTRIES];
unsigned int s_use_clock;
bool s_on_canvas;           /* a wide background is currently in the canvas side bars */
unsigned short* s_overlay;  /* BuildWideFullscreenOverlay buffer (W x 480, permanent) */

/* Stone wings: the "original" path from which the pair name is built
   (Data\Wide\WideFrame_768.sti). WideFrame.sti itself does not exist in the game. */
const char WINGS_PATH[] = "Data\\Wide\\WideFrame.sti";
bool s_wings_on;                 /* the wings are currently in the side bars */
unsigned int s_wings_clear_count; /* CanvasSideBarsClearCount() at the time of drawing */

/* UI window screens made of marble panels, whose margins are covered by
   wings. Main screen (layout A), menu, loading, credits - no. */
bool ScreenWantsWings(int screen)
{
    switch (screen) {
    case W8_SCREEN_CAMP: /* inventory and stats (RCS), both from the game and at camp */
    case W8_SCREEN_CHARACTER:
    case W8_SCREEN_PARTY_SELECTION:
    case W8_SCREEN_AUTOMAP:
    case W8_SCREEN_OPTIONS:
    case W8_SCREEN_JOURNAL:
        return true;
    default:
        return false;
    }
}

/* "Data\\Level Load\\Arnika.sti" -> "Data\\Level Load\\Arnika_768.sti". */
bool MakeWidePath(const char* path, char* out, size_t size)
{
    const char* dot = strrchr(path, '.');
    const char* slash = strrchr(path, '\\');
    if (dot == 0 || (slash != 0 && dot < slash)) {
        return false;
    }
    int written = _snprintf(out, size, "%.*s_%d%s", static_cast<int>(dot - path), path,
                            g_canvas_w, dot);
    return written > 0 && static_cast<size_t>(written) < size;
}

/* Read the pair with the game's own STI loader: it converts the file's 16-bit pixels
   (RGB 5:6:5) to the screen format exactly as for the original
   surfaces (black stays 0, the rest 5:5:5 + alpha bit). */
unsigned short* LoadWideArt(const char* wide_path)
{
    char text[200];
    SGPFILENAME name;
    _snprintf(name, sizeof(name), "%s", wide_path);
    name[sizeof(name) - 1] = '\0';
    if (!FileExists(name)) {
        _snprintf(text, sizeof(text), "[WideArt] %s: no file, original 640x480 is used", name);
        text[sizeof(text) - 1] = '\0';
        CanvasLogLine(text);
        return 0;
    }
    HIMAGE image = CreateImage(name, IMAGE_ALLIMAGEDATA);
    if (image == 0) {
        _snprintf(text, sizeof(text), "[WideArt] %s: cannot read the STI", name);
        text[sizeof(text) - 1] = '\0';
        CanvasLogLine(text);
        return 0;
    }
    unsigned short* pixels = 0;
    if (image->ubBitDepth == 16 && image->usWidth == g_canvas_w && image->usHeight == W8_GAME_H &&
        (image->fFlags & IMAGE_BITMAPDATA) != 0 && (image->fFlags & IMAGE_COMPRESSED) == 0 &&
        image->p16BPPData != 0) {
        size_t count = static_cast<size_t>(g_canvas_w) * W8_GAME_H;
        pixels = new unsigned short[count];
        memcpy(pixels, image->p16BPPData, count * sizeof(unsigned short));
        _snprintf(text, sizeof(text), "[WideArt] %s: loaded %dx%d", name, g_canvas_w, W8_GAME_H);
    } else {
        _snprintf(text, sizeof(text),
                  "[WideArt] %s: ignored, need 16-bit %dx%d, file is %d-bit %dx%d", name,
                  g_canvas_w, W8_GAME_H, image->ubBitDepth, image->usWidth, image->usHeight);
    }
    text[sizeof(text) - 1] = '\0';
    CanvasLogLine(text);
    DestroyImage(image);
    return pixels;
}

/* Free the pixels of the least recently used image if there are already too many
   in memory (keep - the current one, left alone). The entry stays "pair exists". */
void TrimDecoded(const WideArtEntry* keep)
{
    int decoded = 0;
    WideArtEntry* oldest = 0;
    for (int index = 0; index < WIDE_ART_ENTRIES; ++index) {
        WideArtEntry* entry = &s_entries[index];
        if (entry->pixels == 0) {
            continue;
        }
        ++decoded;
        if (entry != keep && (oldest == 0 || entry->last_use < oldest->last_use)) {
            oldest = entry;
        }
    }
    if (decoded > WIDE_ART_DECODED && oldest != 0) {
        delete[] oldest->pixels;
        oldest->pixels = 0;
    }
}

WideArtEntry* FindEntry(const char* path)
{
    WideArtEntry* free_entry = 0;
    WideArtEntry* oldest = 0;
    for (int index = 0; index < WIDE_ART_ENTRIES; ++index) {
        WideArtEntry* entry = &s_entries[index];
        if (entry->path[0] == '\0') {
            if (free_entry == 0) {
                free_entry = entry;
            }
            continue;
        }
        if (_stricmp(entry->path, path) == 0) {
            return entry;
        }
        if (oldest == 0 || entry->last_use < oldest->last_use) {
            oldest = entry;
        }
    }
    /* New path: a free entry or the least recently used one (its image is dropped). */
    WideArtEntry* entry = free_entry != 0 ? free_entry : oldest;
    delete[] entry->pixels;
    memset(entry, 0, sizeof(*entry));
    _snprintf(entry->path, sizeof(entry->path), "%s", path);
    entry->path[sizeof(entry->path) - 1] = '\0';
    return entry;
}

} // namespace

const unsigned short* FindWideFullscreenArt(const char* path)
{
    if (g_canvas_ox <= 0 || path == 0 || strlen(path) >= WIDE_ART_PATH) {
        return 0;
    }
    WideArtEntry* entry = FindEntry(path);
    entry->last_use = ++s_use_clock;
    if (entry->state < 0) {
        return 0;
    }
    if (entry->pixels == 0) {
        char wide_path[WIDE_ART_PATH + 16];
        entry->pixels = MakeWidePath(path, wide_path, sizeof(wide_path)) ? LoadWideArt(wide_path)
                                                                          : 0;
        entry->state = entry->pixels != 0 ? 1 : -1;
        TrimDecoded(entry);
    }
    return entry->pixels;
}

bool DrawWideFullscreenArt(const char* path)
{
    const unsigned short* pixels = FindWideFullscreenArt(path);
    if (pixels == 0) {
        return false;
    }
    unsigned int pitch = 0;
    unsigned char* surface = static_cast<unsigned char*>(LockPrimarySurfaceFull(&pitch));
    if (surface == 0) {
        return false;
    }
    /* Like SGP's 16->16 blit: a copy without transparency, only over the whole canvas. */
    size_t row_bytes = static_cast<size_t>(g_canvas_w) * sizeof(unsigned short);
    for (int y = 0; y < W8_GAME_H; ++y) {
        memcpy(surface + static_cast<size_t>(y) * pitch,
               pixels + static_cast<size_t>(y) * g_canvas_w, row_bytes);
    }
    UnlockPrimarySurface();
    s_on_canvas = true;
    s_wings_on = false;
    CanvasNoteSideBarsCleared(); /* bars overwritten: the wings are no longer there */
    InvalidateCanvasSideBars(0);
    return true;
}

bool RestoreWideFullscreenArtRect(const char* path, int left, int top, int right, int bottom)
{
    const unsigned short* pixels = FindWideFullscreenArt(path);
    if (pixels == 0) {
        return false;
    }
    int x0 = left + g_canvas_ox;
    int x1 = right + g_canvas_ox;
    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 > g_canvas_w) {
        x1 = g_canvas_w;
    }
    if (top < 0) {
        top = 0;
    }
    if (bottom > W8_GAME_H) {
        bottom = W8_GAME_H;
    }
    if (x0 >= x1 || top >= bottom) {
        return true;
    }
    unsigned int pitch = 0;
    unsigned char* surface = static_cast<unsigned char*>(LockPrimarySurfaceFull(&pitch));
    if (surface == 0) {
        return false;
    }
    size_t bytes = static_cast<size_t>(x1 - x0) * sizeof(unsigned short);
    for (int y = top; y < bottom; ++y) {
        memcpy(surface + static_cast<size_t>(y) * pitch + static_cast<size_t>(x0) * 2,
               pixels + static_cast<size_t>(y) * g_canvas_w + x0, bytes);
    }
    UnlockPrimarySurface();
    return true;
}

bool WideFullscreenArtOnCanvas(void)
{
    return s_on_canvas;
}

void ReleaseWideFullscreenArt(void)
{
    if (!s_on_canvas && !s_wings_on) {
        return;
    }
    s_on_canvas = false;
    s_wings_on = false;
    CanvasClearSideBars();
    InvalidateCanvasSideBars(0);
}

void UpdateWideSideWings(int screen)
{
    if (g_canvas_ox <= 0 || s_on_canvas || CanvasWideLayoutActive() || !ScreenWantsWings(screen)) {
        return;
    }
    if (s_wings_on && s_wings_clear_count == CanvasSideBarsClearCount()) {
        return; /* they are there and nobody wiped them */
    }
    const unsigned short* pixels = FindWideFullscreenArt(WINGS_PATH);
    if (pixels == 0) {
        return; /* no wings file for this width - black margins, as before */
    }
    unsigned int pitch = 0;
    unsigned char* surface = static_cast<unsigned char*>(LockPrimarySurfaceFull(&pitch));
    if (surface == 0) {
        return;
    }
    /* Only the bars: x < ox and x >= ox + 640. The screen itself draws the 640 window. */
    size_t bar_bytes = static_cast<size_t>(g_canvas_ox) * sizeof(unsigned short);
    size_t right = static_cast<size_t>(g_canvas_ox + W8_GAME_W);
    for (int y = 0; y < W8_GAME_H; ++y) {
        unsigned char* line = surface + static_cast<size_t>(y) * pitch;
        const unsigned short* source = pixels + static_cast<size_t>(y) * g_canvas_w;
        memcpy(line, source, bar_bytes);
        memcpy(line + right * sizeof(unsigned short), source + right, bar_bytes);
    }
    UnlockPrimarySurface();
    s_wings_on = true;
    s_wings_clear_count = CanvasSideBarsClearCount();
    InvalidateCanvasSideBars(0);
}

bool DrawWideMarbleGap(int left, int top, int right, int bottom, int brightness, bool bevels)
{
    const int width = right - left;
    if (width <= 0 || width > 2 * g_canvas_ox) {
        return false;
    }
    const unsigned short* marble = FindWideFullscreenArt(WINGS_PATH);
    if (marble == 0) {
        return false;
    }
    int x0 = left < 0 ? 0 : left;
    int x1 = right > g_canvas_w ? g_canvas_w : right;
    int y0 = top < 0 ? 0 : top;
    int y1 = bottom > W8_GAME_H ? W8_GAME_H : bottom;
    if (x0 >= x1 || y0 >= y1) {
        return true;
    }
    unsigned int pitch = 0;
    unsigned char* surface = static_cast<unsigned char*>(LockPrimarySurfaceFull(&pitch));
    if (surface == 0) {
        return false;
    }
    for (int y = y0; y < y1; ++y) {
        // reinterpret-ok: a row of the locked 16-bit surface
        unsigned short* line = reinterpret_cast<unsigned short*>(surface + static_cast<size_t>(y) * pitch);
        const unsigned short* source = marble + static_cast<size_t>(y) * g_canvas_w;
        for (int x = x0; x < x1; ++x) {
            int column = x - left;
            int art_x;
            if (bevels) {
                /* Like FillZoneGaps (Video2.cpp): a wing bevel next to each neighbor. */
                art_x = column < width / 2 ? g_canvas_ox + W8_GAME_W + column
                                           : g_canvas_ox - (width - column);
            } else {
                /* Middle of the left wing [8, ox - 8), mirrored repeats: no bevels
                   and no seams. */
                const int span = g_canvas_ox - 16 > 1 ? g_canvas_ox - 16 : 1;
                int phase = column % (2 * span);
                art_x = 8 + (phase < span ? phase : 2 * span - 1 - phase);
            }
            unsigned short pixel = source[art_x];
            if (brightness < 256 && pixel != 0) {
                /* Surface is 5:5:5 + alpha bit (0x8000), channels darken equally. */
                unsigned int r = (((pixel >> 10) & 31u) * brightness + 128) >> 8;
                unsigned int g = (((pixel >> 5) & 31u) * brightness + 128) >> 8;
                unsigned int b = ((pixel & 31u) * brightness + 128) >> 8;
                pixel = static_cast<unsigned short>((pixel & 0x8000u) | (r << 10) | (g << 5) | b);
            }
            line[x] = pixel;
        }
    }
    UnlockPrimarySurface();
    return true;
}

bool DrawWideArtPiece(const char* path, int src_left, int src_top, int width, int height,
                      int dst_left, int dst_top)
{
    const unsigned short* pixels = FindWideFullscreenArt(path);
    if (pixels == 0) {
        return false;
    }
    /* Clipping: the source within the W x 480 image, the destination within the canvas. */
    if (src_left < 0) {
        width += src_left;
        dst_left -= src_left;
        src_left = 0;
    }
    if (src_top < 0) {
        height += src_top;
        dst_top -= src_top;
        src_top = 0;
    }
    if (dst_left < 0) {
        width += dst_left;
        src_left -= dst_left;
        dst_left = 0;
    }
    if (dst_top < 0) {
        height += dst_top;
        src_top -= dst_top;
        dst_top = 0;
    }
    if (src_left + width > g_canvas_w) {
        width = g_canvas_w - src_left;
    }
    if (dst_left + width > g_canvas_w) {
        width = g_canvas_w - dst_left;
    }
    if (src_top + height > W8_GAME_H) {
        height = W8_GAME_H - src_top;
    }
    if (dst_top + height > W8_GAME_H) {
        height = W8_GAME_H - dst_top;
    }
    if (width <= 0 || height <= 0) {
        return true;
    }
    unsigned int pitch = 0;
    unsigned char* surface = static_cast<unsigned char*>(LockPrimarySurfaceFull(&pitch));
    if (surface == 0) {
        return false;
    }
    size_t bytes = static_cast<size_t>(width) * sizeof(unsigned short);
    for (int y = 0; y < height; ++y) {
        memcpy(surface + static_cast<size_t>(dst_top + y) * pitch + static_cast<size_t>(dst_left) * 2,
               pixels + static_cast<size_t>(src_top + y) * g_canvas_w + src_left, bytes);
    }
    UnlockPrimarySurface();
    return true;
}

bool DrawWideStretchStrip(int object, int frame, int image, int left, int top, int width,
                          int cut_left, int cut_right, int clip_left, int clip_top,
                          int clip_right, int clip_bottom)
{
    if (g_canvas_ox <= 0 || cut_right <= cut_left) {
        return false;
    }
    char path[64];
    _snprintf(path, sizeof(path), "Data\\Wide\\Stretch\\S%03X_%d_%d.sti", object, frame, image);
    path[sizeof(path) - 1] = '\0';
    const unsigned short* pixels = FindWideFullscreenArt(path);
    if (pixels == 0) {
        return false;
    }
    const int ox2 = 2 * g_canvas_ox;
    const int slice = cut_right - cut_left;
    const int strip_w = width + ox2;
    unsigned int pitch = 0;
    unsigned char* surface = static_cast<unsigned char*>(LockPrimarySurfaceFull(&pitch));
    if (surface == 0) {
        return false;
    }
    for (int row = 0; row < W8_GAME_H; ++row) {
        const int y = top + row;
        if (y < 0 || y >= W8_GAME_H || y < clip_top || y >= clip_bottom) {
            continue;
        }
        const unsigned short* source = pixels + static_cast<size_t>(row) * g_canvas_w;
        // reinterpret-ok: a row of the locked 16-bit surface
        unsigned short* line = reinterpret_cast<unsigned short*>(surface + static_cast<size_t>(y) * pitch);
        for (int column = 0; column < strip_w && column < g_canvas_w; ++column) {
            const int x = left + column;
            if (x < 0 || x >= g_canvas_w || source[column] == 0) {
                continue; /* 0 - transparent pixel, as in the image itself */
            }
            /* The same 640-window x as for the slice repeat in DrawCatalogImageWide: by
               it, clipping passes or rejects exactly the same columns. */
            int game_x;
            if (column < cut_right) {
                game_x = left + column;
            } else if (column >= cut_right + ox2) {
                game_x = left + column - ox2;
            } else {
                game_x = left + cut_left + (column - cut_right) % slice;
            }
            if (game_x < clip_left || game_x >= clip_right) {
                continue;
            }
            line[x] = source[column];
        }
    }
    UnlockPrimarySurface();
    return true;
}

const unsigned short* BuildWideFullscreenOverlay(const char* path)
{
    /* No pair (or W = 640) - 0: the caller keeps the original 640x480,
       as before the mod (on a wide screen it is stretched). */
    const unsigned short* wide = FindWideFullscreenArt(path);
    if (wide == 0) {
        return 0;
    }
    /* Own copy: the cache pixels may be unloaded by the next lookup,
       while the camp layer lives longer. */
    size_t count = static_cast<size_t>(g_canvas_w) * W8_GAME_H;
    if (s_overlay == 0) {
        s_overlay = new unsigned short[count];
    }
    memcpy(s_overlay, wide, count * sizeof(unsigned short));
    return s_overlay;
}
