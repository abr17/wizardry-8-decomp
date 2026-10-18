/* Wide 2D canvas: settings and coordinate conversion. Not part of the original game
   (added 2026-10-11, see include/wiz8/canvas.h and wiz8-widescreen docs/wide-canvas.md). */
#include "wiz8/canvas.h"

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>

#pragma comment(lib, "winmm.lib") /* timeBeginPeriod */
#include <stdlib.h>
#include <string.h>

/* The game window lives in Video2.cpp. */
extern HWND ghWindow;

int g_canvas_w = W8_GAME_W;
int g_canvas_ox = 0;
int g_canvas_cols = W8_GAME_W / W8_DIRTY_CELL;
int g_canvas_cells = (W8_GAME_W / W8_DIRTY_CELL) * W8_CANVAS_ROWS;

static bool s_initialized;
static bool s_hor_plus = true;
static bool s_mouse_scale = false;
static unsigned int s_frame_buffer_pitch;

/* Layout A of the main screen (see canvas.h): whether it is on and where the
   2D drawing origin on the canvas currently is. */
static bool s_wide_layout;
static int s_draw_origin;

/* Video2.cpp: the whole surface and the whole dirty-cell grid. */
extern void* LockPrimarySurfaceFull(unsigned int* pitch);
extern void UnlockPrimarySurface(void);
extern void InvalidateCanvas(unsigned int flags);

/* Path to a file next to the exe (name - file name only). */
static void PathNextToExe(const char* name, char* out, size_t size)
{
    char module[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, module, sizeof(module));
    if (length == 0 || length >= sizeof(module)) {
        _snprintf(out, size, "%s", name);
        out[size - 1] = '\0';
        return;
    }
    char* slash = strrchr(module, '\\');
    if (slash != 0) {
        slash[1] = '\0';
    } else {
        module[0] = '\0';
    }
    _snprintf(out, size, "%s%s", module, name);
    out[size - 1] = '\0';
}

/* A line to wiz8wide.log and to the debugger (OutputDebugString). */
static void CanvasLog(FILE* log, const char* text)
{
    OutputDebugStringA("wiz8wide: ");
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
    if (log != 0) {
        fprintf(log, "%s\n", text);
    }
}

void CanvasLogLine(const char* text)
{
    char log_path[MAX_PATH];
    PathNextToExe("wiz8wide.log", log_path, sizeof(log_path));
    FILE* log = fopen(log_path, "a");
    CanvasLog(log, text);
    if (log != 0) {
        fclose(log);
    }
}

/* Render output size for auto mode: lines 2 and 3 of 3DVideo.CFG
   (the same file and format that InitializeVideoDevice reads). If there is no file -
   the desktop size. */
static void ReadOutputSize(int* width, int* height, const char** source)
{
    *width = 0;
    *height = 0;
    FILE* config = fopen("3DVideo.CFG", "r");
    if (config != 0) {
        char line[100];
        if (fgets(line, sizeof(line), config) != 0) { /* 1: device name */
            if (fgets(line, sizeof(line), config) != 0) {
                *width = atoi(line);
            }
            if (fgets(line, sizeof(line), config) != 0) {
                *height = atoi(line);
            }
        }
        fclose(config);
        *source = "3DVideo.CFG";
    }
    if (*width <= 0 || *height <= 0) {
        *width = GetSystemMetrics(SM_CXSCREEN);
        *height = GetSystemMetrics(SM_CYSCREEN);
        *source = "desktop";
    }
}

/* Bring any requested width to a valid one: a multiple of 8 (cell grid),
   from 640 to 856. 854 becomes 856. */
static int NormalizeWidth(int width)
{
    width = (width + W8_DIRTY_CELL - 1) / W8_DIRTY_CELL * W8_DIRTY_CELL;
    if (width < W8_GAME_W) {
        width = W8_GAME_W;
    }
    if (width > W8_CANVAS_MAX_W) {
        width = W8_CANVAS_MAX_W;
    }
    return width;
}

/* -1 = not read yet. The launch mode and resolution are read once:
   a change in the settings takes effect from the next launch. */
static int s_display_mode = -1;
static int s_display_w;
static int s_display_h;
/* What DisplayPrepareVideoConfig did - goes to the log from CanvasInitialize (it
   reopens the log). */
static char s_video_config_note[200];

/* Our own string compare and trim: the hybrid has a static UCRT without its own
   initialization (removed-crt-init), its _stricmp/strcspn may crash. */
