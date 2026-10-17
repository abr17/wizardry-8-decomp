/* Modified for the Wizardry 8 reconstruction: 2026-10-03, 2026-10-04, 2026-10-06, 2026-10-07.
   Distributed under the accompanying SFI Source Code license agreement. */
#include "Types.h"
#include <windows.h>
#include <zmouse.h>
#include <stdio.h>
#include <memory.h>
#include "DEBUG.H"
#include "input.h"
#include "MemMan.h"
#include "english.h"
#include "Video2.h"
#include "Font.h" // gfCyrillicText (mod: Russian translation)

// Make sure to refer to the translation table which is within one of the following files (depending
// on the language used). ENGLISH.C, JAPANESE.C, FRENCH.C, GERMAN.C, SPANISH.C, etc...

#include "sgp.h"

#undef GetCursorPos
#define GetCursorPos SGPMouseGetPos

// The gfKeyState table is used to track which of the keys is up or down at any one time. This is used while polling
// the interface.

// GLOBAL: WIZ8 0x006f0520
BOOLEAN gfKeyState[256]; // TRUE = Pressed, FALSE = Not Pressed
// GLOBAL: WIZ8 0x00650db8
BOOLEAN fCursorWasClipped = FALSE;
RECT gCursorClipRect;

// The gsKeyTranslationTables basically translates scan codes to our own key value table. Please note that the table is 2 bytes
// wide per entry. This will be used since we will use 2 byte characters for translation purposes.

// GLOBAL: WIZ8 0x006f04ea
UINT16 gfShiftState; // TRUE = Pressed, FALSE = Not Pressed
// GLOBAL: WIZ8 0x006f051c
UINT16 gfAltState; // TRUE = Pressed, FALSE = Not Pressed
// GLOBAL: WIZ8 0x006f0508
UINT16 gfCtrlState; // TRUE = Pressed, FALSE = Not Pressed

// These data structure are used to track the mouse while polling

// GLOBAL: WIZ8 0x006f04f4
BOOLEAN gfTrackDblClick;
// GLOBAL: WIZ8 0x006f04e4
UINT32 guiDoubleClkDelay; // Current delay in milliseconds for a delay
// GLOBAL: WIZ8 0x006f0504
UINT32 guiSingleClickTimer;
UINT32 guiRecordedWParam;
UINT32 guiRecordedLParam;
// GLOBAL: WIZ8 0x006f0514
UINT16 gusRecordedKeyState;
// GLOBAL: WIZ8 0x006f04e9
BOOLEAN gfRecordedLeftButtonUp;

// GLOBAL: WIZ8 0x006f0518
UINT32 guiLeftButtonRepeatTimer;
// GLOBAL: WIZ8 0x006f04f0
UINT32 guiRightButtonRepeatTimer;

// GLOBAL: WIZ8 0x006f04ec
BOOLEAN gfTrackMousePos; // TRUE = queue mouse movement events, FALSE = don't
// GLOBAL: WIZ8 0x006f04ed
BOOLEAN gfLeftButtonState; // TRUE = Pressed, FALSE = Not Pressed
// GLOBAL: WIZ8 0x006f04e8
BOOLEAN gfRightButtonState; // TRUE = Pressed, FALSE = Not Pressed
// GLOBAL: WIZ8 0x006f050a
UINT16 gusMouseXPos; // X position of the mouse on screen
// GLOBAL: WIZ8 0x006f04f8
UINT16 gusMouseYPos; // y position of the mouse on screen

// The queue structures are used to track input events using queued events

// GLOBAL: WIZ8 0x006ef4e0
InputAtom gEventQueue[256];
// GLOBAL: WIZ8 0x006f04f6
UINT16 gusQueueCount;
// GLOBAL: WIZ8 0x006f04e2
UINT16 gusHeadIndex;
// GLOBAL: WIZ8 0x006f04e0
UINT16 gusTailIndex;

// ATE: Added to signal if we have had input this frame - cleared by the SGP main loop
// GLOBAL: WIZ8 0x00650db9
BOOLEAN gfSGPInputReceived = FALSE;

// This is the WIN95 hook specific data and defines used to handle the keyboard and
// mouse hook

// GLOBAL: WIZ8 0x006f04fc
HHOOK ghKeyboardHook;
// GLOBAL: WIZ8 0x006f050c
HHOOK ghMouseHook;

// If the following pointer is non NULL then input characters are redirected to
// the related string

// GLOBAL: WIZ8 0x006f0500
BOOLEAN gfCurrentStringInputState;
// GLOBAL: WIZ8 0x006f0510
StringInput* gpCurrentStringDescriptor;

// Local function headers

void QueueEvent(UINT16 ubInputEvent, UINT32 usParam, UINT32 uiParam);
void RedirectToString(UINT16 uiInputCharacter);
void HandleSingleClicksAndButtonRepeats(void);
void AdjustMouseForWindowOrigin(void);

/* Mod (Russian translation "Wizardry8_rus 1.2.6"): a second layout for text input
   (character names, save names, words in NPC conversations). In the translation's exe
   gsKeyTranslationTable became an array of two layouts of 512 entries each
   ([layout][256 without Shift + 256 with Shift]), the layout number is a global
   variable (0x6511b8 there), switching is Alt+Shift (releasing Shift while
   Alt is held). Layout 1 is Russian ЙЦУКЕН (JCUKEN) in cp1251 codes. Here it
   is built from the English table plus the list of differences below (taken from its exe).
   Active only with gfCyrillicText; otherwise always layout 0, as in GOG. */
