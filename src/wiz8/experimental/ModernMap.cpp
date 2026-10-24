/* Experimental (2026-10-21, the patch author's request): mouse controls on the automap.
   Not part of the original game; see include/wiz8/experimental.h for the rules of the
   experimental features (off by default, removable whole). Design: docs/experimental.md.

   The original map zooms in on a left click and out on a right click, in fixed steps; the view
   moves only with the arrow keys and buttons. With [Experimental] ModernMap=1:
   - the wheel zooms in small steps towards the point under the cursor;
   - dragging with the left button moves the map with it (hand cursor);
   - a double click does what the original's single click did (ZoomAutomapIn at the cursor:
     the whole map -> the middle level -> the closest, where it centres the view); a single
     click does nothing (in the first version it zoomed, and a drag that the game read as a
     click zoomed too) except on the party marker, where the original hides the cursor: there
     it is the original's click too;
   - the right click, the note tools, the keys and the buttons work as in the original (the
     patch author: as close to the original as possible, only the wheel and the left button
     are new; a version with x2 steps on the Zoom-In button, Enter, the right click and the
     double click was dropped).

   The camera model of AutomapScreen.cpp: an orthographic camera looks straight down from
   g_automap_position; g_automap_zoom is the visible world height (z); the visible width (x)
   is that times the field's aspect (wider on the wide canvas); a field point (u, v) in 0..1 is
   the world point x = pos.x + (u - 0.5) * width, z = pos.z - (v - 0.5) * zoom. The camera's
   height only decides what is clipped: the original puts it the zoom above the ground it
   traces under the clicked point (AutomapGroundAt), at least 25000 (its closest zoom), and at
   g_automap_top_y for the whole map; the zooms here do the same. The first version took the
   ground as the camera height minus the zoom, which after the whole-map view is the lowest
   point of the level: on levels with floors above it a close zoom put the camera under them
   and the map went black.

   Speed: the original's camera update (SetAutomapCameraPoint) plus the redraw it triggers
   switch the texture policy twice (every resident texture is thrown away and uploaded again)
   and recompute the octree visibility; fine once per click, far too slow every frame of a
   drag. During a gesture the camera is moved directly ("live": RenderAutomapFrame then keeps
   the textures, the visibility is refreshed at most every 100 ms), and when it ends the
   original update runs once, so the final picture is the original's.

   Input: each event carries the cursor image's corner in game coordinates (uiParam); the
   pointer tip is that plus the hotspot. Button and move positions come from the events (the
   polled cursor is only updated once per frame), and the state machine does not depend on the
   live button state, so a release queued after the last moves is never lost. The wheel takes
   the polled pointer: its message carries screen pixels when the canvas has no mouse mapping. */
#include <windows.h>
#include "wiz8/experimental.h"
#include "wiz8/canvas.h"
#include "wiz8/cursor.h"
#include "wiz8/float_constants.h"
#include "wiz8/vector.h"
#include "wiz8/local_screens/AutomapScreen.h"
#include "wiz8/engine_code/Video2.h"
#include "wiz8/engine_code/World.h"
#include "wiz8/engine_code/stModelInstance.h"
#include "surrender/srCamera.h"
#include "surrender/srGERD.h"
#include "input.h"

#include <stdio.h>

/* AutomapScreen.cpp */
extern srVector3T<float> g_automap_position;
extern srVector3T<float> g_automap_bounds_min;
extern srVector3T<float> g_automap_bounds_max;
extern float g_automap_zoom;
extern float g_automap_top_y;
extern int g_automap_tool;
extern int g_automap_layer;
extern bool g_automap_cursor_inside;
extern bool g_automap_redraw;
extern bool g_automap_overlay_redraw;
extern stModelInstance2D* g_automap_party_marker;
extern W8WorldCameraState g_automap_saved_camera; /* the party's camera (the marker's place) */
void SetAutomapCameraPoint(srVector3T<float>* position);
void SetAutomapButtonMode(int update);
void SetAutomapLayer(int layer);
float AutomapGroundAt(float x, float z);
unsigned char ZoomAutomapIn(const srVector3T<float>* point);
/* Video2.cpp: the cursor hotspot (event corner + hotspot = pointer tip). */
extern int g_cursor_hotspot_x;
extern int g_cursor_hotspot_y;

