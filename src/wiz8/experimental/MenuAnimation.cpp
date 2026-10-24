/* Experimental (2026-10-20, the patch author's request): an animated main menu background.
   Not part of the original game; see include/wiz8/experimental.h for the rules of the
   experimental features (off by default, removable whole).

   The video: Data\WIDE\MENU_ANIM.AVI, 16-bit ZMBV (the DOSBox capture codec, lossless
   RGB 5:6:5 in an AVI file), 856x480, looped. ZMBV is simple: every frame is zlib data
   (one zlib stream from keyframe to keyframe), a keyframe holds the whole picture, any other
   frame holds for each 16x16 block a motion vector into the previous picture plus an optional
   XOR difference. zlib is already in the game. The whole file is read into memory on menu
   entry and decoded frame by frame; the 768 and 640 canvases show the centre of the picture.

   The menu items (Data\Options\introtext_*.sti) are opaque strips with a piece of the
   original background baked in; over a moving background they would show as rectangles.
   Only their letters, outline and glow are drawn: a pixel is kept where it differs from the
   original background under it (the rule of the 1.0.0 art script menutext_alpha.py:
   3x3 median of the difference above a threshold, small specks dropped, a thin fringe kept,
   and every state keeps at least the letter shapes of the normal state). The masks are made
   once per session from the item pictures of the installed language. */
#include "wiz8/experimental.h"
#include "wiz8/canvas.h"
#include "wiz8/fonts.h"
#include "wiz8/utility.h"
#include "wiz8/video_object_catalog.h"
#include "wiz8/local_screens/MainMenuScreen.h"
#include "wiz8/wiz8_windows.h"

#include "Types.h"
#include "Font.h"
#include "himage.h"
#include "vobject.h"
#include "zlib.h"

#include <string.h>

/* Video2.cpp: the whole canvas surface and marking of the side bars. */
extern void* LockPrimarySurfaceFull(unsigned int* pitch);
extern void UnlockPrimarySurface(void);
extern void ResetTransientRenderScenes(void);
extern void InvalidateCanvasSideBars(unsigned int flags);
/* Compression.cpp (the game's own zlib allocators). */
voidpf ZAlloc(voidpf opaque, uInt items, uInt size);
void ZFree(voidpf opaque, voidpf address);