static bool SameTextAscii(const char* a, const char* b)
{
    for (;; ++a, ++b) {
        char x = *a >= 'A' && *a <= 'Z' ? static_cast<char>(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? static_cast<char>(*b + 32) : *b;
        if (x != y) {
            return false;
        }
        if (x == '\0') {
            return true;
        }
    }
}

static void TrimLineEnd(char* text)
{
    for (; *text != '\0'; ++text) {
        if (*text == '\r' || *text == '\n') {
            *text = '\0';
            return;
        }
    }
}

static int ReadDisplayMode(const char* ini)
{
    char value[32];
    GetPrivateProfileStringA("Display", "Mode", "", value, sizeof(value), ini);
    if (SameTextAscii(value, "window")) {
        return W8_DISPLAY_WINDOW;
    }
    if (SameTextAscii(value, "fullscreen")) {
        return W8_DISPLAY_FULLSCREEN;
    }
    if (SameTextAscii(value, "borderless")) {
        return W8_DISPLAY_BORDERLESS;
    }
    /* Old ini files: only Borderless=0/1. */
    return GetPrivateProfileIntA("Display", "Borderless", 1, ini) != 0 ? W8_DISPLAY_BORDERLESS
                                                                     : W8_DISPLAY_FULLSCREEN;
}

/* Windows desktop resolution. Not GetSystemMetrics: in full-screen
   mode the game switches the monitor, and the metrics return its mode, not the desktop's. */
void DisplayDesktopSize(int* width, int* height)
{
    DEVMODEA mode;
    memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsA(0, ENUM_REGISTRY_SETTINGS, &mode) && mode.dmPelsWidth >= 640 &&
        mode.dmPelsHeight >= 480) {
        *width = static_cast<int>(mode.dmPelsWidth);
        *height = static_cast<int>(mode.dmPelsHeight);
        return;
    }
    *width = GetSystemMetrics(SM_CXSCREEN);
    *height = GetSystemMetrics(SM_CYSCREEN);
}

/* Desktop work area (without the taskbar). In full-screen mode
   SPI_GETWORKAREA is for the game's mode, so the taskbar margins are carried over to
   the desktop size. */
void DisplayDesktopWorkArea(RECT* work)
{
    int desktop_w;
    int desktop_h;
    DisplayDesktopSize(&desktop_w, &desktop_h);
    RECT current;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &current, 0)) {
        SetRect(work, 0, 0, desktop_w, desktop_h);
        return;
    }
    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);
    work->left = current.left;
    work->top = current.top;
    work->right = desktop_w - (screen_w - current.right);
    work->bottom = desktop_h - (screen_h - current.bottom);
}

/* A framed window must fit in the work area: otherwise the bottom of the game goes
   under the taskbar. If it does not fit - shrink it, keeping the aspect ratio. */
static void FitWindowClient(int* width, int* height)
{
    RECT work;
    DisplayDesktopWorkArea(&work);
    RECT frame = {0, 0, 100, 100};
    AdjustWindowRect(&frame, WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX, FALSE);
    const int max_w = (work.right - work.left) - ((frame.right - frame.left) - 100);
    const int max_h = (work.bottom - work.top) - ((frame.bottom - frame.top) - 100);
    if (max_w < 640 || max_h < 480 || (*width <= max_w && *height <= max_h)) {
        return;
    }
    const double scale_w = static_cast<double>(max_w) / *width;
    const double scale_h = static_cast<double>(max_h) / *height;
    const double scale = scale_w < scale_h ? scale_w : scale_h;
    *width = static_cast<int>(*width * scale) & ~1;
    *height = static_cast<int>(*height * scale) & ~1;
}

static void ReadDisplayResolution(const char* ini, int mode, int* width, int* height)
{
    int desktop_w;
    int desktop_h;
    DisplayDesktopSize(&desktop_w, &desktop_h);
    *width = GetPrivateProfileIntA("Display", "ScreenWidth", 0, ini);
    *height = GetPrivateProfileIntA("Display", "ScreenHeight", 0, ini);
    if (mode == W8_DISPLAY_BORDERLESS || *width < 640 || *height < 480) {
        *width = desktop_w;
        *height = desktop_h;
    }
    if (mode == W8_DISPLAY_WINDOW) {
        FitWindowClient(width, height);
    }
}

static void EnsureDisplaySettings(void)
{
    if (s_display_mode >= 0) {
        return;
    }
    char ini[MAX_PATH];
    PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
    s_display_mode = ReadDisplayMode(ini);
    ReadDisplayResolution(ini, s_display_mode, &s_display_w, &s_display_h);
}

bool DisplayBorderlessEnabled(void)
{
    EnsureDisplaySettings();
    return s_display_mode == W8_DISPLAY_BORDERLESS;
}

int DisplayModeSetting(void)
{
    EnsureDisplaySettings();
    return s_display_mode;
}

bool DisplayVideosOnCanvas(void)
{
    /* Bink videos via the 2D canvas (without switching the monitor to 640x480) in every
       mode. Full screen used to play them as the original (exclusive 640x480): with
       dgVoodoo keeping the monitor's own mode (ScalingMode = stretched_ar) that made the
       picture go black and jump from the chosen resolution's shape to 4:3 and back at the
       start and the end of every video. */
    return true;
}

void DisplayResolutionSetting(int* width, int* height)
{
    EnsureDisplaySettings();
    *width = s_display_w;
    *height = s_display_h;
}

void DisplayActivatePendingSettings(void)
{
    char ini[MAX_PATH];
    PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
    s_display_mode = ReadDisplayMode(ini);
    ReadDisplayResolution(ini, s_display_mode, &s_display_w, &s_display_h);
    s_video_config_note[0] = '\0';
    DisplayPrepareVideoConfig();
}

int CanvasWidthForOutput(int width, int height)
{
    char ini[MAX_PATH];
    char value[64];
    PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
    GetPrivateProfileStringA("Canvas", "Width", "auto", value, sizeof(value), ini);
    if (!SameTextAscii(value, "auto") && value[0] != '\0') {
        return NormalizeWidth(atoi(value));
    }
    const double aspect = static_cast<double>(width) / (height > 0 ? height : 1);
    return aspect >= 1.70 ? 856 : (aspect >= 1.55 ? 768 : W8_GAME_W);
}

