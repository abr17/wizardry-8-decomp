/* Experimental features (2026-10-20). Not part of the original game.
   Off by default, switched in wiz8wide.ini [Experimental] (and in Options, category
   "Experimental"). Everything they need lives in src/wiz8/experimental/ and this header;
   the call sites elsewhere are marked "Experimental", so a feature can be removed whole. */
#ifndef WIZ8_EXPERIMENTAL_H
#define WIZ8_EXPERIMENTAL_H

/* [Experimental] AnimatedMenu=1: the main menu background is a looped video. Read once and
   cached; the setter writes the ini at once, the menu picks the value up on its next entry. */
bool ExperimentalAnimatedMenuEnabled(void);
void ExperimentalSetAnimatedMenu(bool enabled);

/* Settings of all experimental features (ExperimentalOptions.cpp). IniPath: the full path of
   wiz8wide.ini next to the exe (just "wiz8wide.ini" if the exe path cannot be read; cut to
   size). ReadSwitch: [Experimental] key=0|1 (default 0). WriteSwitch: writes key=0|1. */
void ExperimentalIniPath(char* out, unsigned long size);
bool ExperimentalReadSwitch(const char* key);
void ExperimentalWriteSwitch(const char* key, bool enabled);

/* [Experimental] ModernMap=1: mouse controls on the automap (ModernMap.cpp): the wheel zooms
   towards the cursor, a left-button drag moves the map (hand cursor), a double click does the
   original's single click; a single click does nothing except on the party marker (the
   original's click there); the right click, the keys and the buttons are the original's.
   Read once and cached; the setter writes the ini and applies at once. */
bool ExperimentalModernMapEnabled(void);
void ExperimentalSetModernMap(bool enabled);
/* AutomapScreenFrame, for every input event (InputAtom usEvent, usParam, uiParam, uiTimeStamp)
   before the original handling: true - the event was used (skip the original handling). */
bool ModernMapHandleEvent(unsigned short event, unsigned long param, unsigned long position,
                          unsigned long time);
/* AutomapScreenFrame, once per frame after the events: moves/zooms the camera cheaply while a
   gesture runs, and does the original full camera update when it ends. */
void ModernMapFrame(void);
/* AutomapScreenEnter / Leave: end any gesture; at leave also drop textures made during live
   redraws. */
void ModernMapLeave(void);
/* RenderAutomapFrame: true - this redraw is part of a gesture (cheap texture handling);
   ModernMapNoteLiveRedraw - such a redraw happened. */
bool ModernMapLiveRedraw(void);
void ModernMapNoteLiveRedraw(void);
/* SetAutomapToolCursor: a drag is running (hand cursor). */
bool ModernMapDragging(void);

/* [Experimental] OriginalFixes=1: fixes of the original game's own bugs (OriginalFixes.cpp):
   the region link file left open on an empty .RLK, and the archive open file array grown in
   bytes instead of entries (together the silent crash after ~20 loads of Arnika Road), the
   crash on exit after the window is closed (Alt+F4, the X in window mode), and the radar and
   formation board surfaces leaked on every redraw (in the transplanted RefreshRadarMap /
   RefreshFormationBoard, MGSRadarMap.cpp / MGSFormation.cpp). Read once and cached; the
   setter writes the ini and applies at once. Install: HybridInstall, once; patches the
   original code over its exact bytes and returns a line for wiz8hy.log. */
bool ExperimentalOriginalFixesEnabled(void);
void ExperimentalSetOriginalFixes(bool enabled);
const char* ExperimentalInstallOriginalFixes(void);
/* [Experimental] FrameRateFix=1 (Options: "Alternative Game Tick"): the game runs the same at
   any frame rate (FixedTick.cpp, FrameRateFixes.cpp). Read once and cached; the setter writes
   the ini and applies at once. Install: HybridInstall, once, whatever the switch (each patch
   checks it when it runs): FixedTickInstall and FrameRateFixesInstall, and returns a line for
   wiz8hy.log. */