namespace {

/* ---------------- settings ---------------- */

int s_setting = -1; /* -1 not read yet, else 0/1 */

void Log(const char* text)
{
    char line[256];
    line[0] = '\0';
    strcat(line, "[MenuAnim] ");
    strncat(line, text, sizeof(line) - strlen(line) - 1);
    CanvasLogLine(line);
}

/* ASCII-only case-insensitive "ends with" (no CRT locale functions in the hybrid). */
bool EndsWithNoCase(const char* text, const char* tail)
{
    size_t a = strlen(text);
    size_t b = strlen(tail);
    if (b > a) {
        return false;
    }
    text += a - b;
    for (size_t i = 0; i < b; ++i) {
        char x = text[i];
        char y = tail[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) {
            return false;
        }
    }
    return true;
}

/* ---------------- the video ---------------- */

enum {
    VIDEO_W = 856,
    VIDEO_H = 480,
    BORDER = 16, /* ZMBV keeps 16 pixels around the picture for motion vectors */
    PITCH = VIDEO_W + 2 * BORDER,
    ROWS = VIDEO_H + 2 * BORDER,
    MAX_FRAMES = 4096,
    MAX_BLOCKS = (VIDEO_W / 4 + 1) * (VIDEO_H / 4 + 1)
};

struct Block {
    int start; /* offset of the block's top-left pixel in a bordered frame */
    int w, h;
};

unsigned char* s_file;        /* the whole AVI */
const unsigned char* s_chunk[MAX_FRAMES];
unsigned int s_chunk_size[MAX_FRAMES];
int s_frames;                 /* frames in the file */
unsigned int s_period_us;     /* microseconds per frame */
unsigned short* s_buffer[2];  /* bordered pictures (RGB 5:6:5) */
int s_current;                /* which buffer holds the decoded picture */
int s_decoded;                /* index of the decoded frame, -1 none */
unsigned char* s_work;        /* inflated frame data */
unsigned int s_work_size;
z_stream s_zlib;
bool s_zlib_ready;
Block* s_blocks;
int s_block_count;
unsigned short* s_lut;        /* RGB 5:6:5 -> surface format (5:5:5 + alpha bit, black 0) */
unsigned short s_alpha = 0x8000; /* the surface's alpha bit, read from a loaded picture */
bool s_active;
bool s_frozen;                /* a frame failed to decode: the last good picture stays */
bool s_broken;                /* ... and the file is not tried again this session */
DWORD s_start_tick;           /* tick that corresponds to s_start_frame */
long s_start_frame;           /* frames counted from the start of this menu entry */
long s_shown;                 /* frame count drawn on the canvas */
int s_resume_frame;           /* where the previous entry stopped */

/* AVI: RIFF chunks; the frames are "00dc"/"00db" chunks inside LIST "movi". */
bool ParseAvi(unsigned int size)
{
    const unsigned char* p = s_file;
    if (size < 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "AVI ", 4) != 0) {
        return false;
    }
    bool zmbv = false;
    int width = 0;
    int height = 0;
    s_frames = 0;
    s_period_us = 0;
    /* A small explicit stack of LIST ranges (JS analogy: walking a tree without recursion). */
    unsigned int stack_end[8];
    unsigned int depth = 0;
    unsigned int pos = 12;
    unsigned int end = size;
    for (;;) {
        while (depth > 0 && pos + 8 > end) {
            pos = end;
            end = stack_end[--depth];
        }
        if (pos + 8 > end) {
            break;
        }
        const unsigned char* c = p + pos;
        unsigned int length = c[4] | (c[5] << 8) | (c[6] << 16) | (static_cast<unsigned int>(c[7]) << 24);
        unsigned int body = pos + 8;
        if (length > end - body) {
            return false;
        }
        if ((memcmp(c, "LIST", 4) == 0 || memcmp(c, "RIFF", 4) == 0) && length >= 4 && depth < 8) {
            stack_end[depth++] = end;
            end = body + length;
            pos = body + 4;
            continue;
        }
        if (memcmp(c, "avih", 4) == 0 && length >= 4) {
            s_period_us = c[8] | (c[9] << 8) | (c[10] << 16) | (static_cast<unsigned int>(c[11]) << 24);
        } else if (memcmp(c, "strf", 4) == 0 && length >= 20 && width == 0) {
            width = c[12] | (c[13] << 8);
            int h = static_cast<int>(c[16] | (c[17] << 8) | (c[18] << 16) | (static_cast<unsigned int>(c[19]) << 24));
            height = h < 0 ? -h : h;
            zmbv = memcmp(c + 24, "ZMBV", 4) == 0;
        } else if ((c[2] == 'd' && (c[3] == 'c' || c[3] == 'b')) && s_frames < MAX_FRAMES) {
            s_chunk[s_frames] = p + body;
            s_chunk_size[s_frames] = length;
            ++s_frames;
        }
        pos = body + length + (length & 1);
    }
    if (!zmbv || width != VIDEO_W || height != VIDEO_H || s_frames == 0) {
        char text[160];
        wsprintfA(text, "MENU_ANIM.AVI: need ZMBV %dx%d, file is %s %dx%d with %d frames", VIDEO_W,
                  VIDEO_H, zmbv ? "ZMBV" : "not ZMBV", width, height, s_frames);
        Log(text);
        return false;
    }
    if (s_period_us < 10000 || s_period_us > 1000000) {
        s_period_us = 66667; /* 15 fps */
    }
    return true;
}

/* zlib 1.0.4 (the game's) reads past its tables on some damaged streams (huft_build uses an
   unset part of v[]; later zlib fixed it): a damaged file must not crash the game. No C++
   objects here, so structured exception handling (__try) is allowed. */
const int kInflateCrashed = -1000;

int SafeInflate(z_stream* stream)
{
    __try {
        return inflate(stream, Z_SYNC_FLUSH);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kInflateCrashed;
    }
}

bool IsKeyframe(int index)
{
    return s_chunk_size[index] > 0 && (s_chunk[index][0] & 1) != 0;
}