void DisplayPendingSettings(int* mode, int* width, int* height)
{
    char ini[MAX_PATH];
    PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
    *mode = ReadDisplayMode(ini);
    *width = GetPrivateProfileIntA("Display", "ScreenWidth", 0, ini);
    *height = GetPrivateProfileIntA("Display", "ScreenHeight", 0, ini);
    if (*width < 640 || *height < 480) {
        DisplayDesktopSize(width, height);
    }
    if (*mode == W8_DISPLAY_WINDOW) {
        FitWindowClient(width, height); /* the same as what will be applied later */
    }
}

void DisplayForceMode(int mode)
{
    char ini[MAX_PATH];
    PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
    EnsureDisplaySettings();
    s_display_mode = mode;
    ReadDisplayResolution(ini, mode, &s_display_w, &s_display_h);
}

void DisplaySaveSettings(int mode, int width, int height)
{
    static const char* const names[] = {"borderless", "fullscreen", "window"};
    char ini[MAX_PATH];
    char value[32];
    PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
    if (mode < 0 || mode > 2) {
        mode = W8_DISPLAY_BORDERLESS;
    }
    WritePrivateProfileStringA("Display", "Mode", names[mode], ini);
    /* the old Borderless key is no longer needed (there were no releases with it) */
    WritePrivateProfileStringA("Display", "Borderless", 0, ini);
    _snprintf(value, sizeof(value), "%d", width);
    value[sizeof(value) - 1] = '\0';
    WritePrivateProfileStringA("Display", "ScreenWidth", value, ini);
    _snprintf(value, sizeof(value), "%d", height);
    value[sizeof(value) - 1] = '\0';
    WritePrivateProfileStringA("Display", "ScreenHeight", value, ini);
}

void DisplayPrepareVideoConfig(void)
{
    int width;
    int height;
    DisplayResolutionSetting(&width, &height);
    char device[100] = "";
    char sound[100] = "Miles Fast 2D Positional Audio";
    int old_w = 0;
    int old_h = 0;
    int old_depth = 0;
    FILE* config = fopen("3DVideo.CFG", "r");
    if (config != 0) {
        char line[100];
        if (fgets(device, sizeof(device), config) != 0) {
            TrimLineEnd(device);
        }
        if (fgets(line, sizeof(line), config) != 0) {
            old_w = atoi(line);
        }
        if (fgets(line, sizeof(line), config) != 0) {
            old_h = atoi(line);
        }
        if (fgets(line, sizeof(line), config) != 0) {
            old_depth = atoi(line);
        }
        if (fgets(line, sizeof(line), config) != 0) {
            TrimLineEnd(line);
            if (line[0] != '\0') {
                _snprintf(sound, sizeof(sound), "%s", line);
                sound[sizeof(sound) - 1] = '\0';
            }
        }
        fclose(config);
    }
    if (SameTextAscii(device, "DirectX7") && old_w == width && old_h == height && old_depth == 32) {
        return;
    }
    config = fopen("3DVideo.CFG", "w");
    if (config != 0) {
        fprintf(config, "DirectX7\n%d\n%d\n32\n%s\n", width, height, sound);
        fclose(config);
    }
    _snprintf(s_video_config_note, sizeof(s_video_config_note),
              "[Display] 3DVideo.CFG rewritten: %s %dx%dx%d -> DirectX7 %dx%dx32", device, old_w,
              old_h, old_depth, width, height);
    s_video_config_note[sizeof(s_video_config_note) - 1] = '\0';
}

static int s_single_texture_2d = -1;

bool DisplaySingleTexture2DEnabled(void)
{
    if (s_single_texture_2d < 0) {
        char ini[MAX_PATH];
        PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
        s_single_texture_2d = GetPrivateProfileIntA("Display", "SingleTexture2D", 1, ini) != 0 ? 1 : 0;
    }
    return s_single_texture_2d != 0;
}

bool CanvasWideAutomapEnabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        char ini[MAX_PATH];
        PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
        enabled = GetPrivateProfileIntA("Canvas", "WideAutomap", 1, ini) != 0 ? 1 : 0;
    }
    return enabled != 0;
}

bool CanvasWideWindowsEnabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        char ini[MAX_PATH];
        PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
        enabled = GetPrivateProfileIntA("Canvas", "WideWindows", 1, ini) != 0 ? 1 : 0;
    }
    return enabled != 0;
}

void CanvasCountFrame(void)
{
    static int enabled = -1;
    static DWORD window_start;
    static int frames;
    if (enabled < 0) {
        char ini[MAX_PATH];
        PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
        enabled = GetPrivateProfileIntA("Debug", "LogFps", 0, ini) != 0 ? 1 : 0;
        window_start = GetTickCount();
    }
    if (!enabled) {
        return;
    }
    ++frames;
    DWORD now = GetTickCount();
    DWORD elapsed = now - window_start;
    if (elapsed >= 5000) {
        char text[96];
        _snprintf(text, sizeof(text), "[Perf] %.1f fps (%d frames in %.1f s)",
                  frames * 1000.0 / elapsed, frames, elapsed / 1000.0);
        text[sizeof(text) - 1] = '\0';
        CanvasLogLine(text);
        frames = 0;
        window_start = now;
    }
}

/* -1 = not read yet, 0 = unlimited. */
static int s_max_fps = -1;

