/* Main-screen HUD layout "A" on the wide canvas. Not part of the original
   game (2026-10-11). What moves where - see include/wiz8/local_screens/
   MainGameLayout.h and wiz8-widescreen docs/wide-canvas.md, section "HUD layout "A"".

   Reminder of the mechanism (wiz8/canvas.h): element coordinates in the game
   code stay 640-based. The anchor (LEFT/RIGHT/CENTER) says which canvas edge
   the element is pinned to; drawing is shifted via CanvasDrawScope, the mouse
   via the region anchor. This file holds the anchors of static regions and
   helpers for stretched elements. */
#include "wiz8/local_screens/MainGameLayout.h"

#include "wiz8/cursor.h"
#include "wiz8/engine_code/Video2.h"
#include "wiz8/local_screens/MainGameScreen.h"
#include "wiz8/local_screens/MGSTextBox.h"
#include "wiz8/local_screens/NPCInteractionSubscreen.h"
#include "wiz8/regions.h"
#include "wiz8/utility.h"
#include "wiz8/video_object_catalog.h"
#include "wiz8/wide_art.h"
#include "wiz8/xstatus.h"
#include "vobject_blitters.h"
#include "wiz8/canvas.h"

#include <stdio.h>

/* Static regions (RegionManager.cpp, g_regions) that belong to the portrait
   columns or to the edges of the bottom panel. Numbers are table indices. */
enum {
    REGION_COMBAT_ACTION_FIRST = 10, /* 10..17: combat action button, slot = i - 10 */
    REGION_PORTRAIT_CONTROL_FIRST = 18, /* 18..25: level-up button, slot = i - 18 */
    REGION_CONDITION_BUTTON_FIRST = 26, /* 26..33: condition icon, slot = i - 26 */
    REGION_PORTRAIT_ROWS_FIRST = 34, /* 34..81: 6 regions per slot (orbs, portrait, ...) */
    REGION_PORTRAIT_ROWS_LAST = 81,
    REGION_TEXT_TABS_FIRST = 86, /* 86..88: log channel tabs (left edge of the log) */
    REGION_TEXT_TABS_LAST = 88,
    REGION_TEXT_MUTE = 89,           /* 128..511 x 358..450: log background (stretched) */
    REGION_EDGE_PORTRAIT_FIRST = 90, /* 90..97: narrow portrait strips at the edges */
    REGION_RADAR_CIRCLE = 98,        /* radar circle (left edge) */
    REGION_FORMATION_CIRCLE = 99,    /* formation board circle (right edge) */
    REGION_FORMATION_MUTE = 100,     /* 512..617: formation board background */
    REGION_SPELL_ICON_FIRST = 202,   /* 202..213: party spell icons on the top bar, right side */
    REGION_SPELL_ICON_LAST = 213,
    REGION_COMBAT_LEFT_EFFECT_FIRST = 214, /* 214..222: combat effects at the left column */
    REGION_COMBAT_LEFT_EFFECT_LAST = 222,
    REGION_COMBAT_RIGHT_EFFECT_FIRST = 223, /* 223..228: combat effects at the right column */
    REGION_COMBAT_RIGHT_EFFECT_LAST = 228,
    REGION_PORTRAIT_QUOTE_FIRST = 302, /* 302..309: speech "bubble" next to a portrait */
    REGION_PORTRAIT_QUOTE_LAST = 309
};

/* Original x bounds of the stretchable static log-background region. */
static short s_text_mute_x1;
static short s_text_mute_x2;
static bool s_text_mute_widened;

static void SetSlotAnchors(unsigned int first, unsigned int count, unsigned int regions_per_slot)
{
    for (unsigned int index = 0; index < count; ++index) {
        SetRegionLayoutAnchor(first + index, CanvasPartySlotAnchor(index / regions_per_slot));
    }
}

static void SetRangeAnchor(unsigned int first, unsigned int last, int anchor)
{
    for (unsigned int index = first; index <= last; ++index) {
        SetRegionLayoutAnchor(index, anchor);
    }
}

