/* Main game screen HUD layout "A" on the wide canvas (W > 640).
   Not part of the original game (added 2026-10-11, see wiz8-widescreen docs/wide-canvas.md,
   section "HUD layout "A"" and wiz8/canvas.h).

   The one place where it is decided what moves where:
     - portrait columns and everything attached to them: even slots to the left
       canvas edge (LEFT), odd ones to the right (RIGHT);
     - bottom panel: strip + radar on the left (LEFT), formation board + strip
       on the right (RIGHT), the message log stretched by 2*ox;
     - top bar and bottom line (y 450-480): left part LEFT, right part RIGHT,
       the middle stretched by repeating the middle piece of the image;
     - the 3D view is widened by ox on each side (g_viewport_modes modes);
     - bottom-panel subscreens (NPC, spells, item, lock, trap)
       are laid out the same way: left part on the left, right part on the right, the middle
       stretched or centered with filled gaps (PlaceSubscreenPanel);
     - everything else (dialogs, tooltips, party movement panel) is centered.
   With W = 640 or outside the main screen all functions return the original numbers. */
#ifndef WIZ8_LOCAL_SCREENS_MAIN_GAME_LAYOUT_H
#define WIZ8_LOCAL_SCREENS_MAIN_GAME_LAYOUT_H

#include "wiz8/canvas.h"

struct W8ScreenRect;
struct Controls;
class W8Widget;

/* Main screen enter/exit: enable the layout (if the canvas is wide),
   set anchors on the mouse regions, widen the static regions; on
   exit restore everything and erase HUD leftovers in the side strips. */
void MainGameLayoutEnter(void);
void MainGameLayoutLeave(void);

/* An element stretched in width, in game (centered) coordinates:
   the left edge moves by -ox, the right by +ox. Outside the layout - unchanged. */
inline int MainGameWideLeft(int x)
{
    return x + CanvasAnchorShift(W8_ANCHOR_LEFT);
}
inline int MainGameWideRight(int x)
{
    return x + CanvasAnchorShift(W8_ANCHOR_RIGHT);
}

/* 3D view rectangle for mode (0..6): g_viewport_modes, widened
   by ox on both sides (top/bottom unchanged). right/bottom are exclusive. */
void MainGameViewportRect(int mode, W8ScreenRect* rect);

/* Draw a catalog image of width width with left edge left so that on the
   canvas it takes width + 2*ox: piece [0, cut_right) at the left edge (LEFT),
   piece [cut_right, width) at the right (RIGHT), between them repeats of the middle
   slice [cut_left, cut_right). No new art needed. Outside the layout - the usual
   DrawCatalogImage(AndInvalidate). */
void DrawCatalogImageWide(int object, int frame, int image, int left, int top, int width,
                          int cut_left, int cut_right, bool invalidate);

/* Combat round progress bar on the roof (image 0x8d, 286x13 at 177,2), filled to percent.
   The roof recess under it is widened by 2*ox (DrawCatalogImageWide of the roof), so the bar
   is stretched the same way (repeating its middle) and the fill runs over the whole recess
   177..463+2*ox. Only in the wide layout. */
void DrawCombatProgressWide(int percent);

/* A piece of a catalog image: the image in its own coordinates (left, top), the window
   origin on the canvas = origin (canvas_x = origin + image x), only the strip
   [clip_left, clip_right) in the same coordinates is visible. Not marked for re-upload.
   For custom wide-screen layouts (the map). */
void DrawCatalogImagePiece(int object, int frame, int image, int left, int top, int origin,
                           int clip_left, int clip_right);

/* Erase (make transparent) the rectangle [left, right) x [top, bottom),
   stretched like DrawCatalogImageWide: left edge at the left canvas edge,
   right edge at the right one. In layout A it also marks it dirty; outside it -
   only ClearSurfaceRect, as in the original. */
void ClearSurfaceRectWide(int left, int top, int right, int bottom);

