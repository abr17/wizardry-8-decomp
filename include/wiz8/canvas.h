/* Wide 2D canvas, 2026-10-11. Not part of the original game.

   The idea is "a window inside a wide surface":
   - the whole game still thinks in 640x480 coordinates ("game" coordinates);
   - the 2D surface (DirectDraw + the texture used for rendering) is W = 640/768/856 wide;
   - the 640x480 game window is drawn centered, offset by ox = (W - 640) / 2;
   - the side bars are black (or show the 3D world if the game does not cover it).

   Coordinate conversion:  canvas_x = game_x + ox,   game_x = canvas_x - ox.
   With W = 640, ox = 0 and everything works as in the original.

   Settings: wiz8wide.ini next to the exe, section [Canvas]:
     Width=auto|640|768|856   (auto: from the screen aspect ratio in 3DVideo.CFG)
     HorPlus=1                (wider 3D view across the full canvas width)
     MouseScale=1|0           (convert screen pixels to canvas pixels;
                               default 1 when W > 640 and 0 when W = 640)
   The chosen value is written to wiz8wide.log. */
#ifndef WIZ8_CANVAS_H
#define WIZ8_CANVAS_H

enum {
    W8_GAME_W = 640,          /* width of game coordinates (never changes) */
    W8_GAME_H = 480,          /* height of both the game and the canvas */
    W8_CANVAS_MAX_W = 856,    /* widest supported width */
    W8_DIRTY_CELL = 8,        /* grid of 8x8-pixel "dirty" cells */
    W8_CANVAS_MAX_COLS = W8_CANVAS_MAX_W / W8_DIRTY_CELL, /* 107 */
    W8_CANVAS_ROWS = W8_GAME_H / W8_DIRTY_CELL,           /* 60 */
    W8_CANVAS_MAX_CELLS = W8_CANVAS_MAX_COLS * W8_CANVAS_ROWS /* 6420 */
};

/* Current canvas. Before CanvasInitialize() - the original's values (640, 0). */
extern int g_canvas_w;     /* canvas width, a multiple of 8 */
extern int g_canvas_ox;    /* game window offset: (W - 640) / 2 */
extern int g_canvas_cols;  /* cells per grid row: W / 8 */
extern int g_canvas_cells; /* total cells: W / 8 * 60 */

/* Once at video startup (InitializeVideoManager): reads the ini, picks W,
   writes the log. A repeated call does nothing. */
void CanvasInitialize(void);

/* Append a line to wiz8wide.log (next to the exe) and to the debugger. For rare
   events (not every frame). */
void CanvasLogLine(const char* text);

/* Borderless (wiz8wide.ini [Display] Borderless=1, default 1): a WS_POPUP
   window the size of the monitor, but the monitor resolution is NEVER changed -
   neither at startup nor for Bink videos (they go through the 2D canvas, in every display
   mode since 1.0.1). /WINDOW
   and the "full screen" toggle do nothing. Alt+Tab resets stuck
   Shift/Ctrl/Alt. Borderless=0 - the original's behavior.
   Read on the first call (may come before CanvasInitialize: the /WINDOW
   command line parsing happens before video starts). */
bool DisplayBorderlessEnabled(void);

/* Display mode and resolution (2026-10-16, in-game settings instead of 3D Setup).
   wiz8wide.ini [Display] Mode = borderless | fullscreen | window (no key -
   from the old Borderless: 1 -> borderless, 0 -> fullscreen); ScreenWidth /
   ScreenHeight - resolution for full screen and window (absent - the desktop's).
   In borderless the resolution is always the desktop's. Changed in game,
   take effect from the next launch (canvas W and the device are created once). */
enum W8DisplayMode { W8_DISPLAY_BORDERLESS = 0, W8_DISPLAY_FULLSCREEN = 1, W8_DISPLAY_WINDOW = 2 };
int DisplayModeSetting(void);
/* Videos go through the canvas (borderless and window), not exclusive 640x480. */
bool DisplayVideosOnCanvas(void);
void DisplayResolutionSetting(int* width, int* height);
/* What is currently selected in the settings (may differ from what the game was started with). */
void DisplaySaveSettings(int mode, int width, int height);
void DisplayPendingSettings(int* mode, int* width, int* height);
/* Before video starts: 3DVideo.CFG = DirectX7, the chosen resolution, 32 bits
   (the sound line is kept) - 3D Setup is no longer needed. */
void DisplayPrepareVideoConfig(void);
/* Make the settings selection current (for switching without a restart) and
   rewrite 3DVideo.CFG. */
void DisplayActivatePendingSettings(void);
/* Windows desktop resolution and work area (not the current monitor mode). */
void DisplayDesktopSize(int* width, int* height);
void DisplayDesktopWorkArea(struct tagRECT* work); /* RECT */
/* For this launch only: /WINDOW on the command line or a fallback after an
   unsupported full-screen mode (the ini is not changed). */