/* Anchors of the main screen's static regions. While the layout is off,
   an anchor has no effect, so they can always be set. */
static void ApplyMainGameRegionAnchors(void)
{
    SetSlotAnchors(REGION_COMBAT_ACTION_FIRST, 8, 1);
    SetSlotAnchors(REGION_PORTRAIT_CONTROL_FIRST, 8, 1);
    SetSlotAnchors(REGION_CONDITION_BUTTON_FIRST, 8, 1);
    SetSlotAnchors(REGION_PORTRAIT_ROWS_FIRST,
                   REGION_PORTRAIT_ROWS_LAST - REGION_PORTRAIT_ROWS_FIRST + 1, 6);
    SetRangeAnchor(REGION_TEXT_TABS_FIRST, REGION_TEXT_TABS_LAST, W8_ANCHOR_LEFT);
    SetSlotAnchors(REGION_EDGE_PORTRAIT_FIRST, 8, 1);
    SetRegionLayoutAnchor(REGION_RADAR_CIRCLE, W8_ANCHOR_LEFT);
    SetRegionLayoutAnchor(REGION_FORMATION_CIRCLE, W8_ANCHOR_RIGHT);
    SetRegionLayoutAnchor(REGION_FORMATION_MUTE, W8_ANCHOR_RIGHT);
    SetRangeAnchor(REGION_SPELL_ICON_FIRST, REGION_SPELL_ICON_LAST, W8_ANCHOR_RIGHT);
    SetRangeAnchor(REGION_COMBAT_LEFT_EFFECT_FIRST, REGION_COMBAT_LEFT_EFFECT_LAST,
                   W8_ANCHOR_LEFT);
    SetRangeAnchor(REGION_COMBAT_RIGHT_EFFECT_FIRST, REGION_COMBAT_RIGHT_EFFECT_LAST,
                   W8_ANCHOR_RIGHT);
    SetSlotAnchors(REGION_PORTRAIT_QUOTE_FIRST, 8, 1);
}

void MainGameLayoutEnter(void)
{
    CanvasSetWideLayout(true);
    ApplyMainGameRegionAnchors();
    if (CanvasWideLayoutActive() && !s_text_mute_widened) {
        /* The log background is stretched, and so is its "muting" region (center, edges +-ox). */
        W8Region& mute = g_regions[REGION_TEXT_MUTE];
        s_text_mute_x1 = mute.x1;
        s_text_mute_x2 = mute.x2;
        mute.x1 = static_cast<short>(MainGameWideLeft(mute.x1));
        mute.x2 = static_cast<short>(MainGameWideRight(mute.x2));
        s_text_mute_widened = true;
    }
}

void MainGameLayoutLeave(void)
{
    if (s_text_mute_widened) {
        g_regions[REGION_TEXT_MUTE].x1 = s_text_mute_x1;
        g_regions[REGION_TEXT_MUTE].x2 = s_text_mute_x2;
        s_text_mute_widened = false;
    }
    CanvasSetWideLayout(false);
}

void MainGameViewportRect(int mode, W8ScreenRect* rect)
{
    *rect = g_viewport_modes[mode];
    rect->left = MainGameWideLeft(rect->left);
    rect->right = MainGameWideRight(rect->right);
}

/* One piece of a stretched image: the image is drawn at its usual
   coordinates (left, top), but the window origin on the canvas = origin, and
   only the strip [clip_left, clip_right) is visible (same coordinates). */
static void DrawWidePiece(int object, int frame, int image, int left, int top, int origin,
                          int clip_left, int clip_right, const SGPRect& saved_clip)
{
    SGPRect clip = saved_clip;
    if (clip.iLeft < clip_left) {
        clip.iLeft = clip_left;
    }
    if (clip.iRight > clip_right) {
        clip.iRight = clip_right;
    }
    if (clip.iLeft >= clip.iRight) {
        return;
    }
    CanvasOriginScope scope(origin);
    SetClippingRect(&clip);
    DrawCatalogImage(FRAME_BUFFER, object, frame, static_cast<short>(image), left, top,
                     VO_BLT_SRCTRANSPARENCY, 0);
}