/* Decode frame 'index' on top of the decoded one (index must be its successor or a
   keyframe). false - broken data: the animation stops. */
bool DecodeFrame(int index)
{
    const unsigned char* data = s_chunk[index];
    unsigned int size = s_chunk_size[index];
    if (size == 0) { /* an empty chunk repeats the previous picture */
        s_decoded = index;
        return true;
    }
    unsigned char tag = data[0];
    unsigned int pos = 1;
    bool key = (tag & 1) != 0;
    bool compressed = true;
    if (key) {
        if (size < 7 || data[1] != 0 || data[2] != 1 || data[4] != 6 || data[5] == 0 || data[6] == 0) {
            Log("bad keyframe header (need ZMBV 0.1, 16 bits)");
            return false;
        }
        compressed = data[3] == 1;
        int bw = data[5];
        int bh = data[6];
        s_block_count = 0;
        for (int y = 0; y < VIDEO_H; y += bh) {
            for (int x = 0; x < VIDEO_W; x += bw) {
                if (s_block_count >= MAX_BLOCKS) {
                    Log("too many blocks");
                    return false;
                }
                Block* block = &s_blocks[s_block_count++];
                block->start = (y + BORDER) * PITCH + x + BORDER;
                block->w = VIDEO_W - x < bw ? VIDEO_W - x : bw;
                block->h = VIDEO_H - y < bh ? VIDEO_H - y : bh;
            }
        }
        if (s_zlib_ready) {
            inflateEnd(&s_zlib);
            s_zlib_ready = false;
        }
        memset(&s_zlib, 0, sizeof(s_zlib));
        s_zlib.zalloc = ZAlloc;
        s_zlib.zfree = ZFree;
        if (compressed) {
            if (inflateInit(&s_zlib) != Z_OK) {
                Log("zlib init failed");
                return false;
            }
            s_zlib_ready = true;
        }
        pos = 7;
    } else if (s_decoded < 0 || s_block_count == 0) {
        return false; /* a delta frame needs the previous picture */
    }

    const unsigned char* work = data + pos;
    unsigned int used = size - pos;
    if (s_zlib_ready) {
        s_zlib.next_in = const_cast<unsigned char*>(data + pos);
        s_zlib.avail_in = size - pos;
        s_zlib.next_out = s_work;
        s_zlib.avail_out = s_work_size;
        int result = SafeInflate(&s_zlib);
        if (result == kInflateCrashed) {
            s_zlib_ready = false; /* its state is unknown: not even inflateEnd */
            Log("zlib crashed on damaged data");
            return false;
        }
        if ((result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) || s_zlib.avail_in != 0) {
            Log("zlib data error");
            return false;
        }
        work = s_work;
        used = s_work_size - s_zlib.avail_out;
    }

    if (key) {
        if (used < static_cast<unsigned int>(VIDEO_W * VIDEO_H * 2)) {
            Log("keyframe too short");
            return false;
        }
        unsigned short* picture = s_buffer[s_current];
        for (int y = 0; y < VIDEO_H; ++y) {
            memcpy(picture + (y + BORDER) * PITCH + BORDER, work + y * VIDEO_W * 2, VIDEO_W * 2);
        }
    } else {
        const unsigned short* old_picture = s_buffer[s_current];
        unsigned short* new_picture = s_buffer[s_current ^ 1];
        unsigned int vectors = static_cast<unsigned int>(s_block_count) * 2;
        unsigned int xor_pos = (vectors + 3) & ~3u;
        if (used < xor_pos) {
            Log("delta frame too short");
            return false;
        }
        for (int i = 0; i < s_block_count; ++i) {
            const Block* block = &s_blocks[i];
            int vx = static_cast<signed char>(work[i * 2]);
            int vy = static_cast<signed char>(work[i * 2 + 1]);
            bool has_xor = (vx & 1) != 0;
            vx >>= 1; /* arithmetic shift, as DOSBox */
            vy >>= 1;
            if (vx < -BORDER || vx > BORDER || vy < -BORDER || vy > BORDER) {
                Log("motion vector out of range");
                return false;
            }
            const unsigned short* src = old_picture + block->start + vy * PITCH + vx;
            unsigned short* dst = new_picture + block->start;
            for (int y = 0; y < block->h; ++y) {
                if (has_xor) {
                    if (xor_pos + static_cast<unsigned int>(block->w) * 2 > used) {
                        Log("XOR data past the end");
                        return false;
                    }
                    const unsigned char* delta = work + xor_pos;
                    for (int x = 0; x < block->w; ++x) {
                        dst[x] = static_cast<unsigned short>(src[x] ^ (delta[2 * x] | (delta[2 * x + 1] << 8)));
                    }
                    xor_pos += static_cast<unsigned int>(block->w) * 2;
                } else {
                    memcpy(dst, src, static_cast<size_t>(block->w) * 2);
                }
                src += PITCH;
                dst += PITCH;
            }
        }
        s_current ^= 1;
    }
    s_decoded = index;
    return true;
}