void DisplayForceMode(int mode);
/* Video2.cpp: the monitor supports this full-screen mode (SurRender). */
bool VideoFullscreenSizeSupported(int width, int height);
/* OptionsScreen.cpp: walk the monitor's mode list on a worker thread (it takes ~1 s). */
void StartScreenSizeScan(void);
/* Canvas width W that CanvasInitialize would pick for this output. */
int CanvasWidthForOutput(int width, int height);
/* Video2.cpp: apply the display mode and resolution from the settings right away.
   false - a different canvas width is needed (different screen aspect ratio): then
   it applies from the next launch. */
bool VideoApplyDisplaySettings(void);

/* 2D layer as a single texture (wiz8wide.ini [Display] SingleTexture2D=1,
   default 1). The original cuts the 2D surface into 128x128 textures and draws
   them as separate quads; when stretched to a large screen with bilinear
   filtering, stripes show at the seams every 128 canvas pixels.
   0 - 128 tiles, as in the original. The size is chosen by SurfaceTileExtent
   (Video2.cpp). */
bool DisplaySingleTexture2DEnabled(void);

/* Wide map (wiz8wide.ini [Canvas] WideAutomap=1, default 1): when
   W > 640 the automap field is stretched across the full canvas width, buttons at
   the right edge (AutomapScreen.cpp). 0 - a 640 map centered, stone wings on the sides. */
bool CanvasWideAutomapEnabled(void);

/* Wide UI windows (wiz8wide.ini [Canvas] WideWindows=1, default
   1): when W > 640, windows reworked for the wide canvas (options and saves,
   journal) turn on the canvas layout and stretch their panels to the edges
   (wiz8-widescreen docs/history/responsive-ui-plan.md). 0 - a 640 window centered with stone wings
   on the sides. */
bool CanvasWideWindowsEnabled(void);

/* Speed measurement (wiz8wide.ini [Debug] LogFps=1, default 0): called
   once per frame from RenderFrame, every 5 seconds writes the line
   "[Perf] ... fps" to wiz8wide.log. */
void CanvasCountFrame(void);

/* Frame rate limit (wiz8wide.ini [Display] MaxFps, default
   W8_DEFAULT_MAX_FPS; 0 - unlimited). The original engine is designed for
   tens of frames per second: it zeroes a per-frame movement step smaller than
   0.1 world units (AdvanceEnvironmentMotion), and from a standstill the party
   moves 112.5 * dt^2 in one frame (dt in tenths of a second). Above ~330 frames
   per second the movement is below the threshold, and on a slope (ground contact
   every frame) the party cannot start moving. The original runs at ~220 fps on
   this machine and gets through; the build without a limit ran at ~1000 and got
   stuck. Called once per frame from RenderFrame, after CanvasCountFrame. */
enum { W8_DEFAULT_MAX_FPS = 120 };
int DisplayMaxFps(void);
void CanvasLimitFrameRate(void);

/* Frame capture on request (for automated tests): if the file wiz8shot.req
   appears next to the exe, it is deleted and true is returned - RenderFrame then
   saves the frame with the stock SaveJpegScreenshot (Wiz8NNNNN.JPG next to the game).
   The renderer itself captures it, so it works even with the monitor off, when a
   Windows screenshot is black. The file is checked at most once per 200 ms. */
bool CanvasTakeScreenshotRequest(void);

/* Draw distance (wiz8wide.ini [Display] ViewDistance, multiplier k,
   1.0 = as in the original, capped at W8_MAX_VIEW_DISTANCE). The camera far
   plane, fog and octree visibility are scaled by k; gameplay
   distances (monster sight, group loading, AI, spells) stay
   unchanged (3d.h WorldGetFarClip / WorldGetRenderFarClip). Monster groups
   are loaded only closer than 1.5 x the gameplay distance, so k > 1.5 makes
   monsters appear in a light haze. */
#define W8_DEFAULT_VIEW_DISTANCE "1.5"
#define W8_MAX_VIEW_DISTANCE 2.0f
float DisplayViewDistanceScale(void);
/* Slider in the graphics settings (2026-10-16): the new distance (1.0 ..
   W8_MAX_VIEW_DISTANCE) goes straight into the current world's camera and into wiz8wide.ini. */
void DisplaySetViewDistanceScale(float scale);

/* Whether Hor+ is on (3D view out to the canvas edges). Only meaningful when ox > 0. */
bool CanvasHorPlusEnabled(void);

/* Does the 3D view touch the 4:3 edges (left <= 23, right >= 618)? Then it can be
   widened to the whole canvas. right is an exclusive bound (like g_viewport.right). */