namespace {

enum State { IDLE, PRESSED, DRAGGING };

const int kDragThresholdPx = 3;     /* game pixels before a press becomes a drag */
const int kDoubleClickSlopPx = 4;   /* the second click this close to the first */
const int kWheelUnit = 30;          /* a quarter notch (high-resolution wheels) */
const float kZoomInPerUnit = 0.945742f;  /* 0.8^(1/4): one notch = x0.8 */
const float kZoomOutPerUnit = 1.057371f; /* 1.25^(1/4) */
const DWORD kSettleDelayMs = 150;   /* after the last wheel step */
const DWORD kVisibilityRefreshMs = 100;
const int kZoomModeFree = 3;        /* our zoom-button mode: both buttons, not the original's 1 */

int s_setting = -1; /* -1 not read yet, else 0/1 */

State s_state;
int s_p0x, s_p0y;              /* press / rebase point (pointer tip, game pixels) */
int s_px, s_py;                /* latest point */
srVector3T<float> s_c0;        /* camera at the press / rebase */
srVector3T<float> s_last_set;  /* last camera position set here */
float s_last_zoom;             /* and the zoom with it */
bool s_second_click;
DWORD s_last_click_time;
int s_last_click_x, s_last_click_y;
bool s_up_seen;
int s_async_up_frames;
bool s_swallow_right_up;
int s_wheel_accum;
float s_wheel_u, s_wheel_v;
bool s_live;                   /* the camera was moved without the original update */
bool s_live_textures;          /* a live redraw made reduced-detail world textures */
DWORD s_settle_at;
bool s_settle_now;
bool s_cursor_dirty;
DWORD s_vis_tick;

void Log(const char* text)
{
    char line[160];
    _snprintf(line, sizeof(line), "[ModernMap] %s", text);
    line[sizeof(line) - 1] = '\0';
    CanvasLogLine(line);
}

void EventTip(unsigned long position, int* x, int* y)
{
    *x = static_cast<short>(LOWORD(position)) + g_cursor_hotspot_x;
    *y = static_cast<short>(HIWORD(position)) + g_cursor_hotspot_y;
}

/* Windowed: with the OS cursor outside the client area SyncSystemCursor keeps the last inside
   position, while Windows can still send the wheel here. */
bool OsCursorOutsideClient(void)
{
    if (g_fullscreen) {
        return false; /* SyncSystemCursor clamps there, never stale */
    }
    POINT p;
    RECT rc;
    if (!GetCursorPos(&p) || !ScreenToClient(ghWindow, &p) || !GetClientRect(ghWindow, &rc)) {
        return false;
    }
    return p.x < rc.left || p.y < rc.top || p.x >= rc.right || p.y >= rc.bottom;
}

bool InField(int x, int y)
{
    return x >= g_viewport.left && y >= g_viewport.top && x <= g_viewport.right &&
           y <= g_viewport.bottom;
}

/* Field coordinates 0..1 (not clamped: a drag may leave the field). */
void ToViewport(int x, int y, float* u, float* v)
{
    *u = (x - g_viewport.left) / static_cast<float>(g_viewport.right - g_viewport.left);
    *v = (y - g_viewport.top) / static_cast<float>(g_viewport.bottom - g_viewport.top);
}

/* Visible world width / height (as AutomapWorldWidth). */
float WidthPerZoom(void)
{
    int ox = CanvasWideLayoutActive() ? g_canvas_ox : 0;
    return static_cast<float>(455 + 2 * ox) / 455.0f;
}

void UpdatePartyMarkerScale(void)
{
    float factor = (1.0f / (g_automap_zoom * 0.00004f)) * 0.44f; /* as AutomapScreenFrame */
    srVector3T<double> scale(factor, factor, factor);
    g_automap_party_marker->setScale(scale);
}

void UpdateZoomButtons(float zoom, float closest, float farthest)
{
    int want = zoom >= farthest ? 0 : zoom <= closest ? 2 : kZoomModeFree;
    if (want != g_automap_zoom_mode) {
        SetAutomapButtonMode(want == kZoomModeFree ? 1 : want);
        g_automap_zoom_mode = want;
    }
}

/* The camera without the original update (no texture flush; visibility now and then). */
void MoveCameraLive(srVector3T<float> wanted, bool height_changed)
{
    srVector3T<float> c = wanted;
    c.x = c.x < g_automap_bounds_min.x ? g_automap_bounds_min.x : c.x > g_automap_bounds_max.x ? g_automap_bounds_max.x : c.x;
    c.z = c.z < g_automap_bounds_min.z ? g_automap_bounds_min.z : c.z > g_automap_bounds_max.z ? g_automap_bounds_max.z : c.z;
    g_world->camera->setLocation(srVector3T<double>(c.x, c.y, c.z));
    g_automap_position = c;
    s_last_set = c;
    s_last_zoom = g_automap_zoom;
    if (height_changed) {
        SetAutomapLayer(g_automap_layer); /* near clip follows the height */
    }
    g_automap_redraw = true;
    g_automap_overlay_redraw = true;
    s_live = true;
    DWORD now = GetTickCount();
    if (g_world->octree != 0 && (height_changed || now - s_vis_tick >= kVisibilityRefreshMs)) {
        MarkRendererReady();
        UpdateWorldMeshAfterLoad();
        s_vis_tick = now;
    }
}

/* The whole-map zoom (ResetAutomapZoom). */
float FarthestZoom(void)
{
    return g_automap_top_y - g_automap_bounds_min.y;
}

/* The camera height for a zoom, as the original's ZoomAutomapIn: the zoom above the ground
   under the view (the higher of the ground under the anchor and under the new centre), the
   whole map at top_y. Below the whole map it stays under top_y: the original Zoom-Out button
   and Backspace act only there. */
float CameraHeight(float anchor_x, float anchor_z, float centre_x, float centre_z, float zoom)
{
    if (zoom >= FarthestZoom()) {
        return g_automap_top_y;
    }
    float ground = AutomapGroundAt(anchor_x, anchor_z);
    if (centre_x != anchor_x || centre_z != anchor_z) {
        const float centre_ground = AutomapGroundAt(centre_x, centre_z);
        ground = centre_ground > ground ? centre_ground : ground;
    }
    const float y = ground + zoom;
    const float highest = g_automap_top_y - 1.0f;
    return y < highest ? y : highest;
}

/* Zoom math shared by the live wheel and the committed zooms: false - at the limit. */
bool ZoomTarget(float u, float v, float factor, srVector3T<float>* position, float* zoom_out,
                float* closest_out, float* farthest_out)
{
    const float closest = g_float_twenty_five_thousand;
    const float farthest = FarthestZoom();
    *closest_out = closest;
    *farthest_out = farthest;
    if (farthest <= closest) {
        return false;
    }
    float zoom = g_automap_zoom * factor;
    zoom = zoom < closest ? closest : zoom > farthest ? farthest : zoom;
    if (zoom == g_automap_zoom) {
        return false;
    }
    const float aspect = WidthPerZoom();
    const float world_x = g_automap_position.x + (u - 0.5f) * g_automap_zoom * aspect;
    const float world_z = g_automap_position.z - (v - 0.5f) * g_automap_zoom;
    position->x = world_x - (u - 0.5f) * zoom * aspect;
    position->z = world_z + (v - 0.5f) * zoom;
    position->y = CameraHeight(world_x, world_z, position->x, position->z, zoom);
    *zoom_out = zoom;
    return true;
}

void ZoomLive(float u, float v, float factor)
{
    srVector3T<float> position;
    float zoom, closest, farthest;
    if (!ZoomTarget(u, v, factor, &position, &zoom, &closest, &farthest)) {
        return;
    }
    g_automap_zoom = zoom;
    MoveCameraLive(position, true);
    UpdateZoomButtons(zoom, closest, farthest);
}

/* The original's hover test (AutomapScreenFrame, MOUSE_POS): the pointer within 5% of the
   visible height of the party, no tool, not at the closest zoom - there the original hides the
   cursor and enlarges the party marker. */
bool PointerOnParty(int x, int y)
{
    if (g_automap_tool != 0 || g_automap_zoom_mode == 2 || !InField(x, y)) {
        return false;
    }
    float u, v;
    ToViewport(x, y, &u, &v);
    const float dx = g_automap_position.x + (u - 0.5f) * g_automap_zoom * WidthPerZoom() -
                     g_automap_saved_camera.position.x;
    const float dz = g_automap_position.z - (v - 0.5f) * g_automap_zoom - g_automap_saved_camera.position.z;
    const float r = g_automap_zoom * 0.05f;
    return dx * dx + dz * dz < r * r;
}

/* The original's single click (the double click, and a click on the party marker): its next
   fixed level at the pointer (ZoomAutomapIn: the whole map -> the middle level -> the closest,
   where it centres the view). */
void OriginalClickZoom(int x, int y)
{
    float u, v;
    ToViewport(x, y, &u, &v);
    srVector3T<float> point;
    point.x = u;
    point.y = v;
    point.z = 0.0f;
    s_live = false;
    s_settle_at = 0;
    ZoomAutomapIn(&point);
    UpdatePartyMarkerScale();
}

/* The camera was changed by other means during a gesture: continue from where it is now.
   After a pan (arrow keys, buttons) the start moves with it; after a zoom (Enter, Backspace,
   Home, the buttons) the press point restarts too, as the pixel offset no longer matches the
   scale (in the first version the drag then put the old camera height back). */
void Rebase(void)
{
    if (g_automap_zoom != s_last_zoom || g_automap_position.y != s_last_set.y) {
        s_c0 = s_last_set = g_automap_position;
        s_last_zoom = g_automap_zoom;
        s_p0x = s_px;
        s_p0y = s_py;
    } else if (g_automap_position.x != s_last_set.x || g_automap_position.z != s_last_set.z) {
        s_c0.x += g_automap_position.x - s_last_set.x;
        s_c0.z += g_automap_position.z - s_last_set.z;
        s_last_set = g_automap_position;
    }
}

void ApplyPan(void)
{
    Rebase();
    const float vp_w = static_cast<float>(g_viewport.right - g_viewport.left);
    const float vp_h = static_cast<float>(g_viewport.bottom - g_viewport.top);
    srVector3T<float> wanted = s_c0;
    wanted.y = g_automap_position.y; /* a drag keeps the height, as the arrow keys do */
    wanted.x -= (s_px - s_p0x) * g_automap_zoom * WidthPerZoom() / vp_w;
    wanted.z += (s_py - s_p0y) * g_automap_zoom / vp_h;
    if (wanted.x == g_automap_position.x && wanted.z == g_automap_position.z) {
        return;
    }
    MoveCameraLive(wanted, false);
    /* stopped at the edge: follow again as soon as the mouse turns back */
    s_c0.x += g_automap_position.x - wanted.x;
    s_c0.z += g_automap_position.z - wanted.z;
}

void Settle(void)
{
    s_live = false;
    s_settle_now = false;
    s_settle_at = 0;
    SetAutomapCameraPoint(&g_automap_position); /* the original update */
    UpdatePartyMarkerScale();
}

/* lost: the button release never came (ModernMapFrame), the only gesture end logged. */
void EndGesture(bool lost)
{
    s_state = IDLE;
    s_cursor_dirty = true;
    s_settle_now = true;
    if (lost) {
        char text[80];
        _snprintf(text, sizeof(text), "up lost zoom=%d", static_cast<int>(g_automap_zoom));
        text[sizeof(text) - 1] = '\0';
        Log(text);
    }
}

} // namespace