int DisplayMaxFps(void)
{
    if (s_max_fps < 0) {
        char ini[MAX_PATH];
        PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
        int value = GetPrivateProfileIntA("Display", "MaxFps", W8_DEFAULT_MAX_FPS, ini);
        if (value < 0) {
            value = 0;
        } else if (value > 0 && value < 30) {
            value = 30; /* below 30 the game visibly stutters, not allowed */
        }
        s_max_fps = value;
    }
    return s_max_fps;
}

/* 0 = not read yet. */
static float s_view_distance;

float DisplayViewDistanceScale(void)
{
    if (s_view_distance <= 0.0f) {
        char ini[MAX_PATH];
        char value[32];
        PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
        GetPrivateProfileStringA("Display", "ViewDistance", W8_DEFAULT_VIEW_DISTANCE, value,
                                 sizeof(value), ini);
        float scale = static_cast<float>(atof(value));
        if (!(scale >= 1.0f)) {
            scale = 1.0f; /* 0, garbage, NaN, < 1 - as in the original */
        }
        if (scale > W8_MAX_VIEW_DISTANCE) {
            scale = W8_MAX_VIEW_DISTANCE;
        }
        s_view_distance = scale;
    }
    return s_view_distance;
}

void DisplaySetViewDistanceScale(float scale)
{
    if (!(scale >= 1.0f)) {
        scale = 1.0f;
    }
    if (scale > W8_MAX_VIEW_DISTANCE) {
        scale = W8_MAX_VIEW_DISTANCE;
    }
    s_view_distance = scale;
    char ini[MAX_PATH];
    char value[32];
    PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
    _snprintf(value, sizeof(value), "%.2f", scale);
    value[sizeof(value) - 1] = '\0';
    WritePrivateProfileStringA("Display", "ViewDistance", value, ini);
}

bool CanvasTakeScreenshotRequest(void)
{
    static DWORD last_poll;
    DWORD now = GetTickCount();
    if (now - last_poll < 200) {
        return false;
    }
    last_poll = now;
    char path[MAX_PATH];
    PathNextToExe("wiz8shot.req", path, sizeof(path));
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    DeleteFileA(path);
    return true;
}

void CanvasLimitFrameRate(void)
{
    static LARGE_INTEGER frequency;
    static LARGE_INTEGER next_frame;
    const int max_fps = DisplayMaxFps();
    if (max_fps <= 0) {
        return;
    }
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
        /* Sleep(1) sleeps ~1 ms, not 15.6 ms (for the whole time the game runs). */
        timeBeginPeriod(1);
    }
    const LONGLONG period = frequency.QuadPart / max_fps;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (next_frame.QuadPart == 0 || now.QuadPart - next_frame.QuadPart > period) {
        /* First frame, or the frame is already late by more than a period (loading,
           Alt+Tab): do not catch up with a burst of fast frames, count from now. */
        next_frame.QuadPart = now.QuadPart + period;
        return;
    }
    /* Sleep in whole milliseconds while more than 2 ms remain until the deadline, the rest -
       by yielding the time slice (Sleep(0)), so as not to overshoot the deadline. */
    for (;;) {
        QueryPerformanceCounter(&now);
        LONGLONG left = next_frame.QuadPart - now.QuadPart;
        if (left <= 0) {
            break;
        }
        if (left * 1000 > 2 * frequency.QuadPart) {
            Sleep(1);
        } else {
            Sleep(0);
        }
    }
    next_frame.QuadPart += period;
}