bool CanvasHorPlusApplies(int left, int right_exclusive);

/* 2D sprite normalization: fraction of the canvas width per pixel (1 / W) and
   the top of the "square" camera (480 / W, 0.75 in the original). */
double CanvasInverseWidth(void);
double CanvasSquareTop(void);
/* The same 1 / W, but as float - replaces g_viewport_x_scale (float 1/640) where it
   was used to normalize sprite width (automap markers). At W = 640 the value
   is bit-for-bit equal to the original constant. */
float CanvasSpriteScaleX(void);

/* Row stride (pitch, bytes) of the canvas DirectDraw surface; remembered when
   it is created. Until known - 0x500 (as in the original at 640 pixels). */
void CanvasSetFrameBufferPitch(unsigned int pitch);
unsigned int CanvasFrameBufferPitch(void);

/* Mouse. "Screen" coordinates are what the original code sees: in full-screen
   mode, screen coordinates; in windowed mode, coordinates inside the client area.
   The canvas is stretched over the whole client area of the window, so
     canvas_x = screen_x * W / window_width,  canvas_y = screen_y * 480 / window_height
   (rounded down). The reverse conversion picks a screen point that is
   guaranteed to map back to the same canvas cell. With MouseScale=0 both
   conversions are the identity (as in the original). */
void CanvasScreenToCanvas(int screen_x, int screen_y, int* canvas_x, int* canvas_y);
void CanvasCanvasToScreen(int canvas_x, int canvas_y, int* screen_x, int* screen_y);
bool CanvasMouseScaleEnabled(void);

/* Whether mouse coordinate conversion is on at all (canvas wider than 640 or MouseScale).
   If not, all mouse paths work exactly as in the original. */
bool CanvasMouseMapActive(void);

/* Does the canvas point lie in a side bar (outside the 640x480 window)? With ox = 0 - never. */
bool CanvasIsInSideBar(int canvas_x);

/* Canvas -> game coordinates, clamped to [0, min(max_x, 639)] x
   [0, min(max_y, 479)]. Clamping applies only if CanvasMouseMapActive(),
   otherwise values pass through as in the original. */
void CanvasToGame(int canvas_x, int canvas_y, int max_x, int max_y, int* game_x, int* game_y);

/* The full "screen/client area -> canvas -> game" path for mouse events
   (MouseHandler, wheel in WindowProcedure). in_side_bar (may be 0)
   reports that the cursor is in a side bar. */
void CanvasScreenToGame(int screen_x, int screen_y, int* game_x, int* game_y, bool* in_side_bar);

/* ---------------------------------------------------------------------------
   Layout "A" of the main game screen (HUD at the edges of the wide canvas), 2026-10-11.
   Details: wiz8-widescreen docs/wide-canvas.md, section "HUD layout "A"".

   Game coordinates stay "centered" (canvas_x = game_x + ox), but
   while the layout is on they are NOT clamped to 0..639: the mouse gives -ox .. 639+ox,
   i.e. the whole canvas. Each HUD element knows its "anchor":
     CENTER - as before, centered (shift 0);
     LEFT   - pinned to the left canvas edge  (shift -ox);
     RIGHT  - pinned to the right canvas edge (shift +ox).
   The element's coordinates in the code do NOT change (they are still 640-based):
   - drawing: while the element is drawn, the "window origin" on the canvas is shifted
     (CanvasDrawScope) - everything drawn via LockPrimarySurface,
     InvalidateRegion, ClearSurfaceRect, 2D sprites lands where it should;
   - mouse: a mouse region stores its anchor, and the hit test subtracts its shift
     from the cursor coordinate (RegionManager, SGP mousesystem).
   Stretched elements (log, 3D view, roof, bottom panel) are CENTER with
   extended coordinates (left edge -ox, right +ox) or pieces with the three
   anchors. At W = 640 (ox = 0) all shifts are zero: the original's behavior.
   --------------------------------------------------------------------------- */
enum W8LayoutAnchor {
    W8_ANCHOR_CENTER = 0, /* default: everything unmarked behaves as before */
    W8_ANCHOR_LEFT = 1,
    W8_ANCHOR_RIGHT = 2
};

/* Turn layout A on/off (entering/leaving the main screen). Turns on
   only when ox > 0. Turning off fills the surface side bars with zeros and
   marks the whole canvas dirty so the next screen does not see HUD leftovers. */
void CanvasSetWideLayout(bool active);
bool CanvasWideLayoutActive(void);

/* Fill both side bars of the 2D surface with zeros (x < ox and x >= ox + 640),
   surface memory only; reaches the texture after InvalidateCanvas /
   ResetTransientRenderScenes. For screens that drew into the bars themselves
   (the wide main menu background). With ox = 0 does nothing. */