void DrawCatalogImagePiece(int object, int frame, int image, int left, int top, int origin,
                           int clip_left, int clip_right)
{
    SGPRect saved_clip;
    GetClippingRect(&saved_clip);
    DrawWidePiece(object, frame, image, left, top, origin, clip_left, clip_right, saved_clip);
    SetClippingRect(&saved_clip);
}

/* Once per image: log which image is stretched by repeating slices with no
   prepared seamless version (wiz8-widescreen scripts/art/stretch_quilt.py makes the missing ones from these lines). */
static void LogStretchWithoutStrip(int object, int frame, int image, int width, int cut_left,
                                   int cut_right)
{
    static int s_logged[64];
    static int s_count;
    const int key = (object << 12) | (frame << 8) | image;
    for (int index = 0; index < s_count; ++index) {
        if (s_logged[index] == key) {
            return;
        }
    }
    if (s_count < 64) {
        s_logged[s_count++] = key;
    }
    short x = 0;
    short y = 0;
    GetCatalogImagePosition(object, frame, image, &x, &y);
    char text[200];
    _snprintf(text, sizeof(text), "[Stretch] no strip: S%03X_%d_%d %s image %d width %d cut %d..%d offset %d,%d",
              object, frame, image, g_video_frames[g_video_slots[object].first_frame + frame].path,
              image, width, cut_left, cut_right, x, y);
    text[sizeof(text) - 1] = '\0';
    CanvasLogLine(text);
}

void DrawCatalogImageWide(int object, int frame, int image, int left, int top, int width,
                          int cut_left, int cut_right, bool invalidate)
{
    if (!CanvasWideLayoutActive()) {
        if (invalidate) {
            DrawCatalogImageAndInvalidate(FRAME_BUFFER, object, frame, image, left, top,
                                          VO_BLT_SRCTRANSPARENCY, 0);
        } else {
            DrawCatalogImage(FRAME_BUFFER, object, frame, static_cast<short>(image), left, top,
                             VO_BLT_SRCTRANSPARENCY, 0);
        }
        return;
    }

    const int ox = g_canvas_ox;
    const int slice = cut_right - cut_left; /* width of the repeated slice */
    SGPRect saved_clip;
    GetClippingRect(&saved_clip);

    /* 0) Prepared seamless version (wiz8-widescreen scripts/art/stretch_quilt.py), if present. */
    if (DrawWideStretchStrip(object, frame, image, left, top, width, cut_left, cut_right,
                             saved_clip.iLeft, saved_clip.iTop, saved_clip.iRight,
                             saved_clip.iBottom)) {
        if (invalidate) {
            short image_width = 0;
            short image_height = 0;
            GetCatalogImageSize(object, frame, image, &image_width, &image_height);
            CanvasOriginScope scope(0);
            InvalidateRegion(left, top, left + width + 2 * ox, top + image_height, 1);
        }
        return;
    }
    LogStretchWithoutStrip(object, frame, image, width, cut_left, cut_right);

    /* 1) Left part [0, cut_right) at the left canvas edge (window origin 0). */
    DrawWidePiece(object, frame, image, left, top, 0, left, left + cut_right, saved_clip);
    /* 2) Repeats of the slice [cut_left, cut_right): the k-th repeat is shifted by k*slice
          to the right; fills [left+cut_right, left+cut_right+2*ox) on the canvas. */
    const int fill_end = left + cut_right + 2 * ox; /* canvas: where the right part starts */
    for (int k = 1; slice > 0 && (k - 1) * slice < 2 * ox; ++k) {
        int origin = k * slice;
        int clip_right = left + cut_right;
        if (clip_right > fill_end - origin) {
            clip_right = fill_end - origin; /* do not run under the right part */
        }
        DrawWidePiece(object, frame, image, left, top, origin, left + cut_left, clip_right,
                      saved_clip);
    }
    /* 3) Right part [cut_right, width) at the right canvas edge (origin 2*ox). */
    DrawWidePiece(object, frame, image, left, top, 2 * ox, left + cut_right, left + width,
                  saved_clip);
    SetClippingRect(&saved_clip);

    if (invalidate) {
        short image_width = 0;
        short image_height = 0;
        GetCatalogImageSize(object, frame, image, &image_width, &image_height);
        CanvasOriginScope scope(0);
        InvalidateRegion(left, top, left + width + 2 * ox, top + image_height, 1);
    }
}