static UINT16 gsRussianKeyTranslationTable[512];
static BOOLEAN gfRussianKeyTableReady = FALSE;
static UINT16 gusKeyboardLayout = 0; // 0 - Latin, 1 - Cyrillic

typedef struct {
    UINT8 ubKey;      // virtual key code
    UINT16 usNormal;  // character without Shift (0 - leave unchanged)
    UINT16 usShifted; // character with Shift (0 - leave unchanged)
} RussianKeyOverride;

static const RussianKeyOverride gRussianKeyOverrides[] = {
    {0x32, 0, 0x22},       // 2   -> Shift: "
    {0x33, 0, 0xB9},       // 3   -> Shift: №
    {0x34, 0, 0x3B},       // 4   -> Shift: ;
    {0x36, 0, 0x3A},       // 6   -> Shift: :
    {0x37, 0, 0x3F},       // 7   -> Shift: ?
    {0x41, 0xF4, 0xD4},    // A   -> ф Ф
    {0x42, 0xE8, 0xC8},    // B   -> и И
    {0x43, 0xF1, 0xD1},    // C   -> с С
    {0x44, 0xE2, 0xC2},    // D   -> в В
    {0x45, 0xF3, 0xD3},    // E   -> у У
    {0x46, 0xE0, 0xC0},    // F   -> а А
    {0x47, 0xEF, 0xCF},    // G   -> п П
    {0x48, 0xF0, 0xD0},    // H   -> р Р
    {0x49, 0xF8, 0xD8},    // I   -> ш Ш
    {0x4A, 0xEE, 0xCE},    // J   -> о О
    {0x4B, 0xEB, 0xCB},    // K   -> л Л
    {0x4C, 0xE4, 0xC4},    // L   -> д Д
    {0x4D, 0xFC, 0xDC},    // M   -> ь Ь
    {0x4E, 0xF2, 0xD2},    // N   -> т Т
    {0x4F, 0xF9, 0xD9},    // O   -> щ Щ
    {0x50, 0xE7, 0xC7},    // P   -> з З
    {0x51, 0xE9, 0xC9},    // Q   -> й Й
    {0x52, 0xEA, 0xCA},    // R   -> к К
    {0x53, 0xFB, 0xDB},    // S   -> ы Ы
    {0x54, 0xE5, 0xC5},    // T   -> е Е
    {0x55, 0xE3, 0xC3},    // U   -> г Г
    {0x56, 0xEC, 0xCC},    // V   -> м М
    {0x57, 0xF6, 0xD6},    // W   -> ц Ц
    {0x58, 0xF7, 0xD7},    // X   -> ч Ч
    {0x59, 0xED, 0xCD},    // Y   -> н Н
    {0x5A, 0xFF, 0xDF},    // Z   -> я Я
    {0xBA, 0xE6, 0xC6},    // ;   -> ж Ж
    {0xBC, 0xE1, 0xC1},    // ,   -> б Б
    {0xBE, 0xFE, 0xDE},    // .   -> ю Ю
    {0xBF, 0x2E, 0x2C},    // /   -> . ,
    {0xC0, 0xB8, 0xA8},    // `   -> ё Ё
    {0xDB, 0xF5, 0xD5},    // [   -> х Х
    {0xDC, 0, 0x2F},       // \   -> Shift: /
    {0xDD, 0xFA, 0xDA},    // ]   -> ъ Ъ
    {0xDE, 0xFD, 0xDD},    // '   -> э Э
};

static UINT16* ActiveKeyTranslationTable(void)
{
    UINT32 i;

    if (!gfCyrillicText || gusKeyboardLayout == 0) {
        return gsKeyTranslationTable;
    }
    if (!gfRussianKeyTableReady) {
        memcpy(gsRussianKeyTranslationTable, gsKeyTranslationTable,
               sizeof(gsRussianKeyTranslationTable));
        for (i = 0; i < sizeof(gRussianKeyOverrides) / sizeof(gRussianKeyOverrides[0]); i++) {
            const RussianKeyOverride* o = &gRussianKeyOverrides[i];
            if (o->usNormal != 0) {
                gsRussianKeyTranslationTable[o->ubKey] = o->usNormal;
            }
            if (o->usShifted != 0) {
                gsRussianKeyTranslationTable[o->ubKey + 256] = o->usShifted;
            }
        }
        gfRussianKeyTableReady = TRUE;
    }
    return gsRussianKeyTranslationTable;
}

// These are the hook functions for both keyboard and mouse

// FUNCTION: WIZ8 0x00401b30
LRESULT CALLBACK KeyboardHandler(int Code, WPARAM wParam, LPARAM lParam)
{
    if ((Code < 0) ||
        (!gfApplicationActive)) { // Do not handle this message, pass it on to another window
        return CallNextHookEx(ghKeyboardHook, Code, wParam, lParam);
    }

    if (lParam & TRANSITION_MASK) { // The key has been released
        KeyUp(wParam, lParam);
        //gfSGPInputReceived =  TRUE;
    } else { // Key was up
        KeyDown(wParam, lParam);
        gfSGPInputReceived = TRUE;
    }

    return TRUE;
}