/* Bring the decoder to frame 'index' (any index): from the decoded frame forward, or from
   the nearest keyframe at or before it. */
bool SeekTo(int index)
{
    if (s_decoded == index) {
        return true;
    }
    int from = s_decoded >= 0 && s_decoded < index ? s_decoded + 1 : -1;
    if (from < 0) {
        from = index;
        while (from > 0 && !IsKeyframe(from)) {
            --from;
        }
        if (!IsKeyframe(from)) {
            Log("no keyframe at the start of the video");
            return false;
        }
    }
    for (int i = from; i <= index; ++i) {
        if (!DecodeFrame(i)) {
            return false;
        }
    }
    return true;
}

/* ---------------- menu items without their baked-in background ---------------- */

enum { ITEM_STATES = 4, ITEM_SLOTS = 6, ITEM_X = 0x98 };

const int kItemObject[ITEM_STATES] = {0xea, 0xec, 0xeb, 0xed}; /* normal, highlight, depressed, unavailable */

struct ItemImage {
    int x, y, w, h;          /* game coordinates of the top-left pixel and size */
    unsigned short* pixels;  /* surface format */
    unsigned char* keep;     /* 1 - drawn over the video */
};

ItemImage s_items[ITEM_STATES][ITEM_SLOTS];
bool s_items_ready;          /* made once per session */
int s_item_state[ITEM_SLOTS];
int s_item_top[ITEM_SLOTS];

void Rgb(unsigned short v, int* r, int* g, int* b)
{
    *r = ((v >> 10) & 31) * 255 / 31;
    *g = ((v >> 5) & 31) * 255 / 31;
    *b = (v & 31) * 255 / 31;
}

int Difference(unsigned short a, unsigned short b)
{
    int r1, g1, b1, r2, g2, b2;
    Rgb(a, &r1, &g1, &b1);
    Rgb(b, &r2, &g2, &b2);
    int d = (r1 > r2 ? r1 - r2 : r2 - r1) + (g1 > g2 ? g1 - g2 : g2 - g1) +
            (b1 > b2 ? b1 - b2 : b2 - b1);
    return d;
}

/* Decode one ETRLE sub-image of an 8-bit video object into palette indices (0 = transparent). */
bool DecodeEtrle(HVOBJECT object, int image, unsigned char* out, int w, int h)
{
    const ETRLEObject* e = &object->pETRLEObject[image];
    const unsigned char* p = static_cast<const unsigned char*>(object->pPixData) + e->uiDataOffset;
    const unsigned char* end = p + e->uiDataLength;
    memset(out, 0, static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        int x = 0;
        while (p < end) {
            unsigned char b = *p++;
            if (b == 0) {
                break;
            }
            if (b & 0x80) {
                x += b & 0x7f;
            } else {
                for (int i = 0; i < b && p < end; ++i, ++x) {
                    unsigned char index = *p++;
                    if (x < w) {
                        out[y * w + x] = index;
                    }
                }
            }
        }
    }
    return true;
}