void DrawCombatProgressWide(int percent)
{
    const int ox = g_canvas_ox;
    const int left = 0xb1;      /* 177: bar position, as in the original */
    const int top = 2;
    const int width = 0x11e;    /* 286: bar image width */
    const int cut_left = 123;   /* repeated middle slice of the bar image */
    const int cut_right = 163;
    const int slice = cut_right - cut_left;
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    /* Canvas x where the fill ends: the whole widened recess is 286 + 2*ox long. */
    const int fill = left + (percent * (width + 2 * ox)) / 100;
    const int fill_end = left + cut_right + 2 * ox; /* canvas: where the right part starts */
    SGPRect saved_clip;
    GetClippingRect(&saved_clip);
    SGPRect base = {0, 0, 0x280, 0x1e0};
    /* A piece drawn with window origin o shows canvas [a, b) as image x [a - o, b - o). */
    int right = left + cut_right;
    if (right > fill) {
        right = fill;
    }
    DrawWidePiece(0x8d, 0, 0, left, top, 0, left, right, base);
    for (int k = 1; (k - 1) * slice < 2 * ox; ++k) {
        const int origin = k * slice;
        right = left + cut_right;
        if (right > fill_end - origin) {
            right = fill_end - origin;
        }
        if (right > fill - origin) {
            right = fill - origin;
        }
        DrawWidePiece(0x8d, 0, 0, left, top, origin, left + cut_left, right, base);
    }
    right = left + width;
    if (right > fill - 2 * ox) {
        right = fill - 2 * ox;
    }
    DrawWidePiece(0x8d, 0, 0, left, top, 2 * ox, left + cut_right, right, base);
    SetClippingRect(&saved_clip);

    short image_width = 0;
    short image_height = 0;
    GetCatalogImageSize(0x8d, 0, 0, &image_width, &image_height);
    CanvasOriginScope scope(0);
    InvalidateRegion(left, top, left + width + 2 * ox, top + image_height, 1);
}

void ClearSurfaceRectWide(int left, int top, int right, int bottom)
{
    if (!CanvasWideLayoutActive()) {
        /* Retail: only the clear (the redraw pass invalidates the panel). */
        ClearSurfaceRect(left, top, right, bottom);
        return;
    }
    /* Window origin 0: x is a canvas x; the right edge moves out by 2*ox. */
    CanvasOriginScope scope(0);
    ClearSurfaceRect(left, top, right + 2 * g_canvas_ox, bottom);
    InvalidateRegion(left, top, right + 2 * g_canvas_ox, bottom, 0);
}

void ClearCanvasForFullscreenArt(void)
{
    if (!CanvasWideLayoutActive()) {
        return;
    }
    /* Window origin 0: x is a canvas x; clear the full width W. */
    CanvasOriginScope scope(0);
    ClearSurfaceRect(0, 0, g_canvas_w, 0x1e0);
    InvalidateRegion(0, 0, g_canvas_w, 0x1e0, 0);
}

bool IsCursorInAnchoredRectangle(int anchor, int left, int top, int right, int bottom)
{
    int shift = CanvasAnchorShift(anchor);
    return IsCursorInRectangle(left + shift, top, right + shift, bottom);
}

void SetMainTextBoxRegionBounds(void)
{
    /* Original: SetTextBoxRegionBounds(0xa8, 0x16e, 0x1c4, 0x1ba). The line
       wrap width (right - left - 10) grows by itself. */
    SetTextBoxRegionBounds(MainGameWideLeft(0xa8), 0x16e, MainGameWideRight(0x1c4), 0x1ba);
}