// Wizardry mouse hander

// FUNCTION: WIZ8 0x00401c70
LRESULT CALLBACK MouseHandler(int Code, WPARAM wParam, LPARAM lParam)
{
    UINT32 uiParam;
    UINT32 uiXPos, uiYPos;
    RECT rcClient;
    BOOLEAN fOutsideClient = FALSE;
    /* Wide canvas: cursor in a side bar; presses there are dropped (below). */
    bool fInSideBar = false;
    static BOOLEAN sfLeftDownDropped = FALSE;
    static BOOLEAN sfRightDownDropped = FALSE;
    // GLOBAL: WIZ8 0x00650dba
    static BOOLEAN fResizing = FALSE;
    LRESULT Result;

    uiXPos = (((MOUSEHOOKSTRUCT*)lParam)->pt).x;
    uiYPos = (((MOUSEHOOKSTRUCT*)lParam)->pt).y;

    if (!VideoIsFullScreen()) {
        if (wParam == WM_NCLBUTTONDOWN)
            fResizing = TRUE;

        VideoGetClientRect(&rcClient);
        if ((uiXPos < (UINT32)rcClient.left) || (uiXPos > (UINT32)rcClient.right) ||
            (uiYPos < (UINT32)rcClient.top) || (uiYPos > (UINT32)rcClient.bottom))
            fOutsideClient = TRUE;
    }

    if ((Code < 0) || (!gfApplicationActive) || fOutsideClient ||
        fResizing) { // Do not handle this message, pass it on to another window
        Result = CallNextHookEx(ghMouseHook, Code, wParam, lParam);

        if ((wParam == WM_LBUTTONUP) || (wParam == WM_NCLBUTTONUP))
            fResizing = FALSE;

        return (Result);
    }

    switch (wParam) {
    case WM_LBUTTONUP:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEMOVE:
        /* Wide canvas: screen/client pixels -> canvas pixels -> game space
           (minus the window offset ox, clamped to 640x480). See wiz8/canvas.h. */
        {
            int screen_x;
            int screen_y;
            int game_x;
            int game_y;
            if (VideoIsFullScreen()) {
                screen_x = (int)uiXPos;
                screen_y = (int)uiYPos;
            } else {
                screen_x = (int)(uiXPos - rcClient.left);
                screen_y = (int)(uiYPos - rcClient.top);
            }
            CanvasScreenToGame(screen_x, screen_y, &game_x, &game_y, &fInSideBar);
            gusMouseXPos = (UINT16)(game_x);
            gusMouseYPos = (UINT16)(game_y);
        }
        uiParam = (UINT32)gusMouseYPos << 16 | (UINT32)gusMouseXPos;
        //Set that we have input
        gfSGPInputReceived = TRUE;
        break;
    }

    if (wParam == WM_MOUSEWHEEL) {
        return (FALSE);
    }

    /* Wide canvas: a button pressed in a side bar (outside the 640x480 window)
       is ignored, otherwise it would land on the nearest edge control (e.g. a
       portrait). Its matching release is ignored too, wherever it happens, so
       no screen sees an "up" without a "down". A press inside the window and
       release in a bar still arrives (clamped), as before, so drags end. */
    switch (wParam) {
    case WM_LBUTTONDOWN:
        sfLeftDownDropped = fInSideBar;
        if (fInSideBar)
            return TRUE;
        break;
    case WM_RBUTTONDOWN:
        sfRightDownDropped = fInSideBar;
        if (fInSideBar)
            return TRUE;
        break;
    case WM_LBUTTONUP:
        if (sfLeftDownDropped) {
            sfLeftDownDropped = FALSE;
            return TRUE;
        }
        break;
    case WM_RBUTTONUP:
        if (sfRightDownDropped) {
            sfRightDownDropped = FALSE;
            return TRUE;
        }
        break;
    }

    switch (wParam) {
    case WM_LBUTTONDOWN:
        gfLeftButtonState = TRUE;
        QueueEvent(LEFT_BUTTON_DOWN, 0, uiParam);
        break;
    case WM_LBUTTONUP:
        gfLeftButtonState = FALSE;
        QueueEvent(LEFT_BUTTON_UP, 0, uiParam);
        break;
    case WM_RBUTTONDOWN:
        gfRightButtonState = TRUE;
        QueueEvent(RIGHT_BUTTON_DOWN, 0, uiParam);
        break;
    case WM_RBUTTONUP:
        gfRightButtonState = FALSE;
        QueueEvent(RIGHT_BUTTON_UP, 0, uiParam);
        break;
    case WM_MOUSEMOVE:
        if (gfTrackMousePos)
            QueueEvent(MOUSE_POS, 0, uiParam);
        break;
    }

    return (TRUE);
}