void DropSmallSpecks(unsigned char* mask, int w, int h, int min_size, int* stack)
{
    unsigned char* seen = new unsigned char[static_cast<size_t>(w) * h];
    memset(seen, 0, static_cast<size_t>(w) * h);
    for (int start = 0; start < w * h; ++start) {
        if (!mask[start] || seen[start]) {
            continue;
        }
        int top = 0;
        int count = 0;
        stack[top++] = start;
        seen[start] = 1;
        int first = 0;
        /* the stack array doubles as the component list */
        while (first < top) {
            int at = stack[first++];
            ++count;
            int cx = at % w;
            int cy = at / w;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = cx + dx;
                    int ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    int n = ny * w + nx;
                    if (mask[n] && !seen[n]) {
                        seen[n] = 1;
                        stack[top++] = n;
                    }
                }
            }
        }
        if (count < min_size) {
            for (int i = 0; i < top; ++i) {
                mask[stack[i]] = 0;
            }
        }
    }
    delete[] seen;
}

bool MakeItemImages(void)
{
    if (s_items_ready) {
        return true;
    }
    /* The original 640x480 menu background (the strips carry a piece of it). */
    SGPFILENAME name;
    strcpy(name, "Data\\Options\\intro_bg_text.sti");
    HIMAGE background = CreateImage(name, IMAGE_ALLIMAGEDATA);
    if (background == 0 || background->ubBitDepth != 16 || background->usWidth != W8_GAME_W ||
        background->usHeight != W8_GAME_H || background->p16BPPData == 0) {
        if (background != 0) {
            DestroyImage(background);
        }
        Log("cannot read the original menu background");
        return false;
    }
    const unsigned short* bg = background->p16BPPData;
    for (int i = 0; i < W8_GAME_W * W8_GAME_H; ++i) {
        if (bg[i] != 0) { /* the game's loader adds its alpha bit to every non-black pixel */
            s_alpha = static_cast<unsigned short>(bg[i] & 0x8000);
            break;
        }
    }
    const int threshold = 28;
    bool ok = true;
    for (int state = 0; state < ITEM_STATES && ok; ++state) {
        int y_offset = 0;
        HVOBJECT object = GetCatalogVideoObject(kItemObject[state], 0, &y_offset);
        if (object == 0 || object->ubBitDepth != 8 || object->p16BPPPalette == 0) {
            ok = false;
            break;
        }
        for (int slot = 0; slot < ITEM_SLOTS; ++slot) {
            int image = y_offset + slot;
            if (image >= object->usNumberOfObjects) {
                ok = false;
                break;
            }
            const ETRLEObject* e = &object->pETRLEObject[image];
            ItemImage* item = &s_items[state][slot];
            item->w = e->usWidth;
            item->h = e->usHeight;
            item->x = ITEM_X + e->sOffsetX;
            item->y = e->sOffsetY; /* the row top is added when drawing */
            int count = item->w * item->h;
            unsigned char* indices = new unsigned char[count];
            int* diff = new int[count];
            int* stack = new int[count];
            item->pixels = new unsigned short[count];
            item->keep = new unsigned char[count];
            DecodeEtrle(object, image, indices, item->w, item->h);
            /* The row top of this slot, as DrawMainMenuItem draws it. */
            static const int kTops[ITEM_SLOTS] = {0x8a, 0xbb, 0xeb, 0x11c, 0x14f, 0x1a7};
            for (int y = 0; y < item->h; ++y) {
                for (int x = 0; x < item->w; ++x) {
                    int i = y * item->w + x;
                    unsigned short colour = indices[i] ? object->p16BPPPalette[indices[i]] : 0;
                    item->pixels[i] = colour;
                    int gx = item->x + x;
                    int gy = kTops[slot] + item->y + y;
                    unsigned short under = gx >= 0 && gx < W8_GAME_W && gy >= 0 && gy < W8_GAME_H
                                               ? bg[gy * W8_GAME_W + gx]
                                               : 0;
                    diff[i] = indices[i] ? Difference(colour, under) : 0;
                }
            }
            /* core: 3x3 median of the difference above the threshold */
            unsigned char* core = item->keep;
            for (int y = 0; y < item->h; ++y) {
                for (int x = 0; x < item->w; ++x) {
                    int values[9];
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            int nx = x + dx;
                            int ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < item->w && ny < item->h) {
                                values[n++] = diff[ny * item->w + nx];
                            }
                        }
                    }
                    for (int a = 1; a < n; ++a) { /* insertion sort of up to 9 values */
                        int v = values[a];
                        int b = a - 1;
                        while (b >= 0 && values[b] > v) {
                            values[b + 1] = values[b];
                            --b;
                        }
                        values[b + 1] = v;
                    }
                    core[y * item->w + x] = values[n / 2] > threshold ? 1 : 0;
                }
            }
            DropSmallSpecks(core, item->w, item->h, 8, stack);
            /* fringe: 1 pixel around the core where the difference is above half the threshold */
            unsigned char* keep = new unsigned char[count];
            memcpy(keep, core, count);
            for (int y = 0; y < item->h; ++y) {
                for (int x = 0; x < item->w; ++x) {
                    if (!core[y * item->w + x]) continue;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            int nx = x + dx;
                            int ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < item->w && ny < item->h &&
                                diff[ny * item->w + nx] > threshold / 2) {
                                keep[ny * item->w + nx] = 1;
                            }
                        }
                    }
                }
            }
            memcpy(item->keep, keep, count);
            delete[] keep;
            /* the letter shapes are the same in every state: dark (unavailable) letters on a
               dark background differ little, so keep the normal state's mask too */
            if (state > 0 && s_items[0][slot].w == item->w && s_items[0][slot].h == item->h) {
                for (int i = 0; i < count; ++i) {
                    if (s_items[0][slot].keep[i]) item->keep[i] = 1;
                }
            }
            for (int i = 0; i < count; ++i) {
                if (indices[i] == 0) item->keep[i] = 0;
            }
            delete[] indices;
            delete[] diff;
            delete[] stack;
        }
    }
    DestroyImage(background);
    if (!ok) {
        Log("menu item pictures are not the expected 8-bit strips");
        for (int state = 0; state < ITEM_STATES; ++state) {
            for (int slot = 0; slot < ITEM_SLOTS; ++slot) {
                delete[] s_items[state][slot].pixels;
                delete[] s_items[state][slot].keep;
                s_items[state][slot].pixels = 0;
                s_items[state][slot].keep = 0;
            }
        }
        return false;
    }
    s_items_ready = true;
    return true;
}