void CanvasInitialize(void)
{
    if (s_initialized) {
        return;
    }
    s_initialized = true;
    /* Mode and resolution from the settings -> 3DVideo.CFG (it is read both below and by
       InitializeVideoDevice). */
    DisplayPrepareVideoConfig();

    char ini[MAX_PATH];
    char log_path[MAX_PATH];
    char value[64];
    char text[256];
    PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
    PathNextToExe("wiz8wide.log", log_path, sizeof(log_path));
    FILE* log = fopen(log_path, "w");

    GetPrivateProfileStringA("Canvas", "Width", "auto", value, sizeof(value), ini);
    s_hor_plus = GetPrivateProfileIntA("Canvas", "HorPlus", 1, ini) != 0;
    /* -1 = key not set: decided below, once the width is known. */
    int mouse_scale = GetPrivateProfileIntA("Canvas", "MouseScale", -1, ini);

    int out_w;
    int out_h;
    const char* source = "";
    ReadOutputSize(&out_w, &out_h, &source);
    double aspect = static_cast<double>(out_w) / out_h;

    int width;
    if (_stricmp(value, "auto") == 0 || value[0] == '\0') {
        /* Output aspect ratio: 16:9 (1.78) and wider -> 856, 16:10 (1.6) -> 768. */
        width = aspect >= 1.70 ? 856 : (aspect >= 1.55 ? 768 : W8_GAME_W);
    } else {
        width = NormalizeWidth(atoi(value));
    }

    g_canvas_w = width;
    g_canvas_ox = (width - W8_GAME_W) / 2;
    g_canvas_cols = width / W8_DIRTY_CELL;
    g_canvas_cells = g_canvas_cols * W8_CANVAS_ROWS;
    s_draw_origin = g_canvas_ox; /* 2D is drawn into the centered window */
    /* By default mouse scaling is on for a wide canvas and in full screen: there
       dgVoodoo (ScalingMode = stretched_ar) keeps the monitor's own mode and shows the
       picture with black bars, so screen pixels no longer match the game's. In a
       W = 640 window or borderless the mouse works exactly as in the original
       (regression reference). */
    s_mouse_scale = mouse_scale < 0
                        ? (g_canvas_w > W8_GAME_W || DisplayModeSetting() == W8_DISPLAY_FULLSCREEN)
                        : (mouse_scale != 0);

    _snprintf(text, sizeof(text), "ini: %s", ini);
    text[sizeof(text) - 1] = '\0';
    CanvasLog(log, text);
    _snprintf(text, sizeof(text), "output %dx%d (%s), aspect %.3f", out_w, out_h, source, aspect);
    text[sizeof(text) - 1] = '\0';
    CanvasLog(log, text);
    _snprintf(text, sizeof(text),
              "[Canvas] Width=%s -> W=%d, ox=%d, grid %dx%d; HorPlus=%d; MouseScale=%d", value,
              g_canvas_w, g_canvas_ox, g_canvas_cols, W8_CANVAS_ROWS, s_hor_plus ? 1 : 0,
              s_mouse_scale ? 1 : 0);
    text[sizeof(text) - 1] = '\0';
    CanvasLog(log, text);
    {
        static const char* const modes[] = {
            "borderless: popup window at monitor size, no display mode switch",
            "fullscreen: exclusive display mode switch", "window: framed window"};
        int screen_w;
        int screen_h;
        DisplayResolutionSetting(&screen_w, &screen_h);
        _snprintf(text, sizeof(text), "[Display] Mode=%s, %dx%d", modes[DisplayModeSetting()],
                  screen_w, screen_h);
        text[sizeof(text) - 1] = '\0';
        CanvasLog(log, text);
        if (s_video_config_note[0] != '\0') {
            CanvasLog(log, s_video_config_note);
        }
    }
    _snprintf(text, sizeof(text), "[Display] SingleTexture2D=%d (%s)",
              DisplaySingleTexture2DEnabled() ? 1 : 0,
              DisplaySingleTexture2DEnabled() ? "2D layer as one texture, no tile seams"
                                              : "original: 128x128 tiles");
    text[sizeof(text) - 1] = '\0';
    CanvasLog(log, text);
    _snprintf(text, sizeof(text), "[Display] MaxFps=%d (%s)", DisplayMaxFps(),
              DisplayMaxFps() > 0 ? "frame rate capped" : "no cap");
    text[sizeof(text) - 1] = '\0';
    CanvasLog(log, text);
    _snprintf(text, sizeof(text),
              "[Display] ViewDistance=%.2f (render far clip = level far clip x k; gameplay "
              "distances unchanged)",
              DisplayViewDistanceScale());
    text[sizeof(text) - 1] = '\0';
    CanvasLog(log, text);
    if (log != 0) {
        fclose(log);
    }
}

bool CanvasHorPlusEnabled(void)
{
    return s_hor_plus && g_canvas_ox > 0;
}

bool CanvasHorPlusApplies(int left, int right_exclusive)
{
    /* As in our DLL (hooks_view3d.cpp): modes 0, 1, 6 and the menu 0..641.
       Narrow modes 2-5 (between the portrait columns) stay in the 4:3 window. */
    /* In layout A the 3D view rectangle is already widened by ox on both sides
       (SetViewportMode), and the usual linear SetViewport formula itself gives
       Hor+ exactly for this rectangle - no separate widening is needed. */
    return CanvasHorPlusEnabled() && !s_wide_layout && left <= 23 && right_exclusive >= 618 &&
           right_exclusive > left;
}

double CanvasInverseWidth(void)
{
    return 1.0 / g_canvas_w;
}

float CanvasSpriteScaleX(void)
{
    /* At 640 - the same float constant as g_viewport_x_scale (1.0f / 640.0f). */
    return g_canvas_w == W8_GAME_W ? 1.0f / 640.0f : 1.0f / static_cast<float>(g_canvas_w);
}

double CanvasSquareTop(void)
{
    return static_cast<double>(W8_GAME_H) / g_canvas_w;
}

void CanvasSetFrameBufferPitch(unsigned int pitch)
{
    s_frame_buffer_pitch = pitch;
}

unsigned int CanvasFrameBufferPitch(void)
{
    return s_frame_buffer_pitch != 0 ? s_frame_buffer_pitch : 0x500;
}

bool CanvasMouseScaleEnabled(void)
{
    return s_mouse_scale;
}

/* Division rounding down (in C++ `/` rounds toward zero, but for -0.5 pixel
   we need -1, otherwise the left edge is "sticky"). */
static int FloorDiv(long long a, long long b)
{
    long long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return static_cast<int>(q);
}

static int CeilDiv(long long a, long long b)
{
    return -FloorDiv(-a, b);
}

/* Where the canvas is shown in the game window's client area: normally the whole client
   area. In full screen dgVoodoo (ScalingMode = stretched_ar) keeps the monitor's own mode
   and fits the game's mode into it keeping the proportions, with black bars (1920x1080
   on a 2560x1600 monitor: 2560x1440 at y = 80), so the picture is the centered rectangle
   of the mode's aspect ratio. Without the bars the result is the whole client area. */