// FUNCTION: WIZ8 0x00401ea0
BOOLEAN InitializeInputManager(void)
{
    // Link to debugger
    RegisterDebugTopic(TOPIC_INPUT, "Input Manager");
    // Initialize the gfKeyState table to FALSE everywhere
    memset(gfKeyState, FALSE, 256);
    // Initialize the Event Queue
    gusQueueCount = 0;
    gusHeadIndex = 0;
    gusTailIndex = 0;
    // By default, we will not queue mousemove events
    gfTrackMousePos = FALSE;
    // Initialize other variables
    gfShiftState = FALSE;
    gfAltState = FALSE;
    gfCtrlState = FALSE;
    // Initialize variables pertaining to DOUBLE CLIK stuff
    gfTrackDblClick = TRUE;
    guiDoubleClkDelay = DBL_CLK_TIME;
    guiSingleClickTimer = 0;
    gfRecordedLeftButtonUp = FALSE;
    // Initialize variables pertaining to the button states
    gfLeftButtonState = FALSE;
    gfRightButtonState = FALSE;
    // Initialize variables pertaining to the repeat mechanism
    guiLeftButtonRepeatTimer = 0;
    guiRightButtonRepeatTimer = 0;
    // Set the mouse to the center of the screen
    gusMouseXPos = 320;
    gusMouseYPos = 240;
    // Initialize the string input mechanism
    gfCurrentStringInputState = FALSE;
    gpCurrentStringDescriptor = NULL;
    // Activate the hook functions for both keyboard and Mouse
    ghKeyboardHook = SetWindowsHookEx(WH_KEYBOARD, (HOOKPROC)KeyboardHandler, (HINSTANCE)0,
                                      GetCurrentThreadId());
    DbgMessage(TOPIC_INPUT, DBG_LEVEL_2, String("Set keyboard hook returned %d", ghKeyboardHook));

    ghMouseHook =
        SetWindowsHookEx(WH_MOUSE, (HOOKPROC)MouseHandler, (HINSTANCE)0, GetCurrentThreadId());
    DbgMessage(TOPIC_INPUT, DBG_LEVEL_2, String("Set mouse hook returned %d", ghMouseHook));
    return TRUE;
}

// FUNCTION: WIZ8 0x00401f70
void ShutdownInputManager(void)
{ // There's very little to do when shutting down the input manager. In the future, this is where the keyboard and
    // mouse hooks will be destroyed
    UnRegisterDebugTopic(TOPIC_INPUT, "Input Manager");
    UnhookWindowsHookEx(ghKeyboardHook);
    UnhookWindowsHookEx(ghMouseHook);
}

// FUNCTION: WIZ8 0x00401f90
void QueueEvent(UINT16 ubInputEvent, UINT32 usParam, UINT32 uiParam)
{
    UINT32 uiTimer;
    UINT16 usKeyState;

    uiTimer = GetTickCount();
    usKeyState = gfShiftState | gfCtrlState | gfAltState;

    // Can we queue up one more event, if not, the event is lost forever
    if (gusQueueCount == 256) { // No more queue space
        return;
    }

    if (ubInputEvent == LEFT_BUTTON_DOWN) {
        guiLeftButtonRepeatTimer = uiTimer + BUTTON_REPEAT_TIMEOUT;
    }

    if (ubInputEvent == RIGHT_BUTTON_DOWN) {
        guiRightButtonRepeatTimer = uiTimer + BUTTON_REPEAT_TIMEOUT;
    }

    if (ubInputEvent == LEFT_BUTTON_UP) {
        guiLeftButtonRepeatTimer = 0;
    }

    if (ubInputEvent == RIGHT_BUTTON_UP) {
        guiRightButtonRepeatTimer = 0;
    }

    if ((ubInputEvent == LEFT_BUTTON_UP)) {
        // Do we have a double click
        if ((uiTimer - guiSingleClickTimer) < DBL_CLK_TIME) {
            guiSingleClickTimer = 0;

            // Add a button up first...
            gEventQueue[gusTailIndex].uiTimeStamp = uiTimer;
            gEventQueue[gusTailIndex].usKeyState = gusRecordedKeyState;
            gEventQueue[gusTailIndex].usEvent = LEFT_BUTTON_UP;
            gEventQueue[gusTailIndex].usParam = usParam;
            gEventQueue[gusTailIndex].uiParam = uiParam;

            // Increment the number of items on the input queue
            gusQueueCount++;

            // Increment the gusTailIndex pointer
            if (gusTailIndex == 255) { // The gusTailIndex is about to wrap around the queue ring
                gusTailIndex = 0;
            } else { // We simply increment the gusTailIndex
                gusTailIndex++;
            }

            // Now do double click
            gEventQueue[gusTailIndex].uiTimeStamp = uiTimer;
            gEventQueue[gusTailIndex].usKeyState = gusRecordedKeyState;
            gEventQueue[gusTailIndex].usEvent = LEFT_BUTTON_DBL_CLK;
            gEventQueue[gusTailIndex].usParam = usParam;
            gEventQueue[gusTailIndex].uiParam = uiParam;

            // Increment the number of items on the input queue
            gusQueueCount++;

            // Increment the gusTailIndex pointer
            if (gusTailIndex == 255) { // The gusTailIndex is about to wrap around the queue ring
                gusTailIndex = 0;
            } else { // We simply increment the gusTailIndex
                gusTailIndex++;
            }

            return;
        } else {
            // Save time
            guiSingleClickTimer = uiTimer;
        }
    }

    // Okey Dokey, we can queue up the event, so we do it
    gEventQueue[gusTailIndex].uiTimeStamp = uiTimer;
    gEventQueue[gusTailIndex].usKeyState = usKeyState;
    gEventQueue[gusTailIndex].usEvent = ubInputEvent;
    gEventQueue[gusTailIndex].usParam = usParam;
    gEventQueue[gusTailIndex].uiParam = uiParam;

    // Increment the number of items on the input queue
    gusQueueCount++;

    // Increment the gusTailIndex pointer
    if (gusTailIndex == 255) { // The gusTailIndex is about to wrap around the queue ring
        gusTailIndex = 0;
    } else { // We simply increment the gusTailIndex
        gusTailIndex++;
    }
}