/* A full-screen image (party death, ending) is drawn centered at 640x480.
   In layout A first erase the whole canvas (including the HUD in the side strips) and
   mark it dirty; outside the layout it does nothing (as in the original). */
void ClearCanvasForFullscreenArt(void);

/* Full-screen colored fade quad (BeginScreenFade, CreateSurpriseFade):
   width and window origin for Position2DNodeUnsnapped(node, 0, 0). In layout A -
   the whole canvas (W, origin 0), otherwise the original 640 centered. */
inline int FullscreenQuadWidth(void)
{
    return CanvasWideLayoutActive() ? g_canvas_w : W8_GAME_W;
}
inline int FullscreenQuadOrigin(void)
{
    return CanvasWideLayoutActive() ? 0 : CanvasDrawOrigin();
}

/* Cursor (game coordinates) inside the rectangle of an anchored element. */
bool IsCursorInAnchoredRectangle(int anchor, int left, int top, int right, int bottom);

/* Main log text rectangle (168,366)-(452,442), in layout A
   widened by ox on both sides; via SetTextBoxRegionBounds. */
void SetMainTextBoxRegionBounds(void);

/* ---------------------------------------------------------------------------
   Bottom-panel subscreens in layout A (2026-10-12): conversation/trade with
   an NPC, spellbook, item selection, lock, trap. Each is made of
   several Controls panels which in the original together occupy 23..617 x
   358..450. In layout A they are laid out across the wide panel the same way as
   the HUD itself: left part at the left edge (in place of the radar), right part at the right (in
   place of the formation board), the middle stretched (like the message log).
   Outside layout A none of this changes anything.
   --------------------------------------------------------------------------- */
enum W8SubscreenPlacement {
    W8_SUBSCREEN_LEFT = 0,       /* the panel and its children pinned to the left canvas edge */
    W8_SUBSCREEN_RIGHT = 1,      /* ... to the right edge */
    W8_SUBSCREEN_STRETCH = 2,    /* left edge at the left canvas edge, background wider by
                                    2*ox (slice repeat, image-specific for known
                                    images), children on the left */
    W8_SUBSCREEN_CENTER_FILL = 3 /* centered; gaps ox wide up to the neighbours on the left
                                    and right are filled with a continuation of its background
                                    (the background edges move to the gap edges) */
};

/* Place a panel (W8SubscreenPlacement): anchor of the panel and of its
   children's mouse regions + background mode (Controls::m_wideMode). Call after
   all children of the panel are created (children created later keep a centered region). */
void PlaceSubscreenPanel(Controls* panel, int placement);

/* Draw the panel background according to wide_mode (W8PanelWideMode), image
   (object, frame, image) with top-left corner (left, top). Returns false
   if nothing special needs doing (mode 0 or the layout is off) - then
   the caller draws as in the original. */
bool DrawSubscreenPanelArt(int wide_mode, int object, int frame, int image, int left, int top);

/* Mark the whole panel background dirty according to wide_mode, including the stretched
   part and the gaps. false - as above (the caller does it itself). */
bool InvalidateSubscreenPanelArt(int wide_mode, int object, int frame, int image, int left,
                                 int top, unsigned int flags);

/* A child of a stretched panel that stretches along with it (text area):
   move the right edge of its mouse region out by 2*ox. Outside the layout - the original
   bounds (may be called repeatedly, on every full redraw). */
void StretchWidgetRegion(W8Widget* widget);

/* How much the panel is stretched: 2*ox for a stretched (W8_SUBSCREEN_STRETCH)
   panel in layout A, otherwise 0. For drawing children that stretch along
   with it (right edge of their rectangle + this number). */
int SubscreenPanelStretch(const Controls* panel);

/* Mouse region of the whole bottom panel (stretched: left edge -ox, right +ox). */
void SetBottomPanelRegionBounds(unsigned int region_index);

#endif