/* ---------------- drawing ---------------- */

/* Copy the decoded picture to the canvas: the rectangle [x0, x1) x [y0, y1) in canvas
   coordinates (the picture is centred on the canvas). */
void BlitPicture(unsigned char* surface, unsigned int pitch, int x0, int y0, int x1, int y1)
{
    const unsigned short* picture = s_buffer[s_current];
    int shift = (VIDEO_W - g_canvas_w) / 2;
    for (int y = y0; y < y1; ++y) {
        const unsigned short* src = picture + (y + BORDER) * PITCH + BORDER + shift;
        unsigned short* dst = reinterpret_cast<unsigned short*>(surface + static_cast<size_t>(y) * pitch);
        for (int x = x0; x < x1; ++x) {
            dst[x] = s_lut[src[x]];
        }
    }
}

void DrawItemOn(unsigned char* surface, unsigned int pitch, int slot)
{
    int state = s_item_state[slot];
    int top = s_item_top[slot];
    if (state < 0 || state >= ITEM_STATES || top < 0) {
        return;
    }
    const ItemImage* item = &s_items[state][slot];
    if (item->pixels == 0) {
        return;
    }
    int origin = g_canvas_ox;
    for (int y = 0; y < item->h; ++y) {
        int cy = top + item->y + y;
        if (cy < 0 || cy >= W8_GAME_H) continue;
        unsigned short* dst = reinterpret_cast<unsigned short*>(surface + static_cast<size_t>(cy) * pitch);
        for (int x = 0; x < item->w; ++x) {
            int i = y * item->w + x;
            int cx = origin + item->x + x;
            if (item->keep[i] && cx >= 0 && cx < g_canvas_w) {
                dst[cx] = item->pixels[i];
            }
        }
    }
}