// FUNCTION: WIZ8 0x00402140
BOOLEAN DequeueEvent(InputAtom* Event)
{
    HandleSingleClicksAndButtonRepeats();

    // Is there an event to dequeue
    if (gusQueueCount > 0) {
        // We have an event, so we dequeue it
        memcpy(Event, &(gEventQueue[gusHeadIndex]), sizeof(InputAtom));

        if (gusHeadIndex == 255) {
            gusHeadIndex = 0;
        } else {
            gusHeadIndex++;
        }

        // Decrement the number of items on the input queue
        gusQueueCount--;

        // dequeued an event, return TRUE
        return TRUE;
    } else {
        // No events to dequeue, return FALSE
        return FALSE;
    }
}

// GLOBAL: WIZ8 0x005ff51c
unsigned short g_key_remap_5ff51c[14] = {0x0069, 0x0063, 0x0061, 0x0067, 0x0064, 0x0068, 0x0066,
                                         0x0062, 0x0000, 0x0000, 0x0000, 0x0000, 0x0060, 0x006e};

// FUNCTION: WIZ8 0x00402270
void KeyChange(UINT32 key, UINT32 flags, UINT8 pressed)
{
    POINT point;
    unsigned int packed;
    unsigned int code;

    if (key == 0x0c) {
        key = 0x65;
    } else if (key < 0x2f && key > 0x20 && (flags & 0x1000000) == 0) {
        key = g_key_remap_5ff51c[key - 0x21];
    } else if (key == 0x0d && (flags & 0x1000000) != 0) {
        key = 0x6c;
    }
    SGPMouseGetPos(&point);
    packed = ((unsigned int)point.y << 0x10) | ((unsigned int)point.x & 0xffff);
    if (pressed == 1) {
        code = key & 0xffff;
        if (gfKeyState[code] == 0) {
            if (gfCurrentStringInputState == 0) {
                gfKeyState[code] = 1;
                QueueEvent(1, code, packed);
                return;
            }
        } else if (gfCurrentStringInputState == 0) {
            QueueEvent(4, code, packed);
            return;
        }
        RedirectToString((unsigned short)key);
        return;
    }
    code = key & 0xffff;
    if (gfKeyState[code] == 1) {
        gfKeyState[code] = 0;
        QueueEvent(2, code, packed);
        return;
    }
    if ((short)key == 9 && gfAltState != 0) {
        ShowWindow(ghWindow, 6);
        gfKeyState[0x12] = 0;
        gfAltState = 0;
    }
}

void KeyDown(UINT32 usParam, UINT32 uiParam)
{                        // Are we PRESSING down one of SHIFT, ALT or CTRL ???
    if (usParam == 16) { // SHIFT key is PRESSED
        gfShiftState = SHIFT_DOWN;
        gfKeyState[16] = TRUE;
    } else {
        if (usParam == 17) { // CTRL key is PRESSED
            gfCtrlState = CTRL_DOWN;
            gfKeyState[17] = TRUE;
        } else {
            if (usParam == 18) { // ALT key is pressed
                gfAltState = ALT_DOWN;
                gfKeyState[18] = TRUE;
            } else {
                if (usParam == SNAPSHOT) {
                    //PrintScreen();
                    // DB Done in the KeyUp function
                    // this used to be keyed to SCRL_LOCK
                    // which I believe Luis gave the wrong value
                } else {
                    // No special keys have been pressed
                    // Call KeyChange() and pass TRUE to indicate key has been PRESSED and not RELEASED
                    KeyChange(usParam, uiParam, TRUE);
                }
            }
        }
    }
}

void KeyUp(UINT32 usParam, UINT32 uiParam)
{                        // Are we RELEASING one of SHIFT, ALT or CTRL ???
    if (usParam == 16) { // SHIFT key is RELEASED
        BOOLEAN fAltHeld = gfAltState != FALSE;
        gfShiftState = FALSE;
        gfKeyState[16] = FALSE;
        /* Mod (Russian translation): Alt+Shift switches the text input layout. */
        if (gfCyrillicText && fAltHeld) {
            gusKeyboardLayout ^= 1;
        }
    } else {
        if (usParam == 17) { // CTRL key is RELEASED
            gfCtrlState = FALSE;
            gfKeyState[17] = FALSE;
        } else {
            if (usParam == 18) { // ALT key is RELEASED
                gfAltState = FALSE;
                gfKeyState[18] = FALSE;
            } else {
                if (usParam == SNAPSHOT) {
                    // DB this used to be keyed to SCRL_LOCK
                    // which I believe Luis gave the wrong value
                    //#ifndef JA2
                    if (_KeyDown(CTRL))
                        VideoCaptureToggle();
                    else
                        //#endif
                        PrintScreen();
                } else {
                    // No special keys have been pressed
                    // Call KeyChange() and pass FALSE to indicate key has been PRESSED and not RELEASED
                    KeyChange(usParam, uiParam, FALSE);
                }
            }
        }
    }
}