static bool OutputRect(int* left, int* top, int* width, int* height)
{
    RECT client;
    if (ghWindow == 0 || !GetClientRect(ghWindow, &client) || client.right <= 0 ||
        client.bottom <= 0) {
        return false;
    }
    *left = 0;
    *top = 0;
    *width = client.right;
    *height = client.bottom;
    if (DisplayModeSetting() == W8_DISPLAY_FULLSCREEN) {
        int mode_w;
        int mode_h;
        DisplayResolutionSetting(&mode_w, &mode_h);
        if (mode_w > 0 && mode_h > 0) {
            if (static_cast<long long>(client.right) * mode_h >
                static_cast<long long>(client.bottom) * mode_w) {
                *width = static_cast<int>(static_cast<long long>(client.bottom) * mode_w / mode_h);
                *left = (client.right - *width) / 2; /* bars left and right */
            } else {
                *height = static_cast<int>(static_cast<long long>(client.right) * mode_h / mode_w);
                *top = (client.bottom - *height) / 2; /* bars top and bottom */
            }
        }
    }
    return true;
}

void CanvasScreenToCanvas(int screen_x, int screen_y, int* canvas_x, int* canvas_y)
{
    int out_x;
    int out_y;
    int out_w;
    int out_h;
    if (!s_mouse_scale || !OutputRect(&out_x, &out_y, &out_w, &out_h)) {
        *canvas_x = screen_x;
        *canvas_y = screen_y;
        return;
    }
    *canvas_x = FloorDiv(static_cast<long long>(screen_x - out_x) * g_canvas_w, out_w);
    *canvas_y = FloorDiv(static_cast<long long>(screen_y - out_y) * W8_GAME_H, out_h);
}

/* A screen point inside canvas cell c: its middle if possible (then
   small mouse movements accumulate instead of being lost on "cursor back to center"
   during mouse look), otherwise the left edge of the cell (ceil). */
static int CanvasAxisToScreen(int c, int canvas_extent, int out_extent)
{
    int s = FloorDiv((2LL * c + 1) * out_extent, 2LL * canvas_extent);
    if (FloorDiv(static_cast<long long>(s) * canvas_extent, out_extent) == c) {
        return s;
    }
    return CeilDiv(static_cast<long long>(c) * out_extent, canvas_extent);
}

void CanvasCanvasToScreen(int canvas_x, int canvas_y, int* screen_x, int* screen_y)
{
    int out_x;
    int out_y;
    int out_w;
    int out_h;
    if (!s_mouse_scale || !OutputRect(&out_x, &out_y, &out_w, &out_h)) {
        *screen_x = canvas_x;
        *screen_y = canvas_y;
        return;
    }
    *screen_x = out_x + CanvasAxisToScreen(canvas_x, g_canvas_w, out_w);
    *screen_y = out_y + CanvasAxisToScreen(canvas_y, W8_GAME_H, out_h);
}

bool CanvasMouseMapActive(void)
{
    return g_canvas_ox > 0 || s_mouse_scale;
}

/* ---- Window zone layout (see canvas.h) ---- */

enum { ZONE_MAX = 32, ZONE_FILL_MAX = 16 };
static W8CanvasZone s_zones[ZONE_MAX];
static int s_zone_count;
static W8CanvasFill s_zone_fills[ZONE_FILL_MAX];
static int s_zone_fill_count;
static bool s_zone_active;
static bool s_zone_recomposite; /* recomposite the whole canvas on the next composite */
static bool s_zone_refill;      /* fill the gaps on the next composite */
/* A modal window on top of the layout: the canvas rectangle and "seen this
   frame" (reset on every composite, CanvasZoneOverlay sets it again). */
static bool s_overlay_active;
static bool s_overlay_seen;
static int s_overlay_left, s_overlay_top, s_overlay_right, s_overlay_bottom;

static bool SameZoneLayout(const W8CanvasZone* zones, int zone_count, const W8CanvasFill* fills,
                           int fill_count)
{
    return s_zone_active && zone_count == s_zone_count && fill_count == s_zone_fill_count &&
           memcmp(zones, s_zones, sizeof(W8CanvasZone) * zone_count) == 0 &&
           memcmp(fills, s_zone_fills, sizeof(W8CanvasFill) * fill_count) == 0;
}

void CanvasSetZoneLayout(const W8CanvasZone* zones, int zone_count, const W8CanvasFill* fills,
                         int fill_count)
{
    if (g_canvas_ox <= 0) {
        return;
    }
    zone_count = zone_count > ZONE_MAX ? ZONE_MAX : (zone_count < 0 ? 0 : zone_count);
    fill_count = fill_count > ZONE_FILL_MAX ? ZONE_FILL_MAX : (fill_count < 0 ? 0 : fill_count);
    if (SameZoneLayout(zones, zone_count, fills, fill_count)) {
        return; /* same layout - nothing to recomposite */
    }
    memcpy(s_zones, zones, sizeof(W8CanvasZone) * zone_count);
    memcpy(s_zone_fills, fills, sizeof(W8CanvasFill) * fill_count);
    s_zone_count = zone_count;
    s_zone_fill_count = fill_count;
    s_zone_active = true;
    s_zone_recomposite = true;
    s_zone_refill = true;
    s_overlay_active = false;
}

void CanvasClearZoneLayout(void)
{
    if (!s_zone_active) {
        return;
    }
    s_zone_active = false;
    s_zone_count = 0;
    s_zone_fill_count = 0;
    s_overlay_active = false;
    s_zone_recomposite = true; /* the canvas is 1:1 again */
}