/* ---- Bottom-panel subscreens (2026-10-12), see MainGameLayout.h ---- */

/* Fallback for images that are not in the table below (another game
   version, a mod): */
enum {
    W8_STRETCH_SLICE = 16,  /* width of the repeated middle slice of a stretched background */
    W8_GAP_SLICE_INSET = 4, /* gap: the slice is taken 4 px inside the background edge ... */
    W8_GAP_SLICE = 8        /* ... 8 px wide (the frame at the very edge is not repeated) */
};

/* Slices for known subscreen backgrounds (2026-10-12, after review). Chosen
   from the images themselves (DATA.SLF, 8-bit STI, unpacked to PNG and checked
   by stitching at 768 and 856): "the middle 16 px" and "8 px at 4 px inside the edge"
   landed on frames and details - for the trap, on the light frame of the list field; for the
   lock, on the mechanism rail and cylinder; for the NPC, on a dark crack - and when
   repeated produced stripes (an "accordion"). All x are inside the image, from its left
   edge. An entry applies only if the image width matches. */
struct W8SubscreenArtCuts {
    short object; /* catalog object (frame 0) */
    short image;  /* image (sub-image) */
    short width;  /* expected image width */
    /* STRETCH: repeated slice [stretch_from, stretch_to); 0, 0 - none. */
    short stretch_from;
    short stretch_to;
    /* FILL_GAPS (panel centered): the piece [0, left_edge) moves to the left edge
       of the left gap, the gap itself is repeats of [left_from, left_to); the piece
       [right_edge, width) moves to the right edge of the right gap, the gap is repeats of
       [right_from, right_to). The image frame stays at the edges, its interior is
       extended. right_edge = 0 - no entry for this mode. */
    short left_edge;
    short left_from;
    short left_to;
    short right_edge;
    short right_from;
    short right_to;
};

static const W8SubscreenArtCuts s_subscreen_art_cuts[] = {
    /* NPC, middle (Npc_bottompanel.sti, image 1, 312 px): the dark
       texture repeats exactly every 97 px, a one-period slice tiles without
       seams. */
    {0x1a9, 1, 312, 100, 197, 0, 0, 0, 0, 0, 0},
    /* Item selection, middle (item_bottompanel.sti, image 1): the same
       texture, the same period. */
    {0x97, 1, 312, 100, 197, 0, 0, 0, 0, 0, 0},
    /* Spellbook, middle (MagicBottomNew.sti, image 1, 190 px):
       no period - a wide 80 px slice (fewer repeats) with the least noticeable
       seam. The dark strip at the right edge (171..187) stays on the right. */
    {0x190, 1, 190, 61, 141, 0, 0, 0, 0, 0, 0},
    /* Trap, device panel (traps_bottompanel.sti, image 1, 276 px):
       on the left a frame with icons (0..137), on the right the black list field (140..271,
       frame 272..273). The list field is stretched: a 32 px slice inside it. */
    {0x1b1, 1, 276, 152, 184, 0, 0, 0, 0, 0, 0},
    /* Lock, mechanism (lockpick_bottompanel.sti, image 1, 276 px): the pins
       sit at their own places on the background, so it stays centered; the black housing
       is extended into both gaps. Left: edge [0,4) (stone), repeat [4,9) -
       black with the bottom rail; right: repeat [267,272) - black between the mechanism
       rail and the frame, edge [267,276) - frame and stone. */
    {0x1af, 1, 276, 0, 0, 4, 4, 9, 267, 267, 272},
    /* Wide windows (2026-10-12): right options panel (options_base.sti,
       image 1, 380 px) - the dark pattern repeats every 97 px, the slice
       [102, 199) tiles without a seam (found with wiz8-widescreen scripts/art/find_slice.py). */
    {0xee, 1, 380, 102, 199, 0, 0, 0, 0, 0, 0},
    /* Right save/load panel (save_load_back.sti, 380 px): the same
       period; the slice is inside the frame of the bottom niches, their lines continue. */
    {0xf7, 0, 380, 102, 199, 0, 0, 0, 0, 0, 0},
    /* Options page strip (options_pagingbase.sti, 326 px): the slice is inside
       the frame of the page-number field - the field gets longer. */
    {0xf3, 0, 326, 130, 159, 0, 0, 0, 0, 0, 0},
};