// These functions will be used for string input

// Since all string input will have to be handle by reentrant capable functions (since we must attend
// to windows messaging as well as network traffic related issues), whenever there is ongoing string input
// going on, we must use InitStringInput() and HandleStringInput() to get the job done. HandleStringInput()
// will return TRUE as long as the string input is going on, and FALSE when its done
//
// During string input, all keyboard are rerouted to the string and hence are not queued up on the
// event queue or registered in the state table. Also note that several string inputs can occur
// at the same time. Use the SetStringFocus() function to manager the focus for multiple
// string inputs

BOOLEAN CharacterIsValid(UINT16 usCharacter, UINT16* pFilter)
{
    UINT32 uiIndex, uiEndIndex;

    if (pFilter != NULL) {
        uiEndIndex = *pFilter;
        for (uiIndex = 1; uiIndex <= *pFilter; uiIndex++) {
            if (usCharacter == *(pFilter + uiIndex)) {
                return TRUE;
            }
        }
        return FALSE;
    }

    return TRUE;
}

// FUNCTION: WIZ8 0x004023b0
void RedirectToString(UINT16 usInputCharacter)
{
    UINT16 usIndex;

    if (gpCurrentStringDescriptor != NULL) {
        // Handle the new character input
        switch (usInputCharacter) {
        case ENTER: // ENTER is pressed, the last character field should be set to ENTER
            if (gpCurrentStringDescriptor->pNextString != NULL) {
                gpCurrentStringDescriptor->fFocus = FALSE;
                gpCurrentStringDescriptor = gpCurrentStringDescriptor->pNextString;
                gpCurrentStringDescriptor->fFocus = TRUE;
                gpCurrentStringDescriptor->usLastCharacter = 0;
            } else {
                gpCurrentStringDescriptor->fFocus = FALSE;
                gpCurrentStringDescriptor->usLastCharacter = usInputCharacter;
                gfCurrentStringInputState = FALSE;
            }
            break;
        case ESC: // ESC was pressed, the last character field should be set to ESC
            gpCurrentStringDescriptor->fFocus = FALSE;
            gpCurrentStringDescriptor->usLastCharacter = usInputCharacter;
            gfCurrentStringInputState = FALSE;
            break;
        case TAB:
            if (gfShiftState) {
                if (gpCurrentStringDescriptor->pPreviousString == NULL)
                    return;
                gpCurrentStringDescriptor->fFocus = FALSE;
                gpCurrentStringDescriptor = gpCurrentStringDescriptor->pPreviousString;
            } else {
                if (gpCurrentStringDescriptor->pNextString == NULL)
                    return;
                gpCurrentStringDescriptor->fFocus = FALSE;
                gpCurrentStringDescriptor = gpCurrentStringDescriptor->pNextString;
            }
            gpCurrentStringDescriptor->fFocus = TRUE;
            gpCurrentStringDescriptor->usLastCharacter = 0;
            break;
        case 0x26: // The UPARROW was pressed, the last character field should be set to UPARROW
            if (gpCurrentStringDescriptor->pPreviousString != NULL) {
                gpCurrentStringDescriptor->fFocus = FALSE;
                gpCurrentStringDescriptor = gpCurrentStringDescriptor->pPreviousString;
                gpCurrentStringDescriptor->fFocus = TRUE;
                gpCurrentStringDescriptor->usLastCharacter = 0;
            }
            break;
        case 0x28: // The DNARROW was pressed, the last character field should be set to DNARROW
            if (gpCurrentStringDescriptor->pNextString != NULL) {
                gpCurrentStringDescriptor->fFocus = FALSE;
                gpCurrentStringDescriptor = gpCurrentStringDescriptor->pNextString;
                gpCurrentStringDescriptor->fFocus = TRUE;
                gpCurrentStringDescriptor->usLastCharacter = 0;
            }
            break;
        case 0x25: // The LEFTARROW was pressed, move one character to the left
            if (gpCurrentStringDescriptor->usStringOffset > 0) { // Decrement the offset
                gpCurrentStringDescriptor->usStringOffset--;
            }
            gpCurrentStringDescriptor->usLastCharacter = usInputCharacter;
            break;
        case 0x27: // The RIGHTARROW was pressed, move one character to the right
            if (gpCurrentStringDescriptor->usStringOffset <
                gpCurrentStringDescriptor
                    ->usCurrentStringLength) { // Ok we can move the cursor one up without going past the end of string
                gpCurrentStringDescriptor->usStringOffset++;
            }
            gpCurrentStringDescriptor->usLastCharacter = usInputCharacter;
            break;
        case BACKSPACE: // Delete the character preceding the cursor
            if (gpCurrentStringDescriptor->usStringOffset >
                0) { // Ok, we are not at the beginning of the string, so we may proceed
                for (usIndex = gpCurrentStringDescriptor->usStringOffset;
                     usIndex <= gpCurrentStringDescriptor->usCurrentStringLength;
                     usIndex++) { // Shift the characters one at a time
                    *(gpCurrentStringDescriptor->pString + usIndex - 1) =
                        *(gpCurrentStringDescriptor->pString + usIndex);
                }
                gpCurrentStringDescriptor->usStringOffset--;
                gpCurrentStringDescriptor->usCurrentStringLength--;
            }

            break;
        case 0x2e: // Delete the character which follows the cursor
            if (gpCurrentStringDescriptor->usStringOffset <
                gpCurrentStringDescriptor
                    ->usCurrentStringLength) { // Ok we are not at the end of the string, so we may proceed
                for (usIndex = gpCurrentStringDescriptor->usStringOffset;
                     usIndex < gpCurrentStringDescriptor->usCurrentStringLength;
                     usIndex++) { // Shift the characters one at a time
                    *(gpCurrentStringDescriptor->pString + usIndex) =
                        *(gpCurrentStringDescriptor->pString + usIndex + 1);
                }
                gpCurrentStringDescriptor->usCurrentStringLength--;
            }
            gpCurrentStringDescriptor->usLastCharacter = usInputCharacter;
            break;
        case 0x2d: // Toggle insert mode
            if (gpCurrentStringDescriptor->fInsertMode == TRUE) {
                gpCurrentStringDescriptor->fInsertMode = FALSE;
            } else {
                gpCurrentStringDescriptor->fInsertMode = TRUE;
            }
            gpCurrentStringDescriptor->usLastCharacter = usInputCharacter;
            break;
        case 0x24: // Go to the beginning of the input string
            gpCurrentStringDescriptor->usStringOffset = 0;
            gpCurrentStringDescriptor->usLastCharacter = usInputCharacter;
            break;
            // Stupid definition causes problems with headers that use the keyword END -- DB
        case 0x23: // Go to the end of the input string
            gpCurrentStringDescriptor->usStringOffset =
                gpCurrentStringDescriptor->usCurrentStringLength;
            gpCurrentStringDescriptor->usLastCharacter = usInputCharacter;
            break;
        default: //
            // normal input
            //
            usInputCharacter = TranslateKeyToCharacter(
                usInputCharacter, (UINT8)(gfAltState | gfCtrlState | gfShiftState));
            if (usInputCharacter == 0)
                return;
            if (CharacterIsValid(usInputCharacter, gpCurrentStringDescriptor->pFilter) == TRUE) {
                if (gpCurrentStringDescriptor->fInsertMode ==
                    TRUE) { // Before we can shift characters for the insert, we must make sure we have the space
                    if (gpCurrentStringDescriptor->usCurrentStringLength <
                        (gpCurrentStringDescriptor->usMaxStringLength -
                         1)) { // Before we can add a new character we must shift existing ones to for the insert
                        for (usIndex = gpCurrentStringDescriptor->usCurrentStringLength;
                             usIndex > gpCurrentStringDescriptor->usStringOffset;
                             usIndex--) { // Shift the characters one at a time
                            *(gpCurrentStringDescriptor->pString + usIndex) =
                                *(gpCurrentStringDescriptor->pString + usIndex - 1);
                        }
                        // Ok now we introduce the new character
                        *(gpCurrentStringDescriptor->pString + usIndex) = usInputCharacter;
                        gpCurrentStringDescriptor->usStringOffset++;
                        gpCurrentStringDescriptor->usCurrentStringLength++;
                    }
                } else {
                    // Ok, add character to string (by overwriting)
                    if (gpCurrentStringDescriptor->usStringOffset <
                        (gpCurrentStringDescriptor->usMaxStringLength -
                         1)) { // Ok, we have not exceeded the maximum number of characters yet
                        *(gpCurrentStringDescriptor->pString +
                          gpCurrentStringDescriptor->usStringOffset) = usInputCharacter;
                        gpCurrentStringDescriptor->usStringOffset++;
                    }
                    // Did we push back the current string length (i.e. add character to end of string)
                    if (gpCurrentStringDescriptor->usStringOffset >
                        gpCurrentStringDescriptor->usCurrentStringLength) { // Add a NULL character
                        *(gpCurrentStringDescriptor->pString +
                          gpCurrentStringDescriptor->usStringOffset) = 0;
                        gpCurrentStringDescriptor->usCurrentStringLength++;
                    }
                }
                gpCurrentStringDescriptor->usLastCharacter = usInputCharacter;
            }
            break;
        }
    }
}