bool CanvasZoneLayoutActive(void)
{
    return s_zone_active;
}

int CanvasZoneMapX(int game_x, int game_y)
{
    if (s_zone_active) {
        if (s_overlay_active) {
            int cx = game_x + g_canvas_ox;
            if (cx >= s_overlay_left && cx < s_overlay_right && game_y >= s_overlay_top &&
                game_y < s_overlay_bottom) {
                return cx;
            }
        }
        for (int index = 0; index < s_zone_count; ++index) {
            const W8CanvasZone& zone = s_zones[index];
            if (zone.fill_right == 0 && game_x >= zone.left && game_x < zone.right &&
                game_y >= zone.top && game_y < zone.bottom) {
                return game_x - zone.left + zone.dst_left;
            }
        }
    }
    return game_x + g_canvas_ox;
}

int CanvasZoneUnmapX(int canvas_x, int canvas_y)
{
    if (!s_zone_active) {
        return canvas_x - g_canvas_ox;
    }
    if (s_overlay_active && canvas_x >= s_overlay_left && canvas_x < s_overlay_right &&
        canvas_y >= s_overlay_top && canvas_y < s_overlay_bottom) {
        return canvas_x - g_canvas_ox;
    }
    int best = canvas_x - g_canvas_ox;
    int best_distance = 0x7fffffff;
    for (int index = 0; index < s_zone_count; ++index) {
        const W8CanvasZone& zone = s_zones[index];
        if (canvas_y < zone.top || canvas_y >= zone.bottom) {
            continue;
        }
        int dst_right = zone.fill_right != 0 ? zone.fill_right
                                             : zone.dst_left + (zone.right - zone.left);
        if (canvas_x >= zone.dst_left && canvas_x < dst_right) {
            if (zone.fill_right != 0) {
                /* A stretched strip is background: the game point inside the source strip. */
                return zone.left + (canvas_x - zone.dst_left) % (zone.right - zone.left);
            }
            return canvas_x - zone.dst_left + zone.left;
        }
        /* A gap (marble): the edge of the nearest zone in this row. */
        int distance = canvas_x < zone.dst_left ? zone.dst_left - canvas_x
                                                : canvas_x - (dst_right - 1);
        if (distance < best_distance) {
            best_distance = distance;
            best = canvas_x < zone.dst_left ? zone.left
                                            : (zone.fill_right != 0 ? zone.right - 1
                                                                    : zone.right - 1);
        }
    }
    return best;
}

/* Modal windows of this frame: the union of their rectangles (canvas). */
static int s_pending_left, s_pending_top, s_pending_right, s_pending_bottom;

void CanvasZoneOverlay(int left, int top, int right, int bottom)
{
    if (!s_zone_active || right <= left || bottom <= top) {
        return;
    }
    left += g_canvas_ox;
    right += g_canvas_ox;
    if (!s_overlay_seen) {
        s_pending_left = left;
        s_pending_top = top;
        s_pending_right = right;
        s_pending_bottom = bottom;
        s_overlay_seen = true;
        return;
    }
    s_pending_left = left < s_pending_left ? left : s_pending_left;
    s_pending_top = top < s_pending_top ? top : s_pending_top;
    s_pending_right = right > s_pending_right ? right : s_pending_right;
    s_pending_bottom = bottom > s_pending_bottom ? bottom : s_pending_bottom;
}

int CanvasZoneCount(void)
{
    return s_zone_active ? s_zone_count : 0;
}

const W8CanvasZone* CanvasZoneAt(int index)
{
    return &s_zones[index];
}

int CanvasZoneFillCount(void)
{
    return s_zone_active ? s_zone_fill_count : 0;
}

const W8CanvasFill* CanvasZoneFillAt(int index)
{
    return &s_zone_fills[index];
}

bool CanvasZoneOverlayRect(int* left, int* top, int* right, int* bottom)
{
    if (!s_zone_active || !s_overlay_active) {
        return false;
    }
    *left = s_overlay_left;
    *top = s_overlay_top;
    *right = s_overlay_right;
    *bottom = s_overlay_bottom;
    return true;
}

void CanvasZoneBeginFlush(bool* recomposite_all, bool* refill)
{
    if (s_overlay_seen) {
        /* A modal window this frame: if it is new or has moved - centered
           on top of the layout, recomposite the rest. */
        if (!s_overlay_active || s_pending_left != s_overlay_left ||
            s_pending_top != s_overlay_top || s_pending_right != s_overlay_right ||
            s_pending_bottom != s_overlay_bottom) {
            s_overlay_left = s_pending_left;
            s_overlay_top = s_pending_top;
            s_overlay_right = s_pending_right;
            s_overlay_bottom = s_pending_bottom;
            s_overlay_active = true;
            s_zone_recomposite = true;
        }
    } else if (s_overlay_active) {
        /* The modal window closed: its area is composited from zones again. */
        s_overlay_active = false;
        s_zone_recomposite = true;
    }
    s_overlay_seen = false;
    *recomposite_all = s_zone_recomposite;
    *refill = s_zone_refill;
    s_zone_recomposite = false;
    s_zone_refill = false;
}

bool CanvasIsInSideBar(int canvas_x)
{
    /* In layout A there are no "side bars": the portrait columns and panels are there.
       Nor in the window zone layout: the whole canvas is window parts or marble. */
    return g_canvas_ox > 0 && !s_wide_layout && !s_zone_active &&
           (canvas_x < g_canvas_ox || canvas_x >= g_canvas_ox + W8_GAME_W);
}