bool ExperimentalModernMapEnabled(void)
{
    if (s_setting < 0) {
        s_setting = ExperimentalReadSwitch("ModernMap") ? 1 : 0;
    }
    return s_setting == 1;
}

void ExperimentalSetModernMap(bool enabled)
{
    s_setting = enabled ? 1 : 0;
    ExperimentalWriteSwitch("ModernMap", enabled);
}

bool ModernMapDragging(void)
{
    return s_state == DRAGGING;
}

bool ModernMapLiveRedraw(void)
{
    return s_live && ExperimentalModernMapEnabled();
}

void ModernMapNoteLiveRedraw(void)
{
    s_live_textures = true;
}

void ModernMapLeave(void)
{
    if (s_live_textures && g_gerd != 0) {
        /* reduced-detail world textures from live redraws must not reach the 3D view */
        g_gerd->invalidateResidentTextures();
        g_gerd->invalidateTextureCache();
    }
    s_live_textures = false;
    s_live = false;
    s_state = IDLE;
    s_wheel_accum = 0;
    s_settle_at = 0;
    s_settle_now = false;
    s_cursor_dirty = false;
    s_swallow_right_up = false;
    s_up_seen = false;
    s_async_up_frames = 0;
    s_last_click_time = 0;
}

bool ModernMapHandleEvent(unsigned short event, unsigned long param, unsigned long position,
                          unsigned long time)
{
    if (!ExperimentalModernMapEnabled()) {
        return false;
    }
    int x, y;
    EventTip(position, &x, &y);
    switch (event) {
    case LEFT_BUTTON_DOWN:
        if (g_automap_tool != 0 || !InField(x, y)) {
            return false;
        }
        s_state = PRESSED;
        s_p0x = s_px = x;
        s_p0y = s_py = y;
        s_c0 = s_last_set = g_automap_position;
        s_last_zoom = g_automap_zoom;
        s_up_seen = false;
        s_async_up_frames = 0; /* a count left by an earlier gesture would end this one */
        s_second_click = s_last_click_time != 0 && time - s_last_click_time <= GetDoubleClickTime() &&
                         abs(x - s_last_click_x) <= kDoubleClickSlopPx &&
                         abs(y - s_last_click_y) <= kDoubleClickSlopPx;
        return true;
    case MOUSE_POS:
        if (s_state == IDLE) {
            return false; /* the original hover */
        }
        s_px = x;
        s_py = y;
        if (s_state == PRESSED &&
            (abs(x - s_p0x) > kDragThresholdPx || abs(y - s_p0y) > kDragThresholdPx)) {
            s_state = DRAGGING;
            s_p0x = x; /* rebase: the map does not jump by the threshold */
            s_p0y = y;
            s_c0 = s_last_set = g_automap_position;
            s_last_zoom = g_automap_zoom;
            s_cursor_dirty = true;
            s_second_click = false;
            s_last_click_time = 0;
        }
        return true;
    case LEFT_BUTTON_UP:
        s_up_seen = true;
        if (s_state == IDLE) {
            /* an orphan release; tools as before. Also where the original would take it as in
               the field (the polled cursor, a frame later than the event): no zoom here */
            return g_automap_tool == 0 && (InField(x, y) || IsCursorInsideViewport());
        }
        if (s_state == DRAGGING) {
            s_px = x;
            s_py = y;
            ApplyPan();
            EndGesture(false);
            return true;
        }
        /* a press without moving */
        s_state = IDLE;
        if (PointerOnParty(x, y)) {
            /* the only single click that acts: on the party marker (the cursor vanishes there),
               as the original's click */
            OriginalClickZoom(x, y);
            s_last_click_time = 0;
            if (!PointerOnParty(x, y)) {
                EnableCursorScene(); /* the original shows it again on the next mouse move */
            }
            return true;
        }
        if (s_second_click) {
            OriginalClickZoom(x, y);
            s_last_click_time = 0;
        } else {
            s_last_click_time = time;
            s_last_click_x = x;
            s_last_click_y = y;
        }
        return true;
    case LEFT_BUTTON_REPEAT:
    case LEFT_BUTTON_DBL_CLK:
        return s_state != IDLE || (g_automap_tool == 0 && InField(x, y));
    case RIGHT_BUTTON_DOWN:
        if (s_state != IDLE) {
            s_swallow_right_up = true;
            return true;
        }
        s_swallow_right_up = false; /* a swallowed release that never came (outside the window) */
        return false;
    case RIGHT_BUTTON_UP:
        if (s_swallow_right_up) {
            s_swallow_right_up = false;
            return true;
        }
        if (s_state != IDLE) {
            return true;
        }
        /* the original zooms out, or drops the tool; its ResetAutomapZoom sets the magnifier
           inline, so the cursor is set again after the events */
        s_cursor_dirty = true;
        return false;
    case MOUSE_WHEEL: {
        /* the wheel message's position is in screen pixels when the canvas has no mouse
           mapping (640 wide, windowed): take the polled pointer, as the other events give */
        POINT tip;
        SGPMouseGetPos(&tip);
        x = tip.x;
        y = tip.y;
        if (s_state == IDLE && (OsCursorOutsideClient() || !InField(x, y))) {
            return false;
        }
        s_wheel_accum += static_cast<short>(HIWORD(param));
        if (s_state == DRAGGING) {
            ToViewport(s_px, s_py, &s_wheel_u, &s_wheel_v);
        } else {
            ToViewport(x, y, &s_wheel_u, &s_wheel_v);
        }
        return true;
    }
    default:
        return false;
    }
}