//
// Miscellaneous input-related utility functions:
//

// FUNCTION: WIZ8 0x00402750
void FreeMouseCursor(void)
{
    ClipCursor(NULL);
    fCursorWasClipped = FALSE;
}

void HandleSingleClicksAndButtonRepeats(void)
{
    UINT32 uiTimer;

    uiTimer = GetTickCount();

    // Is there a LEFT mouse button repeat
    if (gfLeftButtonState) {
        if ((guiLeftButtonRepeatTimer > 0) && (guiLeftButtonRepeatTimer <= uiTimer)) {
            UINT32 uiTmpLParam;
            POINT MousePos;

            GetCursorPos(&MousePos);
            uiTmpLParam = ((MousePos.y << 16) & 0xffff0000) | (MousePos.x & 0x0000ffff);
            QueueEvent(LEFT_BUTTON_REPEAT, 0, uiTmpLParam);
            guiLeftButtonRepeatTimer = uiTimer + BUTTON_REPEAT_TIME;
        }
    } else {
        guiLeftButtonRepeatTimer = 0;
    }

    // Is there a RIGHT mouse button repeat
    if (gfRightButtonState) {
        if ((guiRightButtonRepeatTimer > 0) && (guiRightButtonRepeatTimer <= uiTimer)) {
            UINT32 uiTmpLParam;
            POINT MousePos;

            GetCursorPos(&MousePos);
            uiTmpLParam = ((MousePos.y << 16) & 0xffff0000) | (MousePos.x & 0x0000ffff);
            QueueEvent(RIGHT_BUTTON_REPEAT, 0, uiTmpLParam);
            guiRightButtonRepeatTimer = uiTimer + BUTTON_REPEAT_TIME;
        }
    } else {
        guiRightButtonRepeatTimer = 0;
    }
}

