/* Wide full-screen backgrounds (mod, 2026-10-12). Not part of the original game.
   Details: wiz8-widescreen docs/wide-canvas.md, section "Wide full-screen backgrounds".

   Any 16-bit 640x480 image from the catalog (level loading screens,
   camp, party death, endings, main menu...) can have a "wide pair":
   the same path with _<W> appended to the name, where W is the canvas width:
       Data\Level Load\Arnika.sti  ->  Data\Level Load\Arnika_768.sti
   The pair is a 16-bit STI of exactly W x 480, stored as a separate file next to
   the original (or in an SLF). If the canvas is wider than 640 and the pair exists, it is
   drawn instead of the original - over the whole canvas, all W columns. No pair or W = 640 -
   everything as before.

   The image is read once (by the game's own STI loader, with the same color conversion
   as for the original surfaces) and kept in memory; "no pair" is also
   remembered, so the file is not searched for every frame. */
#ifndef WIZ8_WIDE_ART_H
#define WIZ8_WIDE_ART_H

/* Pixels of the wide pair for an original path (path - as in the catalog,
   "Data\\Level Load\\Arnika.sti"): W x 480, row = W pixels, in the
   SGP surface format (RGB 5:5:5 + alpha bit). 0 - canvas is 640 or there is no pair.
   The pointer lives until the next call into this module. */
const unsigned short* FindWideFullscreenArt(const char* path);

/* Draw the wide pair over the whole canvas (bypassing the 640 window and SGP clipping) and
   mark the side bars for re-upload; the center (640 window) is marked by the caller,
   as for the original. false - no pair, draw the original as before. */
bool DrawWideFullscreenArt(const char* path);

/* Put a piece of the wide background back on the canvas: the rectangle [left, right) x [top,
   bottom) in game coordinates (canvas_x = game_x + ox). false - no wide pair
   (W = 640 or no file), the caller restores the background the original way.
   For main menu items with a transparent background (Data\Options\introtext_*.sti). */
bool RestoreWideFullscreenArtRect(const char* path, int left, int top, int right, int bottom);

/* If a wide background is currently in the canvas side bars - wipe the bars
   (zeros: black/transparent) and re-upload them. Called when a screen with such a
   background ends (screen change in GameLoop, end of the death/ending screen),
   so the next screen does not see the old image on the sides. Otherwise nothing. */
void ReleaseWideFullscreenArt(void);

/* Whether a wide background is currently on the canvas (DrawWideFullscreenArt drew a pair,
   and the screen has not changed yet). The loading screen uses it to decide whether to stretch
   the caption strip and move the gears to the left edge. */
bool WideFullscreenArtOnCanvas(void);

/* "Stone wings" (2026-10-12): for UI windows made of marble panels
   (inventory and stats - the CAMP screen, character and party creation,
   map, options and saves, journal)
   the side bars are covered with marble instead of black, from the pair
   Data\Wide\WideFrame_<W>.sti (16 bit, W x 480; only the bars x < ox and
   x >= ox + 640 are used). Called from GameLoop every frame before the screen's frame: draws
   if the screen is in the list, layout A is off, there is no wide background and the wings
   are not there yet or were wiped (CanvasSideBarsClearCount). No file or
   W = 640 - nothing. ReleaseWideFullscreenArt removes them on a screen change. */
void UpdateWideSideWings(int screen);

/* Marble inset (2026-10-16): cover the canvas rectangle [left, right) x
   [top, bottom) with wing marble. bevels: like the window zone layout gaps -
   the left half is the left edge of the right wing (bevel next to the left neighbor), the right half is
   the right edge of the left wing (bevel next to the right neighbor); otherwise - stone from the middle
   of the left wing without bevels (mirrored repeats). brightness: 256 - as is,
   less - darker ("muted" stone, like the dark strip at the left edge of the
   info panel). Canvas coordinates, bypassing the 640 window and clipping. false -
   no wings file or the inset is wider than 2*ox. */
bool DrawWideMarbleGap(int left, int top, int right, int bottom, int brightness, bool bevels);

/* A piece of a wide pair (path - as for FindWideFullscreenArt) onto the canvas: the image
   rectangle [src_left, src_left + width) x [src_top, src_top + height) to the canvas
   point (dst_left, dst_top), bypassing the 640 window and clipping. For prepared art where
   one W x 480 image holds several pieces (slots of the lockpick panel, 2026-10-16).
   false - no pair. */
/* A stretched catalog image without seams (2026-10-16, the owner saw the seams
   of repeated slices at turned-up brightness). Prepared art
   Data\Wide\Stretch\S<object hex>_<frame>_<image>_<W>.sti (wiz8-widescreen scripts/art/stretch_quilt.py):
   at x [0, width + 2*ox), y from 0 lies the image stretched the same way as in
   DrawCatalogImageWide (left part, middle 2*ox, right part), but the middle is
   stitched from pieces of the image itself along an uneven line, rather than by repeating a slice.
   Drawn onto the canvas at (left, top); 0 - transparent; clipping [clip_*) - in
   640-window coordinates by the same column rule as for the slice repeat.
   false - no prepared art (then the old repeat is drawn). */
bool DrawWideStretchStrip(int object, int frame, int image, int left, int top, int width,
                          int cut_left, int cut_right, int clip_left, int clip_top,
                          int clip_right, int clip_bottom);
bool DrawWideArtPiece(const char* path, int src_left, int src_top, int width, int height,
                      int dst_left, int dst_top);

/* For full-screen render overlays (the camp image is not a drawing in the 2D window,
   but a separate full-screen layer): a copy of the wide pair, W x 480 in the
   SGP surface format. No pair or W = 640 - 0, and the caller takes the original
   640x480 as before. The buffer belongs to the module, is never freed and
   is overwritten by the next call (only one such overlay lives at a time). */
const unsigned short* BuildWideFullscreenOverlay(const char* path);

#endif