static const W8SubscreenArtCuts* FindSubscreenArtCuts(int object, int frame, int image, int width)
{
    if (frame != 0) {
        return 0;
    }
    const int count = static_cast<int>(sizeof(s_subscreen_art_cuts) / sizeof(s_subscreen_art_cuts[0]));
    for (int index = 0; index < count; ++index) {
        const W8SubscreenArtCuts* cuts = &s_subscreen_art_cuts[index];
        if (cuts->object == object && cuts->image == image && cuts->width == width) {
            return cuts;
        }
    }
    return 0;
}

void PlaceSubscreenPanel(Controls* panel, int placement)
{
    if (panel == 0) {
        return;
    }
    switch (placement) {
    case W8_SUBSCREEN_LEFT:
        panel->m_wideMode = W8_PANEL_WIDE_NONE;
        panel->SetLayoutAnchor(W8_ANCHOR_LEFT);
        break;
    case W8_SUBSCREEN_RIGHT:
        panel->m_wideMode = W8_PANEL_WIDE_NONE;
        panel->SetLayoutAnchor(W8_ANCHOR_RIGHT);
        break;
    case W8_SUBSCREEN_STRETCH:
        /* Everything the panel has in 640-coordinates is counted from the left canvas edge
           (like LEFT); only the background and explicitly stretched children are stretched. */
        panel->m_wideMode = W8_PANEL_WIDE_STRETCH;
        panel->SetLayoutAnchor(W8_ANCHOR_LEFT);
        break;
    default: /* W8_SUBSCREEN_CENTER_FILL */
        panel->m_wideMode = W8_PANEL_WIDE_FILL_GAPS;
        panel->SetLayoutAnchor(W8_ANCHOR_CENTER);
        break;
    }
}

/* Fill the canvas strip [canvas_from, canvas_to) with repeats of the image slice
   [slice_left, slice_right) (x inside the image). As in
   DrawCatalogImageWide, the whole image is drawn but only the slice strip is
   visible: the window origin is chosen so that image x slice_left
   lands on the needed canvas point, and clipping (SGP ClippingRect) keeps only the
   slice. Game x values stay within 0..640. */
static void FillCanvasWithImageSlice(int object, int frame, int image, int top, int slice_left,
                                     int slice_right, int canvas_from, int canvas_to)
{
    const int slice = slice_right - slice_left;
    if (slice <= 0 || canvas_from >= canvas_to) {
        return;
    }
    SGPRect saved_clip;
    GetClippingRect(&saved_clip);
    for (int x = canvas_from; x < canvas_to; x += slice) {
        /* The image is drawn with its left edge at image_left in window coordinates,
           whose origin on the canvas is origin: image x slice_left lands on
           canvas origin + image_left + slice_left = x. */
        int origin = x - slice_left;
        int image_left = 0;
        if (origin < 0) {
            image_left = origin; /* do not move the window left of the canvas */
            origin = 0;
        }
        int width = slice;
        if (width > canvas_to - x) {
            width = canvas_to - x; /* the last piece is cut off by the end of the strip */
        }
        DrawWidePiece(object, frame, image, image_left, top, origin, image_left + slice_left,
                      image_left + slice_left + width, saved_clip);
    }
    SetClippingRect(&saved_clip);
}

/* Catalog image geometry: its own offset inside the frame (SGP adds it
   when drawing) and size. false - no size (not an ETRLE object), then
   the panel is drawn as in the original. Subscreen backgrounds are 8-bit STI objects. */
static bool SubscreenArtGeometry(int object, int frame, int image, int* offset_x, int* width,
                                 int* height)
{
    short w = 0;
    short h = 0;
    short x = 0;
    short y = 0;
    GetCatalogImageSize(object, frame, image, &w, &h);
    if (w == 0 || h == 0) {
        return false;
    }
    GetCatalogImagePosition(object, frame, image, &x, &y);
    *offset_x = x;
    *width = static_cast<unsigned short>(w);
    *height = static_cast<unsigned short>(h) + y;
    return *width > 2 * W8_GAP_SLICE_INSET + W8_GAP_SLICE;
}