// FUNCTION: WIZ8 0x00402760
INT16 GetMouseWheelDeltaValue(UINT32 wParam)
{
    INT16 sDelta = HIWORD(wParam);

    return (sDelta / WHEEL_DELTA);
}

// FUNCTION: WIZ8 0x00402780
unsigned short TranslateKeyToCharacter(unsigned short key, unsigned char modifiers)
{
    if ((modifiers & (CTRL_DOWN | ALT_DOWN)) != 0)
        return 0;
    /* Mod (Russian translation): table of the current layout (without the mod - always English). */
    UINT16* table = ActiveKeyTranslationTable();
    if ((modifiers & SHIFT_DOWN) != 0)
        return table[key + 256];
    return table[key];
}

// FUNCTION: WIZ8 0x004027C0
unsigned short TranslateCharacterToKey(unsigned short character)
{
    UINT16 key;
    UINT16* table = ActiveKeyTranslationTable(); // mod (Russian translation), see above
    for (key = 0; key < 0x200; ++key) {
        if (table[key] == character) {
            return key % 256;
        }
    }
    return 0;
}

/* Mod (Russian translation): in its exe letter case also handles cp1251 Cyrillic
   (0xC0..0xDF - uppercase А..Я, 0xE0..0xFF - lowercase а..я; Ё/ё are left alone).
   Used in Text_Input.cpp (capitalizing the first letter of a name, etc.) and in
   CompareWideTextIgnoreAsciiCase (keyword search in NPC conversations). */
static BOOLEAN IsCyrillicUpper(unsigned short character)
{
    return gfCyrillicText && character >= 0xC0 && character <= 0xDF;
}

static BOOLEAN IsCyrillicLower(unsigned short character)
{
    return gfCyrillicText && character >= 0xE0 && character <= 0xFF;
}

// FUNCTION: WIZ8 0x00402800
BOOLEAN IsUppercaseWideChar(unsigned short character)
{
    if (character >= L'A' && character <= L'Z')
        return TRUE;
    if (IsCyrillicUpper(character))
        return TRUE;
    return FALSE;
}

// FUNCTION: WIZ8 0x00402820
BOOLEAN IsLowercaseWideChar(unsigned short character)
{
    if (character >= L'a' && character <= L'z')
        return TRUE;
    if (IsCyrillicLower(character))
        return TRUE;
    return FALSE;
}

// FUNCTION: WIZ8 0x00402840
BOOLEAN IsPunctuationWideChar(unsigned short character)
{
    if ((character >= L'!' && character <= L'/') || (character >= L':' && character <= L'@') ||
        (character >= L'[' && character <= L'_') || (character >= L'{' && character <= L'}'))
        return TRUE;
    return FALSE;
}

// FUNCTION: WIZ8 0x00402880
int ToUppercaseWideChar(int character)
{
    if (((unsigned short)character > L'`' && (unsigned short)character < L'{') ||
        IsCyrillicLower((unsigned short)character)) { // mod (Russian translation)
        character -= L'a' - L'A';
    }
    return character;
}

// FUNCTION: WIZ8 0x004028A0
int ToLowercaseWideChar(int character)
{
    if (((unsigned short)character > L'@' && (unsigned short)character < L'[') ||
        IsCyrillicUpper((unsigned short)character)) { // mod (Russian translation)
        character += L'a' - L'A';
    }
    return character;
}

// FUNCTION: WIZ8 0x00402920
int CompareWideTextIgnoreAsciiCase(const wchar_t* first, const wchar_t* second)
{
    unsigned short left;
    unsigned short right;
    do {
        left = ToLowercaseWideChar(*first++);
        right = ToLowercaseWideChar(*second++);
    } while (left != 0 && left == right);
    return (UINT32)left - (UINT32)right;
}

/* Bounded variant (the NPC dialogue prefix tests: "join", "where is", ...).
   Retail compares at most `count` characters, folding A-Z only; the decomp
   had replaced it with CRT _wcsnicmp (ASCII only as well). Folding goes
   through ToLowercaseWideChar, so with the Russian text on (gfCyrillicText)
   cp1251 capitals 0xC0..0xDF match their lowercase letters, as in the
   translators' exe (0x402930); English behaviour is unchanged. */
// FUNCTION: WIZ8 0x004028c0
int CompareWideTextNIgnoreCase(const wchar_t* first, const wchar_t* second, UINT32 count)
{
    unsigned short left = 0;
    unsigned short right = 0;
    while (count != 0) {
        left = (unsigned short)ToLowercaseWideChar(*first++);
        right = (unsigned short)ToLowercaseWideChar(*second++);
        if (--count == 0 || left == 0 || left != right) {
            break;
        }
    }
    return (int)left - (int)right;
}