/* The whole canvas: picture, items, version line, then mark everything for upload. */
void DrawWholeCanvas(void)
{
    unsigned int pitch = 0;
    unsigned char* surface = static_cast<unsigned char*>(LockPrimarySurfaceFull(&pitch));
    if (surface == 0) {
        return;
    }
    BlitPicture(surface, pitch, 0, 0, g_canvas_w, VIDEO_H);
    for (int slot = 0; slot < ITEM_SLOTS; ++slot) {
        DrawItemOn(surface, pitch, slot);
    }
    UnlockPrimarySurface();
    DrawMainMenuVersionLine();
    CanvasNoteSideBarsCleared();
    /* as the menu's own redraw: reset the dirty marks, mark the whole canvas */
    ResetTransientRenderScenes();
    InvalidateCanvasSideBars(0);
}

void FreeVideo(void)
{
    if (s_zlib_ready) {
        inflateEnd(&s_zlib);
        s_zlib_ready = false;
    }
    delete[] s_file;
    s_file = 0;
    delete[] s_buffer[0];
    delete[] s_buffer[1];
    s_buffer[0] = s_buffer[1] = 0;
    delete[] s_work;
    s_work = 0;
    delete[] s_blocks;
    s_blocks = 0;
    s_frames = 0;
    s_decoded = -1;
    s_block_count = 0;
}

bool LoadVideo(void)
{
    char path[MAX_PATH];
    ExperimentalIniPath(path, sizeof(path)); /* then the file name is replaced */
    char* slash = strrchr(path, '\\');
    if (slash == 0) {
        return false;
    }
    static const char kVideo[] = "Data\\WIDE\\MENU_ANIM.AVI";
    if (static_cast<size_t>(slash + 1 - path) + sizeof(kVideo) > sizeof(path)) {
        Log("game folder path too long: static background");
        return false;
    }
    memcpy(slash + 1, kVideo, sizeof(kVideo));
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        Log("no Data\\WIDE\\MENU_ANIM.AVI: static background");
        return false;
    }
    DWORD size = GetFileSize(file, NULL);
    bool ok = size != INVALID_FILE_SIZE && size > 12 && size < 256u * 1024 * 1024;
    if (ok) {
        s_file = new unsigned char[size];
        DWORD read = 0;
        ok = ReadFile(file, s_file, size, &read, NULL) && read == size;
    }
    CloseHandle(file);
    if (!ok || !ParseAvi(size)) {
        Log("cannot read Data\\WIDE\\MENU_ANIM.AVI");
        FreeVideo();
        return false;
    }
    s_buffer[0] = new unsigned short[PITCH * ROWS];
    s_buffer[1] = new unsigned short[PITCH * ROWS];
    memset(s_buffer[0], 0, PITCH * ROWS * sizeof(unsigned short));
    memset(s_buffer[1], 0, PITCH * ROWS * sizeof(unsigned short));
    s_work_size = VIDEO_W * VIDEO_H * 2 + MAX_BLOCKS * 2 + 1024;
    s_work = new unsigned char[s_work_size];
    s_blocks = new Block[MAX_BLOCKS];
    s_current = 0;
    s_decoded = -1;

    char text[160];
    wsprintfA(text, "MENU_ANIM.AVI: %d frames, %u us per frame", s_frames, s_period_us);
    Log(text);
    return true;
}

} // namespace

bool ExperimentalAnimatedMenuEnabled(void)
{
    if (s_setting < 0) {
        s_setting = ExperimentalReadSwitch("AnimatedMenu") ? 1 : 0;
    }
    return s_setting == 1;
}

void ExperimentalSetAnimatedMenu(bool enabled)
{
    s_setting = enabled ? 1 : 0;
    ExperimentalWriteSwitch("AnimatedMenu", enabled);
}