void CanvasToGame(int canvas_x, int canvas_y, int max_x, int max_y, int* game_x, int* game_y)
{
    int x = s_zone_active ? CanvasZoneUnmapX(canvas_x, canvas_y) : canvas_x - g_canvas_ox;
    int y = canvas_y;
    if (s_wide_layout) {
        /* Layout A: the game needs the whole canvas, -ox .. 639+ox. A negative
           number in the game's 16-bit field turns into 65535-..., but all
           region hit tests read it as signed short. */
        x = x < -g_canvas_ox ? -g_canvas_ox
                             : (x > W8_GAME_W - 1 + g_canvas_ox ? W8_GAME_W - 1 + g_canvas_ox : x);
        y = y < 0 ? 0 : (y > W8_GAME_H - 1 ? W8_GAME_H - 1 : y);
        *game_x = x;
        *game_y = y;
        return;
    }
    if (CanvasMouseMapActive()) {
        /* A click in a side bar gives the nearest point on the edge of the 640x480 window:
           the game stores coordinates in 16 unsigned bits, a negative number
           would turn into 65535. The limit is always 639/479 (the last pixel
           of the window), so that polling (SGPMouseGetPos) and events give the same result. */
        if (max_x > W8_GAME_W - 1) {
            max_x = W8_GAME_W - 1;
        }
        if (max_y > W8_GAME_H - 1) {
            max_y = W8_GAME_H - 1;
        }
        x = x < 0 ? 0 : (x > max_x ? max_x : x);
        y = y < 0 ? 0 : (y > max_y ? max_y : y);
    }
    *game_x = x;
    *game_y = y;
}

void CanvasScreenToGame(int screen_x, int screen_y, int* game_x, int* game_y, bool* in_side_bar)
{
    int canvas_x;
    int canvas_y;
    CanvasScreenToCanvas(screen_x, screen_y, &canvas_x, &canvas_y);
    CanvasToGame(canvas_x, canvas_y, W8_GAME_W - 1, W8_GAME_H - 1, game_x, game_y);
    if (in_side_bar != 0) {
        *in_side_bar = CanvasIsInSideBar(canvas_x);
    }
}

/* ---- Layout A of the main screen (see canvas.h) ---- */

/* Fill both side bars of the 2D surface with zeros (black, transparent):
   x < ox and x >= ox + 640. Centered screens do not draw there and assume
   it is empty. */
static void ClearSurfaceSideBars(void)
{
    if (g_canvas_ox <= 0) {
        return;
    }
    unsigned int pitch = 0;
    unsigned char* surface = static_cast<unsigned char*>(LockPrimarySurfaceFull(&pitch));
    if (surface == 0) {
        return;
    }
    size_t bar_bytes = static_cast<size_t>(g_canvas_ox) * 2;
    size_t right_offset = static_cast<size_t>(g_canvas_ox + W8_GAME_W) * 2;
    for (int row = 0; row < W8_GAME_H; ++row) {
        unsigned char* line = surface + static_cast<size_t>(row) * pitch;
        memset(line, 0, bar_bytes);
        memset(line + right_offset, 0, bar_bytes);
    }
    UnlockPrimarySurface();
    CanvasNoteSideBarsCleared();
}

static unsigned int s_side_bars_clear_count;

void CanvasNoteSideBarsCleared(void)
{
    ++s_side_bars_clear_count;
}

unsigned int CanvasSideBarsClearCount(void)
{
    return s_side_bars_clear_count;
}

void CanvasClearSideBars(void)
{
    ClearSurfaceSideBars();
}

void CanvasSetWideLayout(bool active)
{
    active = active && g_canvas_ox > 0;
    if (active == s_wide_layout) {
        return;
    }
    s_wide_layout = active;
    s_draw_origin = g_canvas_ox;
    if (!active) {
        /* Leaving the main screen: wipe the HUD leftovers in the bars. */
        ClearSurfaceSideBars();
    }
    /* Either way: re-upload the whole canvas to the texture. */
    InvalidateCanvas(0);
}

bool CanvasWideLayoutActive(void)
{
    return s_wide_layout;
}

int CanvasAnchorShift(int anchor)
{
    if (!s_wide_layout) {
        return 0;
    }
    switch (anchor) {
    case W8_ANCHOR_LEFT:
        return -g_canvas_ox;
    case W8_ANCHOR_RIGHT:
        return g_canvas_ox;
    default:
        return 0;
    }
}

int CanvasDrawOrigin(void)
{
    return s_draw_origin;
}

void CanvasSetDrawOrigin(int canvas_x)
{
    s_draw_origin = canvas_x;
}

CanvasDrawScope::CanvasDrawScope(int anchor) : saved_origin(s_draw_origin)
{
    s_draw_origin = g_canvas_ox + CanvasAnchorShift(anchor);
}

CanvasDrawScope::~CanvasDrawScope()
{
    s_draw_origin = saved_origin;
}

CanvasOriginScope::CanvasOriginScope(int canvas_origin) : saved_origin(s_draw_origin)
{
    s_draw_origin = canvas_origin;
}

CanvasOriginScope::~CanvasOriginScope()
{
    s_draw_origin = saved_origin;
}