void CanvasClearSideBars(void);

/* Side bar clear counter: grows on every wipe of the whole surface
   or of the bars (ClearPrimarySurface, ResetVideoFrameState, CanvasClearSideBars,
   drawing a wide background on top). The "stone wings" (wide_art.h) use it to learn
   that they were wiped and must be redrawn. CanvasNoteSideBarsCleared is called by whoever wipes. */
void CanvasNoteSideBarsCleared(void);
unsigned int CanvasSideBarsClearCount(void);

/* Game x shift for an anchor: 0 / -ox / +ox (0 while the layout is off). */
int CanvasAnchorShift(int anchor);

/* Where on the canvas game x = 0 currently is for 2D drawing (ox by default). */
int CanvasDrawOrigin(void);
void CanvasSetDrawOrigin(int canvas_x);

/* RAII (like try/finally in JS): the constructor shifts the drawing origin for the
   anchor, the destructor restores the previous one. Nested scopes work. */
struct CanvasDrawScope {
    explicit CanvasDrawScope(int anchor);
    ~CanvasDrawScope();
    int saved_origin;
};

/* The same, but the origin is given as a number (canvas_x of game zero) - for pieces
   of stretched images. */
struct CanvasOriginScope {
    explicit CanvasOriginScope(int canvas_origin);
    ~CanvasOriginScope();
    int saved_origin;
};

/* ---------------------------------------------------------------------------
   Window "zone layout" (2026-10-12). Details: wiz8-widescreen docs/wide-canvas.md,
   section "Window zone layout".

   A game window still draws itself entirely into the centered 640x480 window of the canvas
   (as now, the window code does not change). But what goes to the screen is not the canvas
   itself, but its "composite": pieces of the window (zones) are moved left/right on the
   canvas or stretched by repeating a strip, and the gaps between pieces are filled with
   marble. A web analogy: a page laid out with absolute 640x480 coordinates,
   and CSS transforms spread its blocks across a wide screen.
   The mouse is converted back (canvas -> game 0..639), so regions,
   buttons and dragging in the window code work without changes.

   Zone: the window rectangle [left, right) x [top, bottom) in game
   coordinates is placed on the canvas so that its x = left lands at canvas_x =
   dst_left. fill_right != 0 - instead of a single move, the strip [left, right)
   is repeated from dst_left up to canvas_x = fill_right (background stretching).
   Marble fill: a canvas rectangle.
   Modal windows (dialogs, messages) are shown centered on top of the layout
   while displayed: CanvasZoneOverlay reports their frame every frame. */
struct W8CanvasZone {
    short left, top, right, bottom; /* game coordinates */
    short dst_left;                 /* canvas_x where left lands */
    short fill_right;               /* 0 or the canvas_x where stretching ends */
};
struct W8CanvasFill {
    short left, top, right, bottom; /* canvas */
};

/* Turn the layout on (tables are copied, up to 32 zones and 16 fills). The whole
   canvas is recomposited. Only when ox > 0; at W = 640 does nothing. */
void CanvasSetZoneLayout(const W8CanvasZone* zones, int zone_count, const W8CanvasFill* fills,
                         int fill_count);
void CanvasClearZoneLayout(void);
bool CanvasZoneLayoutActive(void);
/* Game point -> canvas_x (by the zone it lies in; outside zones - x + ox). */
int CanvasZoneMapX(int game_x, int game_y);
/* Canvas -> game x: by the zone the point falls in; in a gap - the edge of the
   nearest zone in the same row. */
int CanvasZoneUnmapX(int canvas_x, int canvas_y);
/* A modal window (game rectangle) is drawn this frame: show it
   centered on top of the layout. When the call stops, the layout is restored. */
void CanvasZoneOverlay(int left, int top, int right, int bottom);

/* For Video2 (canvas compositing): the layout data. */
int CanvasZoneCount(void);
const W8CanvasZone* CanvasZoneAt(int index);
int CanvasZoneFillCount(void);
const W8CanvasFill* CanvasZoneFillAt(int index);
/* The modal rectangle in canvas coordinates; false - none. */
bool CanvasZoneOverlayRect(int* left, int* top, int* right, int* bottom);
/* Once per frame (FlushDirtyTiles): whether the modal rectangle expires, whether
   the whole canvas must be recomposited and the gaps filled. */
void CanvasZoneBeginFlush(bool* recomposite_all, bool* refill);

/* Portrait column anchor for a party slot: even slots - left column,
   odd - right. */
inline int CanvasPartySlotAnchor(int party_slot)
{
    return (party_slot & 1) != 0 ? W8_ANCHOR_RIGHT : W8_ANCHOR_LEFT;
}

#endif