void ModernMapFrame(void)
{
    if (!ExperimentalModernMapEnabled()) {
        return;
    }
    /* a release that never arrived (outside the window, a dropped event) */
    if (s_state != IDLE && !s_up_seen) {
        int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
        bool up_now = (GetAsyncKeyState(button) & 0x8000) == 0;
        s_async_up_frames = up_now ? s_async_up_frames + 1 : 0;
        if (s_async_up_frames >= 2) {
            s_async_up_frames = 0;
            EndGesture(true);
        }
    }
    s_up_seen = false;
    /* the camera moved or zoomed by other means during a gesture (keys, buttons) */
    if (s_state != IDLE) {
        Rebase();
    }
    if (s_state == DRAGGING) {
        ApplyPan();
    }
    /* wheel: whole quarter notches, the remainder kept */
    float factor = 1.0f;
    while (s_wheel_accum >= kWheelUnit) {
        factor *= kZoomInPerUnit;
        s_wheel_accum -= kWheelUnit;
    }
    while (s_wheel_accum <= -kWheelUnit) {
        factor *= kZoomOutPerUnit;
        s_wheel_accum += kWheelUnit;
    }
    if (factor != 1.0f) {
        ZoomLive(s_wheel_u, s_wheel_v, factor);
        s_settle_at = GetTickCount() + kSettleDelayMs;
        if (s_state != IDLE) {
            s_c0 = g_automap_position;
            s_p0x = s_px;
            s_p0y = s_py;
        }
    }
    if (s_cursor_dirty) {
        s_cursor_dirty = false;
        if (s_state == DRAGGING) {
            EnableCursorScene(); /* the cursor may be hidden over the party marker */
        } else {
            g_automap_cursor_inside = IsCursorInsideViewport();
        }
        SetAutomapToolCursor(g_automap_tool);
    }
    if (!s_live) {
        s_settle_now = false; /* nothing live to settle (a drag that never moved, a committed zoom) */
    }
    if (s_live && s_state == IDLE &&
        (s_settle_now || (s_settle_at != 0 && static_cast<long>(GetTickCount() - s_settle_at) >= 0))) {
        Settle();
    }
}