bool MenuAnimationStart(void)
{
    s_active = false;
    for (int slot = 0; slot < ITEM_SLOTS; ++slot) {
        s_item_state[slot] = -1;
        s_item_top[slot] = -1;
    }
    if (!ExperimentalAnimatedMenuEnabled() || s_broken) {
        return false;
    }
    if (!LoadVideo()) {
        return false;
    }
    if (!MakeItemImages()) {
        FreeVideo();
        return false;
    }
    if (s_lut == 0) {
        /* The game's 5:6:5 -> 5:5:5 conversion (ConvertRGBDistribution565To555): black stays
           0, the rest drops the low green bit and gets the surface's alpha bit. */
        s_lut = new unsigned short[65536];
        for (unsigned int v = 0; v < 65536; ++v) {
            s_lut[v] = v == 0 ? 0 : static_cast<unsigned short>(((v >> 6) << 5) | (v & 31) | s_alpha);
        }
    }
    int first = s_resume_frame >= 0 && s_resume_frame < s_frames ? s_resume_frame : 0;
    while (first > 0 && !IsKeyframe(first)) {
        --first;
    }
    if (!SeekTo(first)) {
        FreeVideo();
        return false;
    }
    s_active = true;
    s_start_tick = GetTickCount();
    s_start_frame = first;
    s_shown = first;
    return true;
}

void MenuAnimationStop(void)
{
    if (s_active) {
        s_resume_frame = s_decoded >= 0 ? s_decoded : 0;
    }
    s_active = false;
    s_frozen = false;
    FreeVideo();
}

void MenuAnimationTick(void)
{
    if (!s_active || s_frozen) {
        return;
    }
    DWORD now = GetTickCount();
    /* A dialog or message box is up: hold the picture (the game draws them over the canvas). */
    if (g_main_menu_dialog != 0 || IsMessageBoxActive()) {
        s_start_tick = now;
        s_start_frame = s_shown;
        return;
    }
    long due = s_start_frame + static_cast<long>((static_cast<unsigned long long>(now - s_start_tick) * 1000) /
                                                 s_period_us);
    if (due <= s_shown) {
        return;
    }
    if (due - s_shown > 15) {
        /* Far behind (Alt+Tab, a long load): continue from where we are, no fast-forward. */
        s_start_tick = now;
        s_start_frame = s_shown + 1;
        due = s_shown + 1;
    }
    int index = static_cast<int>(due % s_frames);
    bool ok = index < s_decoded ? SeekTo(index) : true;
    for (int i = s_decoded + 1; ok && i <= index; ++i) {
        ok = DecodeFrame(i);
    }
    if (!ok) {
        /* A damaged file: keep the last good picture (the items keep being drawn over it),
           and use the static background from the next menu entry on. */
        Log("decoding failed: the picture stops; static background until the game restarts");
        s_frozen = true;
        s_broken = true;
        s_resume_frame = 0;
        return;
    }
    s_shown = due;
    DrawWholeCanvas();
}

bool MenuAnimationDrawItem(int slot, int state, int top)
{
    if (!s_active || slot < 0 || slot >= ITEM_SLOTS) {
        return false;
    }
    s_item_state[slot] = state;
    s_item_top[slot] = top;
    unsigned int pitch = 0;
    unsigned char* surface = static_cast<unsigned char*>(LockPrimarySurfaceFull(&pitch));
    if (surface == 0) {
        return true;
    }
    /* the background under the item from the current picture, then the item */
    const ItemImage* item = &s_items[state >= 0 && state < ITEM_STATES ? state : 0][slot];
    int x0 = g_canvas_ox + item->x;
    int y0 = top + item->y;
    int x1 = x0 + item->w;
    int y1 = y0 + item->h;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > g_canvas_w ? g_canvas_w : x1;
    y1 = y1 > W8_GAME_H ? W8_GAME_H : y1;
    if (x0 < x1 && y0 < y1) {
        BlitPicture(surface, pitch, x0, y0, x1, y1);
    }
    DrawItemOn(surface, pitch, slot);
    UnlockPrimarySurface();
    return true;
}

bool MenuAnimationDrawBackground(const char* path)
{
    if (!s_active || path == 0 || !EndsWithNoCase(path, "\\intro_bg_text.sti")) {
        return false;
    }
    unsigned int pitch = 0;
    unsigned char* surface = static_cast<unsigned char*>(LockPrimarySurfaceFull(&pitch));
    if (surface == 0) {
        return false;
    }
    BlitPicture(surface, pitch, 0, 0, g_canvas_w, VIDEO_H);
    UnlockPrimarySurface();
    CanvasNoteSideBarsCleared();
    InvalidateCanvasSideBars(0);
    return true;
}