bool DrawSubscreenPanelArt(int wide_mode, int object, int frame, int image, int left, int top)
{
    if (wide_mode == W8_PANEL_WIDE_NONE || !CanvasWideLayoutActive()) {
        return false;
    }
    int offset_x;
    int width;
    int height;
    if (!SubscreenArtGeometry(object, frame, image, &offset_x, &width, &height)) {
        return false; /* size unknown - let it be drawn as in the original */
    }
    const int ox = g_canvas_ox;
    const W8SubscreenArtCuts* cuts = FindSubscreenArtCuts(object, frame, image, width);
    if (wide_mode == W8_PANEL_WIDE_STRETCH) {
        /* The piece before the slice at the left edge, the piece after it at the right, between them
           repeats of the slice (image-specific for known images, otherwise the middle 16 px).
           The function itself sets the window origins (0 / 2*ox). Slices are counted from the
           drawing point, so the image's own offset is added to them. */
        int slice_from = width / 2 - W8_STRETCH_SLICE / 2;
        int slice_to = slice_from + W8_STRETCH_SLICE;
        if (cuts != 0 && cuts->stretch_to > cuts->stretch_from) {
            slice_from = cuts->stretch_from;
            slice_to = cuts->stretch_to;
        }
        DrawCatalogImageWide(object, frame, image, left, top, offset_x + width,
                             offset_x + slice_from, offset_x + slice_to, false);
        return true;
    }
    /* W8_PANEL_WIDE_FILL_GAPS: the panel itself is centered (window origin ox),
       gaps ox wide to its left and right. The image is cut into 3 pieces:
       [0, left_edge) - at the left edge of the left gap (window origin 0),
       [left_edge, right_edge) - centered (window origin ox),
       [right_edge, width) - at the right edge of the right gap (window origin 2*ox);
       the gaps between the pieces are slice repeats. Fallback (image not in the
       table): edges do not move, 8 px slices 4 px inside the edges. */
    /* Gaps (2026-10-16, at the owner's request): the whole mechanism, with its own
       frame, centered, and the gaps between it and the side panels are the native
       shaded niche, like the one at the left edge of the info panel (lockpick_bottompanel,
       image 0, x 0..28: shadow from black to dark grainy stone), extended
       with the same grain to width ox (wiz8-widescreen scripts/art/make_lock_niche.py ->
       Data\Wide\LockpickGaps_<W>.sti: left gap at x [0, ox), right one at [ox, 2*ox),
       rows 358..449). No file - dimmed wing stone; none of that either -
       the old slices. Repeating image slices produced black holes and "teeth". */
    {
        const int art_left = left + offset_x;
        const int muted = 153; /* 0.6: wing stone ~23 -> ~14 */
        const char* niche = "Data\\Wide\\LockpickGaps.sti";
        bool filled =
            DrawWideArtPiece(niche, 0, 358, ox, height, art_left, top) &&
            DrawWideArtPiece(niche, ox, 358, ox, height, art_left + width + ox, top);
        if (!filled) {
            filled = DrawWideMarbleGap(art_left, top, art_left + ox, top + height, muted, false) &&
                     DrawWideMarbleGap(art_left + width + ox, top, art_left + width + 2 * ox,
                                       top + height, muted, false);
        }
        if (filled) {
            SGPRect saved_clip;
            GetClippingRect(&saved_clip);
            DrawWidePiece(object, frame, image, left, top, ox, art_left, art_left + width,
                          saved_clip);
            SetClippingRect(&saved_clip);
            return true;
        }
    }
    int left_edge = 0;
    int left_from = W8_GAP_SLICE_INSET;
    int left_to = W8_GAP_SLICE_INSET + W8_GAP_SLICE;
    int right_edge = width;
    int right_from = width - W8_GAP_SLICE_INSET - W8_GAP_SLICE;
    int right_to = width - W8_GAP_SLICE_INSET;
    if (cuts != 0 && cuts->right_edge > 0) {
        left_edge = cuts->left_edge;
        left_from = cuts->left_from;
        left_to = cuts->left_to;
        right_edge = cuts->right_edge;
        right_from = cuts->right_from;
        right_to = cuts->right_to;
    }
    /* Game x of the image's left edge; with window origin 0 it is also the canvas x. */
    const int image_left = left + offset_x;
    SGPRect saved_clip;
    GetClippingRect(&saved_clip);
    DrawWidePiece(object, frame, image, left, top, 0, image_left, image_left + left_edge,
                  saved_clip);
    DrawWidePiece(object, frame, image, left, top, ox, image_left + left_edge,
                  image_left + right_edge, saved_clip);
    DrawWidePiece(object, frame, image, left, top, 2 * ox, image_left + right_edge,
                  image_left + width, saved_clip);
    SetClippingRect(&saved_clip);
    /* Gaps in canvas coordinates: the left one right after the left edge piece,
       the right one right after the central piece (which is shifted by ox). */
    FillCanvasWithImageSlice(object, frame, image, top, offset_x + left_from, offset_x + left_to,
                             image_left + left_edge, image_left + left_edge + ox);
    FillCanvasWithImageSlice(object, frame, image, top, offset_x + right_from,
                             offset_x + right_to, image_left + right_edge + ox,
                             image_left + right_edge + 2 * ox);
    return true;
}