bool ExperimentalFrameRateFixEnabled(void);
void ExperimentalSetFrameRateFix(bool enabled);
const char* ExperimentalInstallFrameRateFix(void);
/* Part of FrameRateFix (FixedTick.cpp): the party's movement steps at a fixed rate instead of
   once per frame. Install: from ExperimentalInstallFrameRateFix; returns "patched", "FAILED"
   or "SKIPPED". RenderPoint: RenderFrame, before drawing the world, with the world camera's
   location; true - draw from *out (between the last two steps), then put the camera back. */
const char* FixedTickInstall(void);
bool FixedTickRenderPoint(const float* camera, float* out);
/* Part of FrameRateFix (FrameRateFixes.cpp): the rest of the frame-rate-dependent behaviour
   runs as the original does at 30 fps (monster group AI, idle fidgets, flickering lights,
   damage numbers, mouselook smoothing), sound fades last their designed length, and camera
   paths and texture scrolling read a smooth clock.
   Install: from ExperimentalInstallFrameRateFix; returns a status line.
   LogicTicks: the 30-per-second logic ticks of the current frame (0..4, 0 while the game is
   paused; 1 with the switch off). AutomapPanScale: AutomapScreenFrame, once per frame: the
   frame time / (1/30 s) for the arrow-key panning step (1 with the switch off). */
const char* FrameRateFixesInstall(void);
/* CountFrame: RenderFrame, once per rendered frame. FrameCount: the frames counted so far
   (never reset, unlike the fps counter g_fps_frame_count): the logic ticks and the movement
   step key their per-frame work on it. */
void FrameRateCountFrame(void);
unsigned FrameRateFrameCount(void);
int FrameRateLogicTicks(void);
float FrameRateAutomapPanScale(void);

/* Animated main menu (MenuAnimation.cpp). The video is Data\WIDE\MENU_ANIM.AVI: 16-bit ZMBV
   (lossless RGB 5:6:5, AVI), 856x480, looped; narrower canvases show its centre.
   Start: on menu entry, before the background is drawn; true - the animation runs.
   Stop: on menu leave (frees everything, remembers where it was).
   Tick: once per game loop pass while the main menu is the current screen; advances by
   elapsed time and redraws the whole canvas when the frame changes. */
bool MenuAnimationStart(void);
void MenuAnimationStop(void);
void MenuAnimationTick(void);
/* DrawMainMenuItem: while the animation runs, draws the item (slot 0..5, state 0..3; other
   states only restore the background) over the current frame with the baked-in background
   of the original picture removed, and returns true; false - draw it the original way. */
bool MenuAnimationDrawItem(int slot, int state, int top);
/* DrawCatalogSurfaceWide: while the animation runs, the main menu background
   (Data\Options\intro_bg_text.sti) is drawn as the current video frame; true - done. */
bool MenuAnimationDrawBackground(const char* path);

/* Options, category "Experimental" (ExperimentalOptions.cpp). Its entry is a 5th category
   button in the left column with a USB drive in the computer picture that lights up with it
   (the drive alone for other button languages; an invisible click area on the drive's spot
   if the patch's art is missing): a 7th entry of the options menu selection, so it gets
   selection index 6 and the screen's spare m_panel[6]. Its page is panel index 14 (the
   original has 0..13). */
class Controls;
class W8ControlSelection;
class W8OptionsPanel;
struct W8OptionsMenuRow;
enum { W8_EXPERIMENTAL_OPTIONS_SELECTION = 6, W8_EXPERIMENTAL_OPTIONS_PANEL = 14 };
/* CreateControls, first: picks the column art (5 buttons and the drive for the known English
   and Russian button pictures; else the drive alone; else nothing) and points the catalog
   entries of options_base/options_buttons/options_computer at the patch's files. */
void ExperimentalOptionsPrepareArt(void);
/* CreateControls, for each of the 4 category rows: the row to use (moved up for 5 buttons). */
const W8OptionsMenuRow* ExperimentalOptionsRow(int index, const W8OptionsMenuRow* original);
/* CreateControls, after the 8 menu rows and before the column gets its layout anchor. */
void ExperimentalOptionsAddEntry(Controls* controls, W8ControlSelection* selection);
/* SelectPanel: the page of the category (one panel). */
W8OptionsPanel* ExperimentalOptionsCreatePanel(void);
/* W8OptionsPanel constructor: the region set of panel index 14 (the original array has 14). */
unsigned int* ExperimentalOptionsRegionSet(void);

#endif