bool InvalidateSubscreenPanelArt(int wide_mode, int object, int frame, int image, int left,
                                 int top, unsigned int flags)
{
    if (wide_mode == W8_PANEL_WIDE_NONE || !CanvasWideLayoutActive()) {
        return false;
    }
    int offset_x;
    int width;
    int height;
    if (!SubscreenArtGeometry(object, frame, image, &offset_x, &width, &height)) {
        return false;
    }
    /* Both modes occupy the same area on the canvas: from left (joint with the left
       neighbour at the left edge) to the right edge of the image + 2*ox (joint with the right
       neighbour at the right edge). For FILL_GAPS this is gap + the panel itself + gap. */
    CanvasOriginScope scope(0); /* x below are canvas coordinates */
    InvalidateRegion(left, top, left + offset_x + width + 2 * g_canvas_ox, top + height, flags);
    return true;
}

void StretchWidgetRegion(W8Widget* widget)
{
    if (widget == 0 || widget->m_region == -1 || widget->m_pPanel == 0) {
        return;
    }
    /* Always sets bounds for the current state (outside the layout - exactly like
       the original W8Widget::UpdateRegionBounds), so the call can be repeated
       on every full redraw of the subscreen. */
    const Controls* panel = widget->m_pPanel;
    const int stretch = CanvasWideLayoutActive() ? 2 * g_canvas_ox : 0;
    SetRegionBounds(static_cast<unsigned int>(widget->m_region),
                    static_cast<unsigned short>(panel->m_bounds.left + widget->m_left),
                    static_cast<unsigned short>(panel->m_bounds.top + widget->m_top),
                    static_cast<unsigned short>(panel->m_bounds.left + widget->m_right + stretch),
                    static_cast<unsigned short>(panel->m_bounds.top + widget->m_bottom));
}

int SubscreenPanelStretch(const Controls* panel)
{
    if (panel == 0 || panel->m_wideMode != W8_PANEL_WIDE_STRETCH || !CanvasWideLayoutActive()) {
        return 0;
    }
    return 2 * g_canvas_ox;
}

void SetBottomPanelRegionBounds(unsigned int region_index)
{
    /* Original: 0x17, 0x166 .. 0x269, 0x1c2 (the whole subscreen). In layout A -
       the whole wide panel between the edge strips. */
    SetRegionBounds(region_index, static_cast<unsigned short>(MainGameWideLeft(0x17)), 0x166,
                    static_cast<unsigned short>(MainGameWideRight(0x269)), 0x1c2);
}
