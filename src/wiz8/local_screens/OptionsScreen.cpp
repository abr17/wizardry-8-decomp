#include "soundman.h"
#include "Font.h"
#include "Types.h"
#include "mousesystem.h"
#include "wiz8/engine_code/Quality.h"
#include "wiz8/engine_code/Video2.h"
#include "wiz8/local_code/ControlsRect.h"
#include "wiz8/local_code/TextBuffer.h"
#include "wiz8/local_code/TextControl.h"
#include "wiz8/local_code/ControlSelection.h"
#include "wiz8/local_screens/OptionsScreen.h"
#include "wiz8/local_screens/JournalScreen.h"
#include "wiz8/local_screens/MGSKeyboard.h"
#include "wiz8/xstatus.h"

#include "wiz8/layouts/combat_state.h"
#include "wiz8/cursor.h"
#include "wiz8/local_screens/Screens.h"
#include "wiz8/layouts/game_status.h"
#include "wiz8/local_code/GameplayCode.h"
#include "wiz8/local_code/GameplayDatabase.h"
#include "wiz8/local_code/LoadSaveGame.h"
#include "wiz8/local_code/Configuration.h"
#include "wiz8/local_code/character_events.h"
#include "wiz8/local_code/MonsterManager.h"
#include "wiz8/engine_code/AmbientSound.h"
#include "wiz8/engine_code/Levels.h"
#include "wiz8/sound_man.h"
#include "wiz8/engine_code/World.h"
#include "wiz8/geometry.h"
#include "wiz8/music_playlist.h"
#include "wiz8/regions.h"
#include "wiz8/layouts/screen_state.h"
#include "wiz8/local_code/Gameloop.h"
#include "wiz8/fonts.h"
#include "wiz8/sr_api.h"
#include "wiz8/utility.h"
#include "wiz8/video_object_catalog.h"
#include "wiz8/text_input.h"
#include "wiz8/local_code/Strings.h"
#include "wiz8/canvas.h"
#include "wiz8/russian_text.h"
#include "wiz8/layouts/world.h"
#include "wiz8/engine_code/3d.h"
#include "wiz8/local_screens/MainGameLayout.h"
#include "vsurface.h"
#include "himage.h"
#include "input.h"
#include "FileMan.h"
#include "surrender/srColorSurface.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <wchar.h>

// GLOBAL: WIZ8 0x0069C130
static unsigned int* g_options_panel_region_sets;

/* Mod: the open resolution drop-down list covers this label (below). */
class W8OptionsPanel;
bool OptionsDropdownCoversText(const W8OptionsPanel* panel, const W8TextBuffer* text);

/* Mod: top of the monster speed slider (5x in the original). */
#define W8_MAX_MONSTER_SPEED 10.0f

void MSYS_SGP_Mouse_Handler_Hook(unsigned short event, unsigned short x, unsigned short y,
                                 char right_button, char left_button);

/* Shared zero-initialized wide string: binary-wide DATA references read it as
   empty text and as a swprintf format argument. No recovered writer exists. */
// GLOBAL: WIZ8 0x00689b34
wchar_t g_empty_wide_string;

/* Suppress hover sounds on the registered options-row children. */
// GLOBAL: WIZ8 0x005ed590
extern const unsigned int g_W8TextControlSilentHover = 0x40;

/* ---------------------------------------------------------------------------
   "Wide windows" mod, stage 2 (2026-10-12): the contents of the right options panel
   are laid out across the stretched panel, like a responsive (fluid) layout. Setting
   labels stay at the left edge of the panel; sliders, checkboxes, toggles and
   key buttons move to the right edge; save list rows are stretched to the
   full width. The panel (Controls) stays centered, and each child
   gets its own mouse-region anchor (it is drawn by the same anchor, see
   Controls::ChildLayoutAnchor). Outside the canvas layout all shifts are zero -
   everything as in the original.
   --------------------------------------------------------------------------- */
static void AnchorOptionsWidget(W8Widget* widget, int anchor)
{
    if (widget != 0 && widget->m_region != -1) {
        SetRegionLayoutAnchor(static_cast<unsigned int>(widget->m_region), anchor);
    }
}

/* Shift of a widget toward its canvas edge (0 outside the layout): for code that
   reads the cursor itself (SGPMouseGetPos) and subtracts the widget corner from it. */
static int OptionsWidgetAnchorShift(const W8Widget* widget)
{
    if (widget->m_region == -1) {
        return 0;
    }
    return CanvasAnchorShift(GetRegionLayoutAnchor(static_cast<unsigned int>(widget->m_region)));
}

/* Panel label (W8TextBuffer from m_text_buffers) - which canvas edge it sits at:
   right-aligned ones (toggle labels) - on the right, next to their
   buttons; centered ones (headings) - centered; the rest - on the left. */
static int OptionsTextAnchor(const W8TextBuffer* text)
{
    unsigned int mode = text->GetLayoutMode();
    if ((mode & g_W8TextBufferAlignRight) != 0) {
        return W8_ANCHOR_RIGHT;
    }
    if ((mode & g_W8TextBufferAlignCenter) != 0) {
        return W8_ANCHOR_CENTER;
    }
    return W8_ANCHOR_LEFT;
}

/* A save list row (save_load_gamelists.sti, 352 px) in the layout is wider
   by 2*ox: the piece [171, 299) inside the text field is repeated - the dark cracked texture
   tiles with itself without a seam (found with wiz8-widescreen scripts/art/find_slice.py).
   The thumbnail on the left and the frame on the right are pixel-for-pixel original. */
enum {
    W8_OPTIONS_SAVE_ROW_SLICE_FROM = 171,
    W8_OPTIONS_SAVE_ROW_SLICE_TO = 299
};

static int OptionsSaveRowStretch(void)
{
    return CanvasWideLayoutActive() ? 2 * g_canvas_ox : 0;
}

// GLOBAL: WIZ8 0x0069c1c8
static bool g_options_first_frame;
// GLOBAL: WIZ8 0x0069c138
W8OptionsValues g_options_values;
// GLOBAL: WIZ8 0x0069c254
W8OptionsScreen* g_options_screen;
// GLOBAL: WIZ8 0x0069c1cc
wchar_t g_options_last_save_name[64];

// GLOBAL: WIZ8 0x0064d72c
static int g_last_options_panel = 5;

// GLOBAL: WIZ8 0x0069c24c
static unsigned int g_options_menu_region_set;
// GLOBAL: WIZ8 0x0069c250
static unsigned int g_options_page_region_set;

// GLOBAL: WIZ8 0x0064d0b8
static W8OptionsMenuRow g_options_menu_rows[8] = {
    {1, {{20, 51, 69, 137}, {-1, -1, -1, -1}}, {16, 194, 0, 0}, {0, 2, 1, -1, -1}},
    {4, {{90, 72, 117, 117}, {217, 72, 244, 117}}, {16, 220, 0, 0}, {3, 5, 4, -1, -1}},
    {3, {{132, 29, 201, 102}, {-1, -1, -1, -1}}, {16, 246, 0, 0}, {6, 8, 7, -1, -1}},
    {2, {{124, 141, 194, 172}, {-1, -1, -1, -1}}, {16, 272, 0, 0}, {9, 11, 10, -1, -1}},
    {0, {{-1, -1, -1, -1}, {-1, -1, -1, -1}}, {7, 322, 0, 0}, {12, 14, 13, -1, 25}},
    {0, {{-1, -1, -1, -1}, {-1, -1, -1, -1}}, {7, 360, 0, 0}, {15, 17, 16, -1, 24}},
    {0, {{-1, -1, -1, -1}, {-1, -1, -1, -1}}, {7, 398, 0, 0}, {18, 20, 19, -1, -1}},
    {0, {{-1, -1, -1, -1}, {-1, -1, -1, -1}}, {7, 436, 0, 0}, {21, 23, 22, -1, -1}}};

// GLOBAL: WIZ8 0x0064d078
static W8OptionsPanelRange g_options_panel_ranges[8] = {{0, 2},   {3, 3},   {4, 5}, {6, 10},
                                                        {11, 11}, {12, 12}, {0, 0}, {0, 0}};
// GLOBAL: WIZ8 0x0064d730
static int g_options_difficulty_labels[3] = {0x7f8, 0x7f9, 0x7fa};
// GLOBAL: WIZ8 0x0064d73c
static int g_options_camera_rotation_labels[3] = {0x7fe, 0x7ff, 0x800};
// GLOBAL: WIZ8 0x0064d748
static int g_options_combat_mode_labels[2] = {0x7f6, 0x7f5};
// GLOBAL: WIZ8 0x0064d750
static int g_options_audio_labels[4] = {0x81e, 0x81f, 0x821, 0x820};

struct W8OptionsKeyName {
    unsigned short key;
    unsigned short reserved;
    int label;
};

struct W8OptionsKeyRow {
    W8MGSCommand primary_binding;
    W8MGSCommand secondary_binding;
    int label;
};

struct W8OptionsKeyboardPage {
    int title;
    W8MGSCommand first_binding;
    W8MGSCommand last_binding;
};

// GLOBAL: WIZ8 0x0064d2f8
static W8OptionsKeyName g_options_key_names[] = {
    {112, 0, 2158}, {113, 0, 2159}, {114, 0, 2160}, {115, 0, 2161}, {116, 0, 2162}, {117, 0, 2163},
    {118, 0, 2164}, {119, 0, 2165}, {120, 0, 2166}, {121, 0, 2167}, {122, 0, 2168}, {123, 0, 2169},
    {9, 0, 2170},   {144, 0, 2171}, {8, 0, 2172},   {45, 0, 2173},  {46, 0, 2174},  {35, 0, 2175},
    {34, 0, 2176},  {33, 0, 2177},  {36, 0, 2178},  {13, 0, 2179},  {32, 0, 2180},  {40, 0, 2181},
    {37, 0, 2182},  {39, 0, 2183},  {38, 0, 2184},  {19, 0, 2185},  {145, 0, 2186}, {20, 0, 2187},
    {96, 0, 2188},  {97, 0, 2189},  {98, 0, 2190},  {99, 0, 2191},  {100, 0, 2192}, {101, 0, 2193},
    {102, 0, 2194}, {103, 0, 2195}, {104, 0, 2196}, {105, 0, 2197}, {106, 0, 2198}, {107, 0, 2199},
    {108, 0, 2200}, {109, 0, 2201}, {110, 0, 2202}, {111, 0, 2203}, {144, 0, 2171}, {192, 0, 2204},
    {0, 0, 2157}};

// GLOBAL: WIZ8 0x0064d480
static W8OptionsKeyRow g_options_key_rows[] = {
    {W8_MGS_COMMAND_MOVE_FORWARD, W8_MGS_COMMAND_MOVE_FORWARD_RUN, 2106},
    {W8_MGS_COMMAND_MOVE_BACKWARD, W8_MGS_COMMAND_MOVE_BACKWARD_RUN, 2107},
    {W8_MGS_COMMAND_TURN_LEFT, W8_MGS_COMMAND_TURN_LEFT_ALT, 2108},
    {W8_MGS_COMMAND_TURN_RIGHT, W8_MGS_COMMAND_TURN_RIGHT_ALT, 2109},
    {W8_MGS_COMMAND_STRAFE_LEFT, W8_MGS_COMMAND_STRAFE_LEFT_RUN, 2110},
    {W8_MGS_COMMAND_STRAFE_RIGHT, W8_MGS_COMMAND_STRAFE_RIGHT_RUN, 2111},
    {W8_MGS_COMMAND_LOOK_UP, W8_MGS_COMMAND_NONE, 2112},
    {W8_MGS_COMMAND_LOOK_DOWN, W8_MGS_COMMAND_NONE, 2113},
    {W8_MGS_COMMAND_LOOK_LEVEL, W8_MGS_COMMAND_NONE, 2114},
    {W8_MGS_COMMAND_PAUSE_GAME, W8_MGS_COMMAND_NONE, 2115},
    {W8_MGS_COMMAND_NEXT_LAYOUT, W8_MGS_COMMAND_PREV_LAYOUT, 2116},
    {W8_MGS_COMMAND_AUTOMAP, W8_MGS_COMMAND_NONE, 2117},
    {W8_MGS_COMMAND_OPTIONS, W8_MGS_COMMAND_NONE, 2118},
    {W8_MGS_COMMAND_USE_ITEM, W8_MGS_COMMAND_USE_LAST_ITEM, 2121},
    {W8_MGS_COMMAND_CAST_SPELL, W8_MGS_COMMAND_CAST_LAST_SPELL, 2122},
    {W8_MGS_COMMAND_INVENTORY, W8_MGS_COMMAND_NONE, 2120},
    {W8_MGS_COMMAND_CAMP, W8_MGS_COMMAND_NONE, 2123},
    {W8_MGS_COMMAND_TOGGLE_SEARCH, W8_MGS_COMMAND_NONE, 2124},
    {W8_MGS_COMMAND_TOGGLE_COMBAT, W8_MGS_COMMAND_NONE, 2125},
    {W8_MGS_COMMAND_RADAR_ZOOM, W8_MGS_COMMAND_NONE, 2127},
    {W8_MGS_COMMAND_QUICK_SAVE, W8_MGS_COMMAND_QUICK_LOAD, 2126},
    {W8_MGS_COMMAND_REPLAY_QUOTE, W8_MGS_COMMAND_NONE, 2128},
    {W8_MGS_COMMAND_SWAP_WEAPONS, W8_MGS_COMMAND_SWAP_ALL_WEAPONS, 2129},
    {W8_MGS_COMMAND_JOURNAL, W8_MGS_COMMAND_NONE, 2119},
    {W8_MGS_COMMAND_SELECT_PC_1, W8_MGS_COMMAND_NONE, 2130},
    {W8_MGS_COMMAND_SELECT_PC_2, W8_MGS_COMMAND_NONE, 2131},
    {W8_MGS_COMMAND_SELECT_PC_3, W8_MGS_COMMAND_NONE, 2132},
    {W8_MGS_COMMAND_SELECT_PC_4, W8_MGS_COMMAND_NONE, 2133},
    {W8_MGS_COMMAND_SELECT_PC_5, W8_MGS_COMMAND_NONE, 2134},
    {W8_MGS_COMMAND_SELECT_PC_6, W8_MGS_COMMAND_NONE, 2135},
    {W8_MGS_COMMAND_SELECT_RECRUITED_1, W8_MGS_COMMAND_NONE, 2136},
    {W8_MGS_COMMAND_SELECT_RECRUITED_2, W8_MGS_COMMAND_NONE, 2137},
    {W8_MGS_COMMAND_TEXTBOX_PAGE_UP, W8_MGS_COMMAND_NONE, 2138},
    {W8_MGS_COMMAND_TEXTBOX_PAGE_DOWN, W8_MGS_COMMAND_NONE, 2139},
    {W8_MGS_COMMAND_TEXTBOX_TOP, W8_MGS_COMMAND_NONE, 2140},
    {W8_MGS_COMMAND_TEXTBOX_BOTTOM, W8_MGS_COMMAND_NONE, 2141},
    {W8_MGS_COMMAND_TEXTBOX_CLEAR, W8_MGS_COMMAND_NONE, 2142},
    {W8_MGS_COMMAND_START_COMBAT_ROUND, W8_MGS_COMMAND_NONE, 2143},
    {W8_MGS_COMMAND_CONTINUOUS_COMBAT, W8_MGS_COMMAND_NONE, 2144},
    {W8_MGS_COMMAND_CAMERA_LOCK, W8_MGS_COMMAND_NONE, 2145},
    {W8_MGS_COMMAND_CYCLE_TARGET, W8_MGS_COMMAND_NONE, 2146},
    {W8_MGS_COMMAND_ATTACK, W8_MGS_COMMAND_NONE, 2147},
    {W8_MGS_COMMAND_BERSERK, W8_MGS_COMMAND_NONE, 2148},
    {W8_MGS_COMMAND_BREATHE, W8_MGS_COMMAND_NONE, 2149},
    {W8_MGS_COMMAND_TURN_UNDEAD, W8_MGS_COMMAND_NONE, 2150},
    {W8_MGS_COMMAND_PRAY, W8_MGS_COMMAND_NONE, 2151},
    {W8_MGS_COMMAND_DEFEND, W8_MGS_COMMAND_NONE, 2152},
    {W8_MGS_COMMAND_PROTECT, W8_MGS_COMMAND_NONE, 2153},
    {W8_MGS_COMMAND_EQUIP, W8_MGS_COMMAND_NONE, 2154},
    {W8_MGS_COMMAND_PARTY_WALK, W8_MGS_COMMAND_PARTY_RUN, 2155},
    {W8_MGS_COMMAND_REPEAT_ACTION, W8_MGS_COMMAND_NONE, 2156},
    {W8_MGS_COMMAND_NONE, W8_MGS_COMMAND_NONE, -1}};

// GLOBAL: WIZ8 0x0064d6f0
static W8OptionsKeyboardPage g_options_keyboard_pages[5] = {
    {0x835, W8_MGS_COMMAND_MOVE_FORWARD, W8_MGS_COMMAND_LOOK_LEVEL},
    {0x836, W8_MGS_COMMAND_PAUSE_GAME, W8_MGS_COMMAND_JOURNAL},
    {0x837, W8_MGS_COMMAND_SELECT_PC_1, W8_MGS_COMMAND_SELECT_RECRUITED_2},
    {0x838, W8_MGS_COMMAND_TEXTBOX_PAGE_UP, W8_MGS_COMMAND_TEXTBOX_CLEAR},
    {0x839, W8_MGS_COMMAND_START_COMBAT_ROUND, W8_MGS_COMMAND_REPEAT_ACTION}};

W8OptionsPanelSet::W8OptionsPanelSet()
    : m_page_count(0), m_compact_layout(0), m_hide_navigation(0), m_active(false), m_current(0)
{
}

W8OptionsPanelSet::~W8OptionsPanelSet()
{
    m_panels.RemoveAllAndDelete();
}

/* Standalone JMP thunk onto Controls::~Controls emitted for this TU. */

/* Vtable 0x005EEDDC is an unnamed W8TextControl subclass emitted for this TU:
   identical to W8TextControl's table except slot 0, which holds the generated
   deleting destructor 0x005A76B0 wrapping the emitted destructor 0x005A76D0. */

W8OptionsGamePanel::W8OptionsGamePanel() : W8OptionsPanel(0) {}

W8OptionsMousePanel::W8OptionsMousePanel() : W8OptionsPanel(1) {}

W8OptionsInterfacePanel::W8OptionsInterfacePanel() : W8OptionsPanel(2) {}

W8OptionsAudioPanel::W8OptionsAudioPanel() : W8OptionsPanel(3) {}

W8OptionsGraphicsPanel::W8OptionsGraphicsPanel() : W8OptionsPanel(4) {}

W8OptionsAdvancedGraphicsPanel::W8OptionsAdvancedGraphicsPanel() : W8OptionsPanel(5) {}

W8OptionsKeyboardPanel::W8OptionsKeyboardPanel(int panel)
    : W8OptionsPanel(panel), m_panel(panel), m_captured_button(0)
{
}
W8OptionsSaveLoadPanel::W8OptionsSaveLoadPanel(int panel) : W8OptionsPanel(panel), m_panel(panel)
{
    m_catalogObject = 0xf7;
    m_catalogImage = 0;
}

W8OptionsUnavailablePanel::W8OptionsUnavailablePanel(int message)
    : W8OptionsPanel(13), m_message(message)
{
}

void W8OptionsSaveRow::SetSave(W8SaveSlot* save)
{
    m_save = save;
    SetEnabled(save != 0);
    Invalidate(false);
}

// FUNCTION: WIZ8 0x005aa7d0
void W8OptionsSaveLoadPanel::Populate()
{
    int top = 0x12;
    for (int index = 0; index < 5; ++index) {
        W8OptionsSaveRow* row = new W8OptionsSaveRow(this, top, m_panel == 12);
        AnchorOptionsWidget(row, W8_ANCHOR_LEFT); /* mod: stretched from the left edge */
        row->m_save_listener = this;
        m_selection.AddEntry(row);
        m_rows.Add(row);
        top += 0x49;
    }
    m_selection.m_selectionListener = this;

    if (m_panel == 11) {
        m_delete_button =
            new W8TextControl(this, 0xffffffff, 0xd, 0x1b0, 0, 0, 0xf8, 0, 6, 8, 7, -1, -1);
        m_delete_button->m_listener = this;
        AnchorOptionsWidget(m_delete_button, W8_ANCHOR_LEFT); /* mod */
    } else {
        m_delete_button = 0;
    }

    int first_sprite = m_panel == 11 ? 3 : 0;
    m_action_button = new W8TextControl(this, 0xffffffff, 0xf5, 0x1b0, 0, 0, 0xf8, 0, first_sprite,
                                        first_sprite + 2, first_sprite + 1, -1, -1);
    m_action_button->m_listener = this;
    AnchorOptionsWidget(m_action_button, W8_ANCHOR_RIGHT); /* mod: in the right niche */

    if (m_panel == 11 && g_options_screen->m_save_slots.GetCount() == 1) {
        m_delete_button->SetEnabled(false);
        m_action_button->SetEnabled(false);
    }

    int page = 0;
    int selected = 0;
    if (m_panel == 11 && g_options_last_save_name[0] != 0) {
        for (int index = 1; index < g_options_screen->m_save_slots.GetCount(); ++index) {
            if (wcscmp(g_options_last_save_name,
                       (*g_options_screen->m_save_slots.GetAt(index))->name) == 0) {
                page = (index - 1) / 5;
                selected = (index - 1) % 5;
            }
        }
    }
    SetCurrent(page);
    m_selection.SetSelected(selected);
}

// FUNCTION: WIZ8 0x005aaac0
void W8OptionsSaveLoadPanel::SetActive(bool active)
{
    W8OptionsPanel::SetActive(active);
    if (active && g_options_screen->m_text_editor == 0 && m_panel == 12 &&
        m_selection.m_selectedIndex == 0 && m_current == 0) {
        OnEditSaveName(m_rows[0]);
    }
}

// FUNCTION: WIZ8 0x005aab30
void W8OptionsSaveLoadPanel::SetCurrent(int current)
{
    if (g_options_screen->m_text_editor != 0) {
        W8OptionsTextEditor* editor = g_options_screen->m_text_editor;
        if (editor->m_listener != 0) {
            editor->m_listener->OnTextEditComplete(editor, 1);
        }
        delete editor;
        g_options_screen->m_text_editor = 0;
    }
    m_current = current;
    Invalidate(0);

    int first = current * 5;
    if (m_panel == 11) {
        ++first;
    }
    int limit = g_options_screen->m_save_slots.GetCount();
    if (first + 5 < limit) {
        limit = first + 5;
    }
    for (int slot = first; slot < limit; ++slot) {
        W8SaveSlot* save = *g_options_screen->m_save_slots.GetAt(slot);
        W8OptionsSaveRow* row = *m_rows.GetAt(slot - first);
        row->SetSave(save);
    }
    for (; limit < first + 5; ++limit) {
        W8OptionsSaveRow* row = *m_rows.GetAt(limit - first);
        row->SetSave(0);
    }
    m_selection.SetSelected(0);
    if (g_options_screen->m_text_editor == 0 && m_panel == 12 && m_selection.m_selectedIndex == 0 &&
        m_current == 0) {
        OnEditSaveName(m_rows[0]);
    }
}

// FUNCTION: WIZ8 0x005aace0
void W8OptionsSaveLoadPanel::OnPrimary(W8TextControl* control)
{
    if (control == m_delete_button) {
        g_options_screen->ShowNotification(this, true, 0x828, 1);
        return;
    }
    if (control != m_action_button) {
        return;
    }
    if (m_panel == 11) {
        LoadSelectedSave();
        return;
    }

    W8OptionsTextEditor* editor = g_options_screen->m_text_editor;
    if (editor != 0) {
        if (editor->m_listener != 0) {
            editor->m_listener->OnTextEditComplete(editor, 0);
        }
        delete editor;
        g_options_screen->m_text_editor = 0;
    }
    if (editor == 0 && m_current == 0 && m_selection.m_selectedIndex == 0) {
        SaveSelectedSave();
    } else {
        g_options_screen->ShowNotification(this, true, 0x829, 2);
    }
}

// FUNCTION: WIZ8 0x005aadd0
void W8OptionsSaveLoadPanel::OnDialogClosed(bool accepted, int value)
{
    int selected_slot;
    if (!accepted) {
        if (value == 2) {
            if (g_options_screen->m_text_editor != 0 || m_panel != 12 ||
                m_selection.m_selectedIndex != 0) {
                return;
            }
            selected_slot = m_current;
        } else if (value == 3) {
            selected_slot = m_current * 5 + m_selection.m_selectedIndex;
            wcscpy((*g_options_screen->m_save_slots.GetAt(selected_slot))->name, m_previous_name);
            W8OptionsSaveRow* row = *m_rows.GetAt(m_selection.m_selectedIndex);
            row->SetSave(*g_options_screen->m_save_slots.GetAt(selected_slot));
            if (g_options_screen->m_text_editor != 0 || m_panel != 12 ||
                m_selection.m_selectedIndex != 0) {
                return;
            }
            selected_slot = m_current;
        } else {
            return;
        }
    } else {
        switch (value) {
        case 1:
            DeleteSelectedSave();
            return;
        case 3: {
            char path[260];
            sprintf(path, "%s\\%S.%s", "Saves", m_previous_name, g_save_extension);
            if (DeleteFileA(path) == 0) {
                g_options_screen->ShowNotification(this, false, 0x82e, 0);
                return;
            }
        }
        case 2:
            SaveSelectedSave();
            return;
        case 4:
            if (g_options_screen->m_text_editor != 0 || m_panel != 12 ||
                m_selection.m_selectedIndex != 0) {
                return;
            }
            selected_slot = m_current;
            break;
        default:
            return;
        }
    }
    if (selected_slot == 0) {
        OnEditSaveName(m_rows[0]);
    }
}

// FUNCTION: WIZ8 0x005aafc0
void W8OptionsSaveLoadPanel::OnTextEditComplete(W8OptionsTextEditor*, unsigned char cancelled)
{
    int selected_slot = m_current * 5 + m_selection.m_selectedIndex;
    (*m_rows.GetAt(m_editing_row))->m_editing = false;
    if (cancelled != 0) {
        (*m_rows.GetAt(m_selection.m_selectedIndex))->Invalidate(false);
        return;
    }

    W8SaveSlot* slot = *g_options_screen->m_save_slots.GetAt(selected_slot);
    wcscpy(m_previous_name, slot->name);
    Get16BitStringFromField(0, slot->name);
    if (wcslen(slot->name) == 0) {
        wcscpy(slot->name, m_previous_name);
        g_options_screen->ShowNotification(this, false, 0x82a, 4);
        return;
    }

    W8OptionsSaveRow* row = *m_rows.GetAt(m_selection.m_selectedIndex);
    row->SetSave(slot);
    if (m_current == 0 && m_selection.m_selectedIndex == 0) {
        if (SaveSlotFileExists(ConvertWideStringToString(slot->name)) == 0) {
            SaveSelectedSave();
        } else {
            g_options_screen->ShowNotification(this, true, 0x829, 2);
        }
    } else {
        g_options_screen->ShowNotification(this, true, 0x829, 3);
    }
}

// FUNCTION: WIZ8 0x005ab1b0
void W8OptionsSaveLoadPanel::OnEditSaveName(W8OptionsSaveRow*)
{
    int selected_slot = m_current * 5 + m_selection.m_selectedIndex;
    g_options_screen->BeginSaveNameEdit(
        this, m_selection.m_selectedIndex,
        (*g_options_screen->m_save_slots.GetAt(selected_slot))->name);
    m_editing_row = m_selection.m_selectedIndex;
    (*m_rows.GetAt(m_editing_row))->m_editing = true;
}

// FUNCTION: WIZ8 0x005ab230
void W8OptionsSaveLoadPanel::OnActivateSave(W8OptionsSaveRow*)
{
    OnPrimary(m_action_button);
}

// FUNCTION: WIZ8 0x005ab250
void W8OptionsSaveLoadPanel::OnSelectionChanged(W8ControlSelection*, int)
{
    if (g_options_screen->m_text_editor != 0) {
        W8OptionsTextEditor* editor = g_options_screen->m_text_editor;
        if (editor->m_listener != 0) {
            editor->m_listener->OnTextEditComplete(editor, 1);
        }
        delete editor;
        g_options_screen->m_text_editor = 0;
    }
    if (m_panel == 12 && m_current == 0 && m_selection.m_selectedIndex == 0) {
        OnEditSaveName(m_rows[0]);
    }
}

// FUNCTION: WIZ8 0x005ab2c0
void W8OptionsSaveLoadPanel::DeleteSelectedSave()
{
    int selected_slot = m_current * 5 + 1 + m_selection.m_selectedIndex;
    W8SaveSlot* slot = *g_options_screen->m_save_slots.GetAt(selected_slot);
    char path[260];
    sprintf(path, "%s\\%S.%s", "Saves", slot->name, g_save_extension);
    if (DeleteFileA(path) == 0) {
        g_options_screen->ShowNotification(this, false, 0x82f, 0);
        return;
    }

    g_options_screen->m_save_slots.RemoveAt(selected_slot);
    if (g_options_screen->m_save_slots.GetCount() == 1) {
        selected_slot = -1;
        if (m_panel == 11) {
            m_delete_button->SetEnabled(false);
            m_action_button->SetEnabled(false);
        }
    } else if (selected_slot >= g_options_screen->m_save_slots.GetCount()) {
        --selected_slot;
        if (m_selection.m_selectedIndex == 0) {
            --m_current;
        }
    }

    if (g_options_screen->m_selected_panel == 4) {
        g_options_screen->m_panel[4]->m_page_count =
            (g_options_screen->m_save_slots.GetCount() - 2) / 5 + 1;
    } else if (g_options_screen->m_selected_panel == 5) {
        g_options_screen->m_panel[5]->m_page_count =
            (g_options_screen->m_save_slots.GetCount() - 1) / 5 + 1;
    }
    g_options_screen->m_menu_set->UpdateMenuSet();
    SetCurrent(m_current);
    m_selection.SetSelected(selected_slot == -1 ? -1 : (selected_slot - 1) % 5);
}

// FUNCTION: WIZ8 0x005ab460
void W8OptionsSaveLoadPanel::LoadSelectedSave()
{
    int selected_slot = m_current * 5 + 1 + m_selection.m_selectedIndex;
    W8SaveSlot* slot = *g_options_screen->m_save_slots.GetAt(selected_slot);
    if (slot->version_major + slot->version_minor * 0.1f + slot->version_patch * 0.01f <= 1.24f) {
        SetLastSaveName(slot->name);
        if (g_status.game_started) {
            ClearHeldItemDisplay();
        }
        RequestScreenTransition();
        g_pending_screen_state.mode = 1;
        g_pending_screen_state.parameter = slot->level_id;
        strcpy(g_pending_screen_state.name, ConvertWideStringToString(slot->name));
        SetPendingScreenState(W8_SCREEN_PLEASE_WAIT);
    }
}

// FUNCTION: WIZ8 0x005ab590
void W8OptionsSaveLoadPanel::SaveSelectedSave()
{
    int selected_slot = m_current * 5 + m_selection.m_selectedIndex;
    W8SaveSlot* slot = *g_options_screen->m_save_slots.GetAt(selected_slot);
    if (wcslen(slot->name) == 0) {
        g_options_screen->ShowNotification(this, false, 0x82a, 0);
        return;
    }

    char path[260];
    sprintf(path, "%s\\%S.%s", "Saves", slot->name, g_save_extension);
    if (FileExists(path) != 0 && DeleteFileA(path) == 0) {
        g_options_screen->ShowNotification(this, false, 0x82e, 0);
        return;
    }
    SetLastSaveName(slot->name);
    RequestScreenTransition();
    g_pending_screen_state.mode = 2;
    strcpy(g_pending_screen_state.name, ConvertWideStringToString(slot->name));
    g_pending_screen_state.parameter_3 =
        new W8SaveScreenshot(g_options_screen->m_save_slots[0]->screenshot);
    SetPendingScreenState(W8_SCREEN_PLEASE_WAIT);
}

W8OptionsButton::W8OptionsButton(Controls* owner, int left, int top, int right, int bottom,
                                 const wchar_t* text)
    : W8TextControl(owner, 0xffffffff, left, top, right, bottom, -1, -1, -1, -1, -1, -1, -1)
{
    m_textBuffer.SetLayoutMode(g_W8TextBufferAlignTop);
    m_textBuffer.SetText(text, g_options_detail_font);
}

W8OptionsKeyButton::W8OptionsKeyButton(Controls* owner, int top, W8MGSCommand primary_binding,
                                       W8MGSCommand secondary_binding)
    : W8OptionsButton(owner, 100, top, 0x15e, top + 22, &g_empty_wide_string),
      m_primary_binding(primary_binding), m_secondary_binding(secondary_binding)
{
    AddLayoutFlags(g_W8TextControlLayoutLatchedImage | g_W8TextControlLayoutToggle);
    m_textBuffer.SetLayoutMode(g_W8TextBufferNoWrap | g_W8TextBufferAlignRight |
                               g_W8TextBufferAlignTop);
}

/* Sprite indices within catalog object 0xf9, frame 0. */
enum W8OptionsSaveRowSprite {
    W8_OPTIONS_SAVE_ROW_SAVE_NORMAL = 0,
    W8_OPTIONS_SAVE_ROW_SAVE_PRESSED = 1,
    W8_OPTIONS_SAVE_ROW_LOAD_NORMAL = 2,
    W8_OPTIONS_SAVE_ROW_LOAD_PRESSED = 3,
    W8_OPTIONS_SAVE_ROW_DISABLED = 4
};

W8OptionsSaveRow::W8OptionsSaveRow(Controls* owner, int top, unsigned char save_mode)
    : W8TextControl(owner, 0xffffffff, 10, top, 0, 0, 0xf9, 0,
                    save_mode ? W8_OPTIONS_SAVE_ROW_SAVE_NORMAL : W8_OPTIONS_SAVE_ROW_LOAD_NORMAL,
                    save_mode ? W8_OPTIONS_SAVE_ROW_SAVE_PRESSED : W8_OPTIONS_SAVE_ROW_LOAD_PRESSED,
                    -1, -1, W8_OPTIONS_SAVE_ROW_DISABLED),
      m_save_mode(save_mode), m_editing(0), m_save(0), m_save_listener(0)
{
    AddLayoutFlags(g_W8TextControlLayoutLatchedImage | g_W8TextControlLayoutToggle);
}

// FUNCTION: WIZ8 0x005a77b0
void W8OptionsSaveRow::Redraw(bool full_redraw)
{
    if (!m_active || (!full_redraw && !m_dirty)) {
        return;
    }
    int x = m_pPanel->m_bounds.left + m_left;
    int y = m_pPanel->m_bounds.top + m_top;
    if (OptionsSaveRowStretch() == 0) {
        W8TextControl::Redraw(full_redraw);
    } else {
        /* Mod (wide windows): the same image W8TextControl::Redraw would pick
           (the row has no text and no alternative images), but stretched; and
           the mouse region covers the full length of the row. */
        int sprite;
        if (!m_enabled) {
            sprite = m_disabledSprite;
        } else if ((m_stateFlags & g_W8TextControlStatePressed) != 0) {
            sprite = m_pressedSprite;
        } else {
            sprite = m_normalSprite;
        }
        short width;
        short height;
        GetCatalogImageSize(m_imageObject, m_imageFrame, sprite, &width, &height);
        DrawCatalogImageWide(m_imageObject, m_imageFrame, sprite, x, y,
                             static_cast<unsigned short>(width), W8_OPTIONS_SAVE_ROW_SLICE_FROM,
                             W8_OPTIONS_SAVE_ROW_SLICE_TO, true);
        StretchWidgetRegion(this);
        m_dirty = false;
    }
    if (!m_enabled || m_save == 0) {
        return;
    }
    if (m_save->screenshot.capture_result == 0) {
        DrawCatalogImage(FRAME_BUFFER, 0xf6, 0, 0, x + 6, y + 6, VO_BLT_SRCTRANSPARENCY, 0);
    } else {
        srColorSurface* portrait = new srColorSurface(srPixelConvert::SURFACE_ARGB1555,
                                                      m_save->screenshot.pixels, 0x50, 0x3c, 0xa0);
        DrawColorSurface(portrait, x + 6, y + 6);
        portrait->release();
    }

    const wchar_t* level_name = m_save->level_id == 0x38
                                    ? g_default_level
                                    : gppStringList[g_level_name_indices[m_save->level_id]];
    wchar_t timestamp[32];
    swprintf(timestamp, L"%d-%2.2d-%2.2d %2d:%2.2d", m_save->timestamp.wYear,
             m_save->timestamp.wMonth, m_save->timestamp.wDay, m_save->timestamp.wHour,
             m_save->timestamp.wMinute);

    /* Mod: in a stretched row the texts are drawn from the right canvas edge (window
       origin 2*ox) with x smaller by the stretch - on screen they stay in the same place, while clipping
       at x + 0x156 (as in retail) falls on the end of the stretched row rather than on
       canvas 640. Without stretching - as in retail. */
    const int stretch = OptionsSaveRowStretch();
    CanvasOriginScope text_scope(stretch != 0 ? 2 * g_canvas_ox : CanvasDrawOrigin());
    /* Retail 0x5a78eb: the row texts are clipped at x + 0x156. */
    SetFontDestBuffer(FRAME_BUFFER, 0, 0, x + 0x156, 0x1e0, 0);
    SetFont(g_wiz_text_font_secondary);
    int text_x = x + 0x5e - stretch;
    if (m_save->version_major + m_save->version_minor * 0.1f + m_save->version_patch * 0.01f <=
        1.24f) {
        gprintf(text_x, y + 9, L"%s", level_name);
        gprintf(text_x, y + 0x16, L"%s %3d, %2d:%2.2d", gppStringList[0x826],
                m_save->game_time_days, m_save->game_time_ms / 3600000,
                (m_save->game_time_ms / 60000) % 60);
        gprintf(text_x, y + 0x23, L"%s", timestamp);
    } else {
        gprintf(text_x, y + 9, L"%s", gppStringList[0x830]);
        gprintf(text_x, y + 0x16, L"%s", gppStringList[0x831]);
    }
    gprintf(text_x, y + 0x34, L"%s", m_save->name);
    if (m_save->iron_man) {
        /* Retail 0x5a7a92: the label is right-aligned to x + 0x156. */
        const int width = StringPixLength(gppStringList[0x827], g_wiz_text_font_secondary);
        gprintf(x + 0x156 - width, y + 9, L"%s", gppStringList[0x827]);
    }
    SetFontDestBuffer(FRAME_BUFFER, 0, 0, 0x280, 0x1e0, 0); /* retail 0x5a7ad2 */
}

// FUNCTION: WIZ8 0x005a7b10
void W8OptionsSaveRow::OnLeftButtonUp(int event)
{
    if (m_active && m_enabled && !m_editing &&
        (m_stateFlags & g_W8TextControlStateSecondary) != 0) {
        POINT point;
        SGPMouseGetPos(&point);
        point.x -= OptionsWidgetAnchorShift(this); /* mod: the row is at the left canvas edge */
        point.x -= m_pPanel->m_bounds.left + m_left;
        point.y -= m_pPanel->m_bounds.top + m_top;
        if (m_save_mode != 0 && point.x >= 0x5c && point.y >= 0x33 && m_save_listener != 0) {
            m_save_listener->OnEditSaveName(this);
        }
    }
    W8TextControl::OnLeftButtonUp(event);
}

// FUNCTION: WIZ8 0x005a7bb0
void W8OptionsSaveRow::OnLeftButtonDoubleClick(int event)
{
    if (m_active && m_enabled && !m_editing &&
        (m_stateFlags & g_W8TextControlStateSecondary) != 0 && m_save_listener != 0) {
        m_save_listener->OnActivateSave(this);
    }
    W8TextControl::OnLeftButtonDoubleClick(event);
}

// FUNCTION: WIZ8 0x005a9820
void W8OptionsScreen::BeginSaveNameEdit(W8OptionsTextEditor::Listener* listener, int row,
                                        const wchar_t* text)
{
    W8OptionsTextEditor* editor = new W8OptionsTextEditor;
    InitTextInputModeWithScheme(1);
    /* Mod (wide windows): the name field is longer by the row stretch. Its left edge
       on the canvas is the same (under the row text), so the field is anchored to the right
       edge, and its game x is reduced by 2*ox - so it stays entirely within
       game 0..640 (drawing is clipped beyond that). */
    int stretch = OptionsSaveRowStretch();
    AddTextInputField(0x16b - stretch, row * 0x49 + 0x46, 0xfa + stretch, 0xc, 0x7f, text, 0x3b,
                      0xf, 1);
    if (stretch != 0) {
        SetTextInputFieldLayoutAnchor(0, W8_ANCHOR_RIGHT);
    }
    SetActiveField(0);
    m_text_editor = editor;
    editor->m_listener = listener;
}

// FUNCTION: WIZ8 0x005a7c70
void W8OptionsButton::Redraw(bool full_redraw)
{
    if (m_active && (full_redraw || m_dirty) && m_textBuffer.HasBuffer()) {
        if (m_enabled) {
            int font_state;
            if ((m_stateFlags & g_W8TextControlStatePressed) != 0) {
                font_state = 13;
            } else {
                font_state = m_alternateTextEnabled ? 14 : -1;
            }
            m_textBuffer.SetFontStateIndex(font_state);
        }
        m_textBuffer.RenderToTarget(0, full_redraw, FRAME_BUFFER);
        m_dirty = false;
    }
}

// FUNCTION: WIZ8 0x005a7ce0
void W8OptionsButton::OnMouseEnter(int event)
{
    W8TextControl::OnMouseEnter(event);
    SetAlternateTextEnabled(true);
    Invalidate(static_cast<unsigned char>(event));
}

// FUNCTION: WIZ8 0x005a7d10
void W8OptionsButton::OnMouseLeave(int event)
{
    W8TextControl::OnMouseLeave(event);
    SetAlternateTextEnabled(false);
    Invalidate(static_cast<unsigned char>(event));
}

// FUNCTION: WIZ8 0x005a7db0
void W8OptionsKeyButton::SetKey(unsigned short key)
{
    m_textBuffer.SetFontStateIndex(m_alternateTextEnabled ? 14 : -1);
    if (key == VK_ESCAPE) {
        Invalidate(false);
        return;
    }

    MGSKeyBinding* binding =
        g_mgs_keyboard->GetBinding(g_mgs_keyboard->FindBinding(m_primary_binding));
    if (binding != 0) {
        binding->key = key;
    }
    if (m_secondary_binding != W8_MGS_COMMAND_NONE) {
        binding = g_mgs_keyboard->GetBinding(g_mgs_keyboard->FindBinding(m_secondary_binding));
        if (binding != 0) {
            binding->key = key;
        }
    }
    SetKeyText(key);
    Invalidate(true);
}

// FUNCTION: WIZ8 0x005a7e40
void W8OptionsKeyButton::SetKeyText(unsigned short key)
{
    wchar_t character[2] = {0, 0};
    int index = 0;
    const wchar_t* text;

    if (g_options_key_names[0].key != key) {
        W8OptionsKeyName* name = g_options_key_names;
        do {
            unsigned short current = name->key;
            ++index;
            ++name;
            if (current == 0) {
                goto translate_key;
            }
        } while (name->key != key);
    }

    text = gppStringList[g_options_key_names[index].label];
    if (text == 0) {
    translate_key:
        character[0] = TranslateKeyToCharacter(key, 0);
        if (character[0] == 0) {
            m_textBuffer.SetText(gppStringList[0x86d], g_options_detail_font);
            return;
        }
        character[0] = static_cast<wchar_t>(toupper(character[0]));
        text = character;
    }

    if (m_secondary_binding != W8_MGS_COMMAND_NONE && key != 0) {
        text = FormatWideString(L"(%s) %s", gppStringList[0x89d], text);
    }
    m_textBuffer.SetText(text, g_options_detail_font);
}

// FUNCTION: WIZ8 0x005ab930
void W8OptionsKeyboardPanel::Populate()
{
    m_selection.m_selectionListener = this;

    W8OptionsKeyboardPage& page = g_options_keyboard_pages[m_panel - 6];
    W8ControlsRect title_bounds = {m_bounds.left + 30, m_bounds.top + m_content_top,
                                   m_bounds.right - 30, m_bounds.top + m_content_top + 22};
    W8TextBuffer* title =
        new W8TextBuffer(&title_bounds, gppStringList[page.title], g_options_detail_font,
                         g_W8TextBufferAlignTop | g_W8TextBufferAlignCenter, 4);
    m_text_buffers.Add(title);
    m_content_top += 44;

    int first = 0;
    while (g_options_key_rows[first].primary_binding != page.first_binding) {
        ++first;
    }

    W8OptionsKeyRow* row = g_options_key_rows + first;
    for (;;) {
        W8ControlsRect label_bounds = {m_bounds.left + 20, m_bounds.top + m_content_top,
                                       m_bounds.right, m_bounds.top + m_content_top + 22};
        W8TextBuffer* label =
            new W8TextBuffer(&label_bounds, gppStringList[row->label], g_options_detail_font,
                             g_W8TextBufferAlignTop | g_W8TextBufferAlignLeft, 4);
        m_text_buffers.Add(label);

        W8OptionsKeyButton* button = new W8OptionsKeyButton(
            this, m_content_top, row->primary_binding, row->secondary_binding);
        AnchorOptionsWidget(button, W8_ANCHOR_RIGHT); /* mod: key button at the right edge */
        m_selection.AddEntry(button);
        m_content_top += 22;

        W8MGSCommand binding = row->primary_binding;
        ++row;
        if (binding == page.last_binding) {
            wchar_t* reset_text = gppStringList[0x833];
            short text_width = StringPixLength(reset_text, g_options_detail_font);
            int left = (m_bounds.right - (text_width + 20) - m_bounds.left) / 2;
            W8OptionsButton* reset =
                new W8OptionsButton(this, left, 0x18e, left + text_width + 20, 0x1a4, reset_text);
            reset->m_listener = this;
            AnchorOptionsWidget(reset, W8_ANCHOR_CENTER); /* mod: centered in the panel */
            return;
        }
    }
}

// FUNCTION: WIZ8 0x005abce0
void W8OptionsKeyboardPanel::Invalidate(const W8ControlsRect* bounds)
{
    Controls::Invalidate(bounds);
    g_options_screen->m_menu_set->Invalidate(0);
}

// FUNCTION: WIZ8 0x005abd00
void W8OptionsKeyboardPanel::OnPrimary(W8TextControl*)
{
    g_options_screen->ShowNotification(this, true, 0x834, 0);
}

// FUNCTION: WIZ8 0x005abd30
void W8OptionsKeyboardPanel::OnSelectionChanged(W8ControlSelection*, int selected)
{
    if (selected == -1) {
        m_captured_button = 0;
        g_options_screen->m_key_capture = 0;
        return;
    }

    W8Widget** control = m_controls.GetAt(selected);
    m_captured_button = static_cast<W8OptionsKeyButton*>(*control);
    g_options_screen->m_key_capture = this;
}

void W8OptionsKeyboardPanel::RefreshBindingLabels()
{
    int count = m_controls.GetCount() - 1;
    for (int index = 0; index < count; ++index) {
        W8OptionsKeyButton* button = static_cast<W8OptionsKeyButton*>(ControlAt(index));
        MGSKeyBinding* binding =
            g_mgs_keyboard->GetBinding(g_mgs_keyboard->FindBinding(button->m_primary_binding));
        if (binding != 0) {
            button->SetKeyText(binding->key);
        }
    }
}

// FUNCTION: WIZ8 0x005abd90
void W8OptionsKeyboardPanel::SetActive(bool active)
{
    W8OptionsPanel::SetActive(active);
    if (active) {
        RefreshBindingLabels();
    }
    m_selection.SetSelected(-1);
}

// FUNCTION: WIZ8 0x005abe20
unsigned char W8OptionsKeyboardPanel::OnKey(unsigned short key, unsigned short modifiers)
{
    if (modifiers != 0) {
        return 0;
    }
    if (key != VK_ESCAPE) {
        ClearDuplicateBinding(key);
    }
    m_captured_button->SetKey(key);
    m_selection.SetSelected(-1);
    return 1;
}

// FUNCTION: WIZ8 0x005abe60
void W8OptionsKeyboardPanel::OnDialogClosed(bool accepted, int)
{
    m_selection.SetSelected(-1);
    if (accepted) {
        ResetMGSKeyboardBindings();
        RefreshBindingLabels();
    }
}

// FUNCTION: WIZ8 0x005abed0
void W8OptionsKeyboardPanel::ClearDuplicateBinding(unsigned short key)
{
    int row_index = 0;
    if (g_options_key_rows[0].primary_binding == W8_MGS_COMMAND_NONE) {
        return;
    }

    W8OptionsKeyRow* row = g_options_key_rows;
    for (;;) {
        MGSKeyBinding* binding =
            g_mgs_keyboard->GetBinding(g_mgs_keyboard->FindBinding(row->primary_binding));
        if (binding != 0 && binding->key == key) {
            binding->key = 0;
            if (g_options_key_rows[row_index].secondary_binding != W8_MGS_COMMAND_NONE) {
                binding = g_mgs_keyboard->GetBinding(
                    g_mgs_keyboard->FindBinding(g_options_key_rows[row_index].secondary_binding));
                if (binding != 0) {
                    binding->key = 0;
                }
            }

            RefreshBindingLabels();
            return;
        }

        ++row_index;
        ++row;
        if (row->primary_binding == W8_MGS_COMMAND_NONE) {
            return;
        }
    }
}

// FUNCTION: WIZ8 0x005abfc0
W8OptionsPanel* CreateOptionsPanel(int panel, unsigned char* compact,
                                   unsigned char* hide_navigation)
{
    switch (panel) {
    case 0:
        return new W8OptionsGamePanel();
    case 1:
        return new W8OptionsMousePanel();
    case 2:
        return new W8OptionsInterfacePanel();
    case 3:
        return new W8OptionsAudioPanel();
    case 4:
        return new W8OptionsGraphicsPanel();
    case 5:
        return new W8OptionsAdvancedGraphicsPanel();
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
        return new W8OptionsKeyboardPanel(panel);
    case 11:
        if (g_status.game_started && g_status.iron_man) {
            *hide_navigation = 1;
            return new W8OptionsUnavailablePanel(0x82c);
        }
        *compact = 1;
        return new W8OptionsSaveLoadPanel(11);
    case 12:
        if (g_status.game_started) {
            if (g_status.iron_man) {
                *hide_navigation = 1;
                return new W8OptionsUnavailablePanel(0x82c);
            }
            if (gXStatus.fCombatMode) {
                *hide_navigation = 1;
                return new W8OptionsUnavailablePanel(0x82b);
            }
        }
        *compact = 1;
        return new W8OptionsSaveLoadPanel(12);
    }
    return 0;
}

// FUNCTION: WIZ8 0x005a9fd0
void W8OptionsGamePanel::Populate()
{
    m_content_top += 22;
    W8OptionsSlider* slider = AddSlider(0x805, &g_options_values.combat_speed, false);
    slider->m_minimumPosition = 0.0f;
    slider->m_maximumPosition = 1.0f;
    slider->UpdatePixelPosition();
    m_content_top += 22;
    slider = AddSlider(0x80c, &g_options_values.text_display_delay_ms, false);
    slider->m_minimumPosition = 0.0f;
    slider->m_maximumPosition = 5000.0f;
    slider->UpdatePixelPosition();
    m_content_top += 22;
    /* Mod (2026-10-16, replacing the Wiz8Fast utility): monster movement
       speed in combat up to 10x instead of 5x. Wiz8Fast did exactly this - wrote to the
       config a value above the slider maximum. The slider has no "1x/3x" marks
       (the image with marks is designed for 1..5). */
    slider = AddSlider(0x802, &g_options_values.monster_movement_speed, false);
    slider->m_minimumPosition = 1.0f;
    slider->m_maximumPosition = W8_MAX_MONSTER_SPEED;
    slider->UpdatePixelPosition();
    m_content_top += 22;
    AddChoices(0x7f7, 3, g_options_difficulty_labels, &g_options_values.difficulty);
    AddChoices(0x7f4, 2, g_options_combat_mode_labels, &g_options_values.continuous_combat);
    AddChoices(0x7fd, 3, g_options_camera_rotation_labels, &g_options_values.camera_auto_rotation);
}

// FUNCTION: WIZ8 0x005aa0c0
void W8OptionsMousePanel::Populate()
{
    m_content_top += 22;
    AddCheckbox(0x7f0, &g_options_values.mouselook_toggle);
    AddCheckbox(0x7f1, &g_options_values.mouselook_smoothing);
    AddCheckbox(0x7f2, &g_options_values.invert_mouse_y);
    AddCheckbox(0x801, &g_options_values.stop_movement_for_events);
    AddCheckbox(0x809, &g_options_values.autotarget_spells);
    AddCheckbox(0x804, &g_options_values.auto_advance_character);
    AddCheckbox(0x808, &g_options_values.autoswap_weapons);
    if (!g_status.game_started || !g_status.iron_man) {
        AddCheckbox(0x80d, &g_options_values.auto_save);
    }
}

// FUNCTION: WIZ8 0x005aa170
void W8OptionsInterfacePanel::Populate()
{
    m_content_top += 22;
    m_tooltip_delay = AddSlider(0x80e, 0, false);
    m_tooltip_delay->m_minimumPosition = 0.0f;
    m_tooltip_delay->m_maximumPosition = 1200.0f;
    m_tooltip_delay->UpdatePixelPosition();
    m_tooltip_delay->m_position = static_cast<float>(g_settings.tooltip_delay_ms);
    m_tooltip_delay->UpdatePixelPosition();
    m_tooltip_delay->m_listener = this;
    W8TextControl* button = AddChoiceButton(0x80f);
    button->EnableRegionHelp(0x810);
    button->m_listener = this;
    if (!g_settings.tooltips_enabled) {
        button->EnableSecondaryState(false);
        m_tooltip_delay->SetEnabled(false);
    }
    m_content_top += 22;
    AddCheckbox(0x80b, &g_options_values.simplified_npc_interaction);
    AddCheckbox(0x807, &g_options_values.verbose_combat_messages);
    AddCheckbox(0x7f3, &g_options_values.ctrl_right_click_info);
    AddCheckbox(0x7fc, &g_options_values.skill_increase_messages);
    AddCheckbox(0x806, &g_options_values.numeric_hit_points);
    AddCheckbox(0x80a, &g_options_values.autoscroll_combat_messages);
}

// FUNCTION: WIZ8 0x005aa2a0
void W8OptionsInterfacePanel::OnDragEnd(W8HorizontalRangeThumb* thumb)
{
    g_settings.tooltip_delay_ms = static_cast<int>(thumb->m_position);
    SetFastHelpDelay(static_cast<short>(g_settings.tooltip_delay_ms));
    SetRegionHelpDelay(0);
}

// FUNCTION: WIZ8 0x005aa2d0
void W8OptionsInterfacePanel::OnPrimary(W8TextControl* control)
{
    g_settings.tooltips_enabled = (control->m_stateFlags & g_W8TextControlStateSecondary) == 0;
    m_tooltip_delay->SetEnabled(g_settings.tooltips_enabled);
    m_tooltip_delay->Invalidate(false);
}

// FUNCTION: WIZ8 0x005aa480
void W8OptionsAudioPanel::Populate()
{
    for (int index = 0; index < 4; ++index) {
        int volume;
        bool muted = false;

        m_content_top += 22;
        W8OptionsSlider* slider = AddSlider(g_options_audio_labels[index], 0, false);
        switch (index) {
        case 0:
            if (IsMusicMuted()) {
                volume = g_settings.muted_music_volume;
                muted = true;
            } else {
                volume = g_settings.music_volume;
            }
            break;
        case 1:
            if (IsSoundEffectsMuted()) {
                volume = g_settings.muted_sound_effects_volume;
                muted = true;
            } else {
                volume = g_settings.sound_effects_volume;
            }
            break;
        case 2:
            if (GetRenderOptionState(W8_RENDER_OPTION_FOOTSTEP_SOUND) == 0) {
                muted = true;
            }
            volume = g_settings.footstep_volume;
            break;
        case 3:
            if (IsVoiceMuted()) {
                volume = g_settings.muted_voice_volume;
                muted = true;
            } else {
                volume = g_settings.voice_volume;
            }
            break;
        }
        slider->m_minimumPosition = 0.0f;
        slider->m_maximumPosition = 127.0f;
        slider->UpdatePixelPosition();
        slider->m_position = static_cast<float>(volume);
        slider->UpdatePixelPosition();
        slider->m_listener = this;
        m_sliders[index] = slider;

        m_mute_buttons[index] = AddChoiceButton(0x822);
        m_mute_buttons[index]->m_listener = this;
        if (muted) {
            m_sliders[index]->SetEnabled(false);
            m_mute_buttons[index]->EnableSecondaryState(false);
        }
    }
    m_content_top += 22;
    AddCheckbox(0x823, &g_options_values.pc_confirmations);
    AddCheckbox(0x824, &g_options_values.pc_subtitles);
}

// FUNCTION: WIZ8 0x005aa620
void W8OptionsAudioPanel::OnDrag(W8HorizontalRangeThumb* thumb)
{
    int index;
    for (index = 0; index < 4; ++index) {
        if (thumb == m_sliders[index]) {
            break;
        }
    }
    switch (index) {
    case 0:
        SetMusicVolume(static_cast<unsigned char>(thumb->m_position));
        return;
    case 1:
        SetSoundEffectsVolume(static_cast<unsigned char>(thumb->m_position));
        return;
    case 2:
        g_settings.footstep_volume = static_cast<unsigned char>(thumb->m_position);
        return;
    case 3:
        g_settings.voice_volume = static_cast<unsigned char>(thumb->m_position);
        return;
    }
}

// FUNCTION: WIZ8 0x005aa6a0
void W8OptionsAudioPanel::OnDragEnd(W8HorizontalRangeThumb* thumb)
{
    int index;
    for (index = 0; index < 4; ++index) {
        if (thumb == m_sliders[index]) {
            break;
        }
    }
    if (index == 4) {
        index = -1;
    }
    switch (index) {
    case 1:
        SoundPlay("Data\\Sound\\Misc\\Interface Swoosh 01.wav", 0);
        break;
    case 2:
        PlayFootstep(W8_FOOTSTEP_SURFACE_MEDIUM_ROOM, W8_FOOTSTEP_MATERIAL_GRAVEL,
                     W8_FOOTSTEP_KIND_STEP);
        break;
    case 3: {
        SOUNDPARMS options;
        memset(&options, -1, sizeof(options));
        options.uiVolume = g_settings.voice_volume;
        SoundPlay("Data\\Sound\\Misc\\Interface Vox 01.wav", &options);
        break;
    }
    }
}

// FUNCTION: WIZ8 0x005aa730
void W8OptionsAudioPanel::OnPrimary(W8TextControl* control)
{
    int index;
    for (index = 0; index < 4; ++index) {
        if (control == m_mute_buttons[index]) {
            break;
        }
    }
    if (index == 4) {
        index = -1;
    }
    unsigned char muted =
        static_cast<unsigned char>(control->m_stateFlags) & g_W8TextControlStateSecondary;
    switch (index) {
    case 0:
        SetMusicMuted(muted);
        break;
    case 1:
        SetSoundEffectsMuted(muted);
        break;
    case 2:
        if (muted != 0) {
            DisableRenderOption(W8_RENDER_OPTION_FOOTSTEP_SOUND);
        } else {
            EnableRenderOption(W8_RENDER_OPTION_FOOTSTEP_SOUND);
        }
        break;
    case 3:
        SetVoiceMuted(muted);
        break;
    }
    m_sliders[index]->SetEnabled(muted == 0);
    m_sliders[index]->Invalidate(false);
}

/* Mod: draw distance slider (one per settings screen). */
static W8OptionsSlider* g_view_distance_slider;
static float g_view_distance_slider_value;

// FUNCTION: WIZ8 0x005aa310
void W8OptionsGraphicsPanel::Populate()
{
    m_content_top += 22;
    W8OptionsSlider* slider = AddSlider(0x811, &g_options_values.gamma, false);
    slider->m_minimumPosition = 0.1f;
    slider->m_maximumPosition = 1.9f;
    slider->UpdatePixelPosition();
    slider->m_listener = this;
    slider->SetEnabled(GetRendererModeByte());
    m_content_top += 22;
    /* Mod (2026-10-16): draw distance, 1.0 - as in the original. */
    g_view_distance_slider_value = DisplayViewDistanceScale();
    g_view_distance_slider =
        AddSliderText(RussianOr(L"View distance", L"Дальность прорисовки"),
                      &g_view_distance_slider_value);
    g_view_distance_slider->m_minimumPosition = 1.0f;
    g_view_distance_slider->m_maximumPosition = W8_MAX_VIEW_DISTANCE;
    g_view_distance_slider->UpdatePixelPosition();
    g_view_distance_slider->m_listener = this;
    m_content_top += 22;
    AddCheckbox(0x816, &g_options_values.render_options[4]);
    AddCheckbox(0x819, &g_options_values.smooth_monster_animations);
    AddCheckbox(0x81a, &g_options_values.smooth_world_animations);
    AddCheckbox(0x81b, &g_options_values.render_options[6]);
    AddCheckbox(0x818, &g_options_values.monster_shadows);
    AddCheckbox(0x817, &g_options_values.render_options[5]);
    AddCheckbox(0x81d, &g_options_values.render_options[8]);
}

// FUNCTION: WIZ8 0x005aa3f0
void W8OptionsGraphicsPanel::OnDrag(W8HorizontalRangeThumb* thumb)
{
    if (thumb != 0 && thumb == g_view_distance_slider) {
        return; /* mod: draw distance is applied on release */
    }
    SetDisplayGamma(g_options_values.gamma);
}

// FUNCTION: WIZ8 0x005aa400
void W8OptionsGraphicsPanel::OnDragEnd(W8HorizontalRangeThumb* thumb)
{
    /* Mod: the new draw distance - to the ini and immediately to the current level's camera. */
    if (thumb != 0 && thumb == g_view_distance_slider) {
        /* Slider position: on a click on the track without dragging there was
           no OnMouseMove and *m_value is still the old one. */
        g_view_distance_slider_value = g_view_distance_slider->m_position;
        const float old_scale = DisplayViewDistanceScale();
        DisplaySetViewDistanceScale(g_view_distance_slider_value);
        if (g_world != 0) {
            WorldSetFarClip(g_world, g_world->view_distance);
            /* terrain draw range (render_range) was set at level load with the
               old multiplier - rescale it in the same proportion */
            const float new_scale = DisplayViewDistanceScale();
            const float range = WorldGetRenderRange(g_world);
            if (old_scale > 0.0f && range > 0.0f) {
                WorldSetRenderRange(g_world, range * new_scale / old_scale);
            }
        }
    }
}

// FUNCTION: WIZ8 0x005aa410
void W8OptionsAdvancedGraphicsPanel::Populate()
{
    m_content_top += 22;
    AddCheckbox(0x815, &g_options_values.render_options[3]);
    AddCheckbox(0x814, &g_options_values.render_options[2]);
    AddCheckbox(0x812, &g_options_values.render_options[0]);
    AddCheckbox(0x81c, &g_options_values.render_options[7]);
    AddCheckbox(0x813, &g_options_values.render_options[1]);
    AddDisplaySettings(); /* mod: display mode and resolution */
}

// FUNCTION: WIZ8 0x005a9ea0
void W8OptionsUnavailablePanel::Populate()
{
    m_content_top += 44;
    W8ControlsRect bounds = {m_bounds.left + 30, m_bounds.top + m_content_top, m_bounds.right - 30,
                             m_bounds.bottom};
    W8TextBuffer* text = new W8TextBuffer(&bounds, gppStringList[m_message], g_options_detail_font,
                                          g_W8TextBufferAlignTop | g_W8TextBufferAlignCenter, 4);
    m_text_buffers.Add(text);
    (*m_text_buffers.GetAt(0))->SetLineHeight(22);
}

// FUNCTION: WIZ8 0x005a9090
W8OptionsScreen::W8OptionsScreen()
    : m_redraw_pending(1), m_modal_closing(0), m_selected_panel(-1), m_controls(0), m_menu_set(0),
      m_menu_selection(0), m_active_modal(0), m_text_editor(0), m_key_capture(0)
{
    for (int index = 0; index < 8; ++index) {
        m_panel[index] = 0;
    }
    W8SaveSlot* current = new W8SaveSlot;
    m_save_slots.Add(current);
    if (g_status.game_started) {
        FillCurrentSaveSlot(current);
    }
    EnumerateSaveSlots(&m_save_slots);
}

// FUNCTION: WIZ8 0x005a98b0
void W8OptionsScreen::CreateControls()
{
    m_controls = new Controls();
    m_menu_selection = new W8ControlSelection();
    m_controls->AcquireRegionSet(&g_options_menu_region_set);
    for (int index = 0; index < 8; ++index) {
        W8OptionsMenuButton* button =
            new W8OptionsMenuButton(m_controls, &g_options_menu_rows[index]);
        if (index == 7) {
            button->m_primaryActivationCallback = RequestScreenTransition;
        } else if (index == 6) {
            button->m_listener = this;
        } else {
            m_menu_selection->AddEntry(button);
            if (index == 5) {
                button->SetEnabled(g_status.game_started);
            } else if (index == 4) {
                button->SetEnabled(m_save_slots.GetCount() > 1);
            }
        }
    }
    m_menu_selection->m_selectionListener = this;
    m_controls->SetEnabled(true);
    m_controls->EnableRegionSet(true);
    /* Mod (wide windows): the left column - the computer and 8 buttons - at the left
       canvas edge. Outside the layout the anchor has no effect. */
    m_controls->SetLayoutAnchor(W8_ANCHOR_LEFT);
    m_controls->Invalidate(0);
    m_menu_set = new W8OptionsMenuSet(&g_options_page_region_set);
    m_menu_set->SetEnabled(true);
    m_menu_set->EnableRegionSet(true);
    /* Mod (wide windows): page arrows at the ends of the stretched strip. */
    if (m_menu_set->m_previous->m_region != -1) {
        SetRegionLayoutAnchor(m_menu_set->m_previous->m_region, W8_ANCHOR_LEFT);
    }
    if (m_menu_set->m_next->m_region != -1) {
        SetRegionLayoutAnchor(m_menu_set->m_next->m_region, W8_ANCHOR_RIGHT);
    }
}

// FUNCTION: WIZ8 0x005a9200
W8OptionsScreen::~W8OptionsScreen()
{
    int index;
    SelectPanel(-1, true);
    m_save_slots.RemoveAllAndDelete();
    delete m_controls;
    delete m_menu_set;
    delete m_menu_selection;
    for (index = 0; index < 8; ++index) {
        delete m_panel[index];
    }
}

// FUNCTION: WIZ8 0x005a93c0
void W8OptionsScreen::SelectPanel(int selected, bool notify)
{
    if (selected == m_selected_panel) {
        return;
    }
    if (m_text_editor != 0) {
        if (m_text_editor->m_listener != 0) {
            m_text_editor->m_listener->OnTextEditComplete(m_text_editor, 1);
        }
        delete m_text_editor;
        m_text_editor = 0;
    }
    if (m_selected_panel != -1) {
        W8OptionsPanelSet* previous = m_panel[m_selected_panel];
        (*previous->m_panels.GetAt(previous->m_current))->SetActive(false);
        previous->m_active = false;
    }
    m_selected_panel = selected;
    if (selected != -1) {
        if (m_panel[selected] == 0) {
            m_panel[selected] = new W8OptionsPanelSet();
            W8OptionsPanelSet* created = m_panel[m_selected_panel];
            for (int index = g_options_panel_ranges[m_selected_panel].first;
                 index <= g_options_panel_ranges[m_selected_panel].last; ++index) {
                W8OptionsPanel* panel = CreateOptionsPanel(index, &created->m_compact_layout,
                                                           &created->m_hide_navigation);
                panel->Populate();
                created->m_panels.Add(panel);
            }
        }
        W8OptionsPanelSet* current = m_panel[m_selected_panel];
        if (!current->m_active) {
            current->m_current = 0;
        }
        (*current->m_panels.GetAt(current->m_current))->SetActive(true);
        current->m_active = true;
        m_menu_set->m_pMenuSet = m_panel[m_selected_panel];
        m_menu_set->UpdateMenuSet();
        if (m_selected_panel == 4) {
            m_panel[4]->m_page_count = (m_save_slots.GetCount() - 2) / 5 + 1;
        } else if (m_selected_panel == 5) {
            m_panel[5]->m_page_count = (m_save_slots.GetCount() - 1) / 5 + 1;
        }
        m_menu_set->UpdateMenuSet();
    }
    if (notify) {
        m_menu_selection->SetSelected(m_selected_panel);
    }
    if (m_selected_panel >= 0) {
        g_last_options_panel = m_selected_panel;
    }
}

// FUNCTION: WIZ8 0x005a9690
void W8OptionsScreen::ShowNotification(W8DialogCloseListener* listener, bool allow_cancel,
                                       int message, int value)
{
    m_modal_closing = true;
    delete m_active_modal;
    W8NotificationDialog* dialog = new W8NotificationDialog(message, allow_cancel, value);
    m_active_modal = dialog;
    if (listener != 0) {
        dialog->notify_target = listener;
    }
}

// FUNCTION: WIZ8 0x005a9a60
void W8OptionsScreen::OnPrimary(W8TextControl*)
{
    if (m_text_editor != 0) {
        if (m_text_editor->m_listener != 0) {
            m_text_editor->m_listener->OnTextEditComplete(m_text_editor, 1);
        }
        delete m_text_editor;
        m_text_editor = 0;
    }
    ShowNotification(this, true, 0x832, 0);
}

// FUNCTION: WIZ8 0x005a9720
unsigned char W8OptionsScreen::ProcessInput(const InputAtom* input)
{
    W8OptionsTextEditor* editor = m_text_editor;
    if (editor != 0) {
        if (input->usEvent == KEY_DOWN || input->usEvent == KEY_REPEAT) {
            if (input->usParam != VK_ESCAPE && input->usParam != VK_RETURN) {
                unsigned short character = TranslateKeyToCharacter(
                    static_cast<unsigned short>(input->usParam), input->usKeyState);
                if (character != 0 && strchr("\\/:*?\"<>|", character) != 0) {
                    return 0;
                }
                HandleTextInput(input);
                return 0;
            }
            SetTargetCursor(W8_CURSOR_NONE);
            if (editor->m_listener != 0) {
                editor->m_listener->OnTextEditComplete(editor, input->usParam == VK_ESCAPE);
            }
            delete m_text_editor;
            m_text_editor = 0;
            return 1;
        }
        DispatchMainGameMouseButtons(input);
    } else if (m_key_capture != 0 && input->usEvent == KEY_DOWN) {
        return m_key_capture->OnKey(static_cast<unsigned short>(input->usParam), input->usKeyState);
    }
    return 0;
}

// FUNCTION: WIZ8 0x005a95f0
void W8OptionsScreen::Redraw()
{
    if (m_redraw_pending) {
        ClearPrimarySurface();
        ClearSurfaceRect(0, 0, 640, 480);
        if (CanvasWideLayoutActive()) {
            /* Mod (wide windows): left column at the left canvas edge, the whole
               canvas marked for re-upload (the right panel is stretched to the right edge). */
            {
                CanvasDrawScope left_scope(W8_ANCHOR_LEFT);
                DrawCatalogImage(FRAME_BUFFER, 0xee, 0, 0, 0, 0, VO_BLT_SRCTRANSPARENCY, 0);
            }
            CanvasOriginScope scope(0);
            InvalidateRegion(0, 0, g_canvas_w, 0x1e0, 0);
        } else {
            DrawCatalogImageAndInvalidate(FRAME_BUFFER, 0xee, 0, 0, 0, 0, VO_BLT_SRCTRANSPARENCY,
                                          0);
        }
        m_redraw_pending = false;
    }
    if (m_selected_panel != -1) {
        W8OptionsPanelSet* panel_set = m_panel[m_selected_panel];
        if (panel_set->m_active) {
            (*panel_set->m_panels.GetAt(panel_set->m_current))->Redraw();
        }
    }
    m_controls->Redraw();
    if (m_selected_panel != -1) {
        m_menu_set->Redraw();
    }
    if (m_active_modal != 0) {
        m_active_modal->Draw();
    }
    if (m_text_editor != 0) {
        RenderActiveTextField();
    }
}

// FUNCTION: WIZ8 0x005a72f0
void W8OptionsValues::TransferByte(int* value, unsigned char* setting)
{
    if (applying == 0) {
        *value = *setting;
    } else {
        *setting = static_cast<unsigned char>(*value);
    }
}

// FUNCTION: WIZ8 0x005a7320
void W8OptionsValues::TransferRenderOption(int* value, W8RenderOption option)
{
    if (applying == 0) {
        *value = GetRenderOptionState(option);
    } else if (*value != 0) {
        EnableRenderOption(option);
    } else {
        DisableRenderOption(option);
    }
}

// FUNCTION: WIZ8 0x005a6e20
void W8OptionsValues::TransferSettings()
{
    TransferByte(&mouselook_toggle, &g_settings.mouselook_toggle);
    TransferByte(&mouselook_smoothing, &g_settings.mouselook_smoothing);
    TransferByte(&invert_mouse_y, &g_settings.invert_mouse_y);
    TransferByte(&ctrl_right_click_info, &g_settings.ctrl_right_click_info);
    TransferByte(&numeric_hit_points, &g_settings.numeric_hit_points);
    TransferByte(&autoscroll_combat_messages, &g_settings.autoscroll_combat_messages);
    TransferByte(&simplified_npc_interaction, &g_settings.simplified_npc_interaction);
    TransferByte(&verbose_combat_messages, &g_settings.verbose_combat_messages);
    TransferByte(&autoswap_weapons, &g_settings.autoswap_weapons);
    TransferByte(&skill_increase_messages, &g_settings.skill_increase_messages);
    if (applying == 0) {
        monster_movement_speed = g_settings.monster_movement_speed;
    } else {
        g_settings.monster_movement_speed = monster_movement_speed;
    }
    TransferByte(&auto_advance_character, &g_settings.auto_advance_character);
    TransferByte(&autotarget_spells, &g_settings.autotarget_spells);
    if (applying == 0) {
        gamma = g_settings.gamma;
    } else {
        g_settings.gamma = gamma;
    }
    TransferByte(&pc_confirmations, &g_settings.pc_confirmations);
    TransferByte(&pc_subtitles, &g_settings.pc_subtitles);
    if (applying == 0) {
        text_display_delay_ms = static_cast<float>(g_settings.text_display_delay_ms);
    } else {
        g_settings.text_display_delay_ms = static_cast<unsigned int>(text_display_delay_ms);
    }
    TransferByte(&auto_save, &g_settings.auto_save);
    if (applying == 0) {
        difficulty = g_status.difficulty;
    } else {
        g_status.difficulty = static_cast<W8Difficulty>(difficulty);
    }
    if (applying == 0) {
        difficulty = g_settings.difficulty;
    } else {
        g_settings.difficulty = static_cast<W8Difficulty>(difficulty);
    }
    TransferRenderOption(&render_options[0], W8_RENDER_OPTION_MIP_MAPPING);
    TransferRenderOption(&render_options[1], W8_RENDER_OPTION_MISSILE_LIGHTS);
    TransferRenderOption(&render_options[2], W8_RENDER_OPTION_DITHER);
    TransferRenderOption(&render_options[3], W8_RENDER_OPTION_MESH_SKY);
    TransferRenderOption(&render_options[4], W8_RENDER_OPTION_VIDEO_SYNC);
    TransferRenderOption(&render_options[5], W8_RENDER_OPTION_ADDITIONAL_ANIMATIONS);
    TransferRenderOption(&render_options[6], W8_RENDER_OPTION_HIGH_TEXTURE_DETAIL);
    TransferRenderOption(&render_options[7], W8_RENDER_OPTION_HIGH_TEXTURE_CACHE);
    TransferRenderOption(&render_options[8], W8_RENDER_OPTION_CORRECT_BLURRED_TEXT);
    TransferByte(&monster_shadows, &g_settings.monster_shadows);
    TransferByte(&smooth_monster_animations, &g_settings.smooth_monster_animations);
    TransferByte(&smooth_world_animations, &g_settings.smooth_world_animations);
    if (applying == 0) {
        combat_speed = (g_settings.combat_delay_ms - 5000) * -0.0002f;
        camera_auto_rotation = g_settings.camera_rotation_mode == W8_CAMERA_ROTATION_DISABLED
                                   ? 2
                                   : g_settings.camera_rotation_style;
        TransferByte(&continuous_combat, &g_settings.continuous_combat);
    } else {
        g_settings.combat_delay_ms = 5000 - static_cast<int>(combat_speed * 5000.0f);
        if (camera_auto_rotation == 2) {
            g_settings.camera_rotation_mode = W8_CAMERA_ROTATION_DISABLED;
        } else {
            g_settings.camera_rotation_mode = W8_CAMERA_ROTATION_ALL_TARGETS;
            g_settings.camera_rotation_style =
                static_cast<W8CameraRotationStyle>(camera_auto_rotation);
        }
        if (continuous_combat != g_settings.continuous_combat) {
            TogglePartyCombatStance();
        }
    }
}

/* Menu rows come from the shared source table: the button's own geometry and
   text parameters sit in row[9..17], the item id in row[0], and the two
   optional child controls in row[1..4] and row[5..8].  Each child joins the
   button's owner panel, registers the button's listener subobject, and picks
   up the shared layout flag. */
// FUNCTION: WIZ8 0x005a7370
W8OptionsMenuButton::W8OptionsMenuButton(Controls* owner, const W8OptionsMenuRow* row)
    : W8TextControl(owner, 0xffffffff, row->bounds.left, row->bounds.top, row->bounds.right,
                    row->bounds.bottom, 0xef, 0, row->image_indices[0], row->image_indices[1],
                    row->image_indices[2], row->image_indices[3], row->image_indices[4])
{
    m_item_id = row->item_id;

    if (row->child_bounds[0].left != -1) {
        W8TextControl* control = new W8TextControl(
            m_pPanel, 0xffffffff, row->child_bounds[0].left, row->child_bounds[0].top,
            row->child_bounds[0].right, row->child_bounds[0].bottom, -1, -1, -1, -1, -1, -1, -1);
        control->m_listener = this;
        control->AddLayoutFlags(g_W8TextControlSilentHover);
    }

    if (row->child_bounds[1].left != -1) {
        W8TextControl* control = new W8TextControl(
            m_pPanel, 0xffffffff, row->child_bounds[1].left, row->child_bounds[1].top,
            row->child_bounds[1].right, row->child_bounds[1].bottom, -1, -1, -1, -1, -1, -1, -1);
        control->m_listener = this;
        control->AddLayoutFlags(g_W8TextControlSilentHover);
    }
}

// FUNCTION: WIZ8 0x005a7570
void W8OptionsMenuButton::OnPrimary(W8TextControl*)
{
    if (m_listener != 0) {
        m_listener->OnPrimary(this);
    }
}

W8OptionsCheckbox::W8OptionsCheckbox(Controls* owner, int top, int* value)
    : W8TextControl(owner, 0xffffffff, 0x14d, top - 2, 0, 0, 0xf1, 0, 2, 0, 3, 1, -1),
      m_value(value)
{
    AddLayoutFlags(g_W8TextControlLayoutLatchedImage | g_W8TextControlLayoutToggle);
    if (*m_value != 0) {
        EnableSecondaryState(false);
    }
}

// FUNCTION: WIZ8 0x005a7680
void W8OptionsCheckbox::OnLeftButtonUp(int event)
{
    W8TextControl::OnLeftButtonUp(event);
    *m_value = static_cast<unsigned char>(m_stateFlags) & g_W8TextControlStateSecondary & 0xff;
}

W8OptionsSlider::W8OptionsSlider(Controls* owner, int top, float* value, bool alternate)
    : W8HorizontalRangeThumb(owner, 0xffffffff, 0xc9, top - 2, 0xf5, 0, alternate ? 4 : 0, 1, 2, 3),
      m_value(value)
{
    if (m_value != 0) {
        m_position = *m_value;
    }
}

// FUNCTION: WIZ8 0x005a7f60
void W8OptionsSlider::OnMouseMove(int event)
{
    W8HorizontalRangeThumb::OnMouseMove(event);
    if (m_dragging && m_value != 0) {
        *m_value = m_position;
    }
}

// FUNCTION: WIZ8 0x005a7f90
void W8OptionsSlider::Redraw(bool full_redraw)
{
    if (m_active && (full_redraw || m_dirty)) {
        W8HorizontalRangeThumb::Redraw(full_redraw);
        if (m_enabled && m_pixelPosition > 13) {
            int left = m_pPanel->m_bounds.left + m_left;
            int top = m_pPanel->m_bounds.top + m_top;
            unsigned short color = Get16BPPColor(0x00ff00);
            int end = m_pixelPosition < 74 ? m_pixelPosition - 1 : 73;
            ColorFillVideoSurfaceArea(FRAME_BUFFER, left + 13, top + 7, left + end, top + 9, color);
            if (m_pixelPosition > 83) {
                ColorFillVideoSurfaceArea(FRAME_BUFFER, left + 83, top + 7,
                                          left + m_pixelPosition - 1, top + 9, color);
            }
        }
    }
}

/* The base cleanup at 0x005A93A0 destroys m_lsButtons; absence of constructor
   unwind registration does not establish its authored declaration. */

W8OptionsSelection::W8OptionsSelection(int* value) : m_value(value) {}

/* The empty body still emits the m_lsButtons vector teardown over the
   W8ControlSelection base. */
// FUNCTION: WIZ8 0x005A8B50
W8OptionsSelection::~W8OptionsSelection() {}

// FUNCTION: WIZ8 0x005a8080
void W8OptionsSelection::OnPrimary(W8TextControl* control)
{
    W8ControlSelection::OnPrimary(control);
    *m_value = m_selectedIndex;
}

// FUNCTION: WIZ8 0x005a8320
W8OptionsPanel::~W8OptionsPanel()
{
    DestroyAllControls();
    m_text_buffers.RemoveAllAndDelete();
    m_option_selections.RemoveAllAndDelete();
}

// FUNCTION: WIZ8 0x005a84e0
W8OptionsCheckbox* W8OptionsPanel::AddCheckbox(int label, int* value)
{
    W8ControlsRect bounds = {m_bounds.left + 20, m_bounds.top + m_content_top, m_bounds.right,
                             m_bounds.top + m_content_top + 22};
    W8TextBuffer* text = new W8TextBuffer(&bounds, gppStringList[label], g_options_detail_font,
                                          g_W8TextBufferAlignTop | g_W8TextBufferAlignLeft, 4);
    m_text_buffers.Add(text);
    W8OptionsCheckbox* checkbox = new W8OptionsCheckbox(this, m_content_top, value);
    AnchorOptionsWidget(checkbox, W8_ANCHOR_RIGHT); /* mod: checkbox at the right edge */
    m_content_top += 44;
    return checkbox;
}

// FUNCTION: WIZ8 0x005a8670
W8TextControl* W8OptionsPanel::AddChoiceButton(int label)
{
    W8ControlsRect bounds = {m_bounds.left, m_bounds.top + m_content_top, m_bounds.left + 0x147,
                             m_bounds.top + m_content_top + 22};
    W8TextBuffer* text = new W8TextBuffer(&bounds, gppStringList[label], g_options_detail_font,
                                          g_W8TextBufferAlignTop | g_W8TextBufferAlignRight, 4);
    m_text_buffers.Add(text);
    W8TextControl* button = new W8TextControl(this, 0xffffffff, 0x151, m_content_top + 1, 0, 0,
                                              0xf1, 0, 4, 6, 5, 7, -1);
    button->AddLayoutFlags(g_W8TextControlLayoutLatchedImage | g_W8TextControlLayoutToggle);
    AnchorOptionsWidget(button, W8_ANCHOR_RIGHT); /* mod: with its label at the right edge */
    m_content_top += 22;
    return button;
}

// FUNCTION: WIZ8 0x005a8800
W8OptionsSlider* W8OptionsPanel::AddSlider(int label, float* value, bool alternate)
{
    W8ControlsRect bounds = {m_bounds.left + 20, m_bounds.top + m_content_top, m_bounds.right,
                             m_bounds.top + m_content_top + 22};
    W8TextBuffer* text = new W8TextBuffer(&bounds, gppStringList[label], g_options_detail_font,
                                          g_W8TextBufferAlignTop | g_W8TextBufferAlignLeft, 4);
    m_text_buffers.Add(text);
    W8OptionsSlider* slider = new W8OptionsSlider(this, m_content_top, value, alternate);
    AnchorOptionsWidget(slider, W8_ANCHOR_RIGHT); /* mod: slider at the right edge */
    m_content_top += 22;
    return slider;
}

/* Mod: a slider whose label is not from StringData.DAT (new settings). */
W8OptionsSlider* W8OptionsPanel::AddSliderText(const wchar_t* label, float* value)
{
    W8ControlsRect bounds = {m_bounds.left + 20, m_bounds.top + m_content_top, m_bounds.right,
                             m_bounds.top + m_content_top + 22};
    W8TextBuffer* text = new W8TextBuffer(&bounds, const_cast<wchar_t*>(label), g_options_detail_font,
                                          g_W8TextBufferAlignTop | g_W8TextBufferAlignLeft, 4);
    m_text_buffers.Add(text);
    W8OptionsSlider* slider = new W8OptionsSlider(this, m_content_top, value, false);
    AnchorOptionsWidget(slider, W8_ANCHOR_RIGHT);
    m_content_top += 22;
    return slider;
}

W8TextControl* W8OptionsPanel::AddChoiceButtonText(const wchar_t* label)
{
    W8ControlsRect bounds = {m_bounds.left, m_bounds.top + m_content_top, m_bounds.left + 0x147,
                             m_bounds.top + m_content_top + 22};
    W8TextBuffer* text = new W8TextBuffer(&bounds, const_cast<wchar_t*>(label), g_options_detail_font,
                                          g_W8TextBufferAlignTop | g_W8TextBufferAlignRight, 4);
    m_text_buffers.Add(text);
    W8TextControl* button = new W8TextControl(this, 0xffffffff, 0x151, m_content_top + 1, 0, 0,
                                              0xf1, 0, 4, 6, 5, 7, -1);
    button->AddLayoutFlags(g_W8TextControlLayoutLatchedImage | g_W8TextControlLayoutToggle);
    AnchorOptionsWidget(button, W8_ANCHOR_RIGHT);
    m_content_top += 22;
    return button;
}

void W8OptionsPanel::AddChoicesText(const wchar_t* label, int count, const wchar_t* const* choices,
                                    W8OptionsSelection* selection)
{
    W8ControlsRect bounds = {m_bounds.left + 20, m_bounds.top + m_content_top, m_bounds.right,
                             m_bounds.top + m_content_top + 22};
    W8TextBuffer* text = new W8TextBuffer(&bounds, const_cast<wchar_t*>(label), g_options_detail_font,
                                          g_W8TextBufferAlignTop | g_W8TextBufferAlignLeft, 4);
    m_text_buffers.Add(text);
    m_option_selections.Add(selection);
    for (int index = 0; index < count; ++index) {
        selection->AddEntry(AddChoiceButtonText(choices[index]));
    }
    selection->SetSelected(*selection->m_value);
    m_content_top += 22;
}

/* ---- Mod (2026-10-16): display mode and resolution instead of 3D Setup ----
   Second "Video" page. The choice is written to wiz8wide.ini and applied immediately
   (VideoApplyDisplaySettings) if the screen aspect ratio does not change; otherwise - from
   the next launch (different canvas width), which a line under the setting says.
   The resolution is chosen with the same selector as class and race in character
   creation: arrows on the sides and a field that opens a list on click
   (images CG_Buttons.sti / CG_Profession.sti, like W8CharacterStatsRow). */

namespace {

struct W8ScreenSize {
    int width;
    int height;
};

enum {
    W8_SCREEN_SIZE_MAX = 24,
    W8_DROPDOWN_ROWS = 10, /* how many rows the open list shows */
    W8_DROPDOWN_X = 0xa5,  /* left edge of the selector in the panel (the arrow) */
    W8_CG_BUTTONS = 0x10a, /* Char Generation\CG_Buttons.sti: arrows */
    W8_CG_FIELD = 0x104    /* Char Generation\CG_Profession.sti: field and list rows */
};

int g_display_mode_value; /* what is selected in the mode group */
int g_display_width;      /* selected resolution (window / full screen) */
int g_display_height;
W8ScreenSize g_screen_sizes[W8_SCREEN_SIZE_MAX]; /* 32-bit monitor modes, largest first */
int g_screen_size_count;
wchar_t g_resolution_text[32];
wchar_t g_entry_texts[W8_DROPDOWN_ROWS][32];
W8TextBuffer* g_display_note;

struct W8ResolutionDropdown {
    W8TextControl* decrement;
    W8TextControl* value;
    W8TextControl* increment;
    W8TextControl* entries[W8_DROPDOWN_ROWS];
    int entry_count;
    bool open;
    int list_top; /* list rows, 640-screen coordinates (without the edge shift) */
    int list_bottom;
    int list_left;
    int list_right;
    const W8OptionsPanel* owner; /* panel with the list (comparison only, never dereferenced) */
};
/* The last value saved in the ini - we go back to it if the monitor does not
   support the selected full-screen mode. */
int g_saved_mode;
int g_saved_width;
int g_saved_height;
W8ResolutionDropdown g_dropdown;

void FormatSize(wchar_t* out, int width, int height)
{
    /* "1920 x 1080" without _snwprintf (UCRT in the hybrid is not initialized) */
    const int parts[2] = {width, height};
    for (int part = 0; part < 2; ++part) {
        wchar_t digits[12];
        int count = 0;
        int value = parts[part] > 0 ? parts[part] : 0;
        do {
            digits[count++] = static_cast<wchar_t>(L'0' + value % 10);
            value /= 10;
        } while (value > 0 && count < 11);
        while (count > 0) {
            *out++ = digits[--count];
        }
        if (part == 0) {
            *out++ = L' ';
            *out++ = L'x';
            *out++ = L' ';
        }
    }
    *out = L'\0';
}

void CollectScreenSizes(void)
{
    g_screen_size_count = 0;
    int desktop_w;
    int desktop_h;
    DisplayDesktopSize(&desktop_w, &desktop_h); /* not a monitor mode in full screen */
    DEVMODEA mode;
    memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(mode);
    for (DWORD index = 0; EnumDisplaySettingsA(0, index, &mode); ++index) {
        const int w = static_cast<int>(mode.dmPelsWidth);
        const int h = static_cast<int>(mode.dmPelsHeight);
        if (mode.dmBitsPerPel != 32 || w < 640 || h < 480 || w > desktop_w || h > desktop_h) {
            continue;
        }
        bool known = false;
        for (int k = 0; k < g_screen_size_count; ++k) {
            known = known || (g_screen_sizes[k].width == w && g_screen_sizes[k].height == h);
        }
        if (known) {
            continue;
        }
        /* insertion sorted by descending area; extra small ones drop off */
        int at = g_screen_size_count < W8_SCREEN_SIZE_MAX ? g_screen_size_count : W8_SCREEN_SIZE_MAX;
        while (at > 0 && g_screen_sizes[at - 1].width * g_screen_sizes[at - 1].height < w * h) {
            if (at < W8_SCREEN_SIZE_MAX) {
                g_screen_sizes[at] = g_screen_sizes[at - 1];
            }
            --at;
        }
        if (at < W8_SCREEN_SIZE_MAX) {
            g_screen_sizes[at].width = w;
            g_screen_sizes[at].height = h;
            if (g_screen_size_count < W8_SCREEN_SIZE_MAX) {
                ++g_screen_size_count;
            }
        }
    }
    if (g_screen_size_count == 0) {
        g_screen_sizes[0].width = desktop_w;
        g_screen_sizes[0].height = desktop_h;
        g_screen_size_count = 1;
    }
}

/* Which resolutions go into the open list (it has 10 rows, while a
   monitor can have 20+ modes): the Windows resolution, the classic 4:3 ones for those who
   want it as in the original, then the largest of the rest. The arrows
   cycle through all modes. */
int g_entry_size_index[W8_DROPDOWN_ROWS];

int ChooseDropdownEntries(void)
{
    bool chosen[W8_SCREEN_SIZE_MAX] = {};
    int count = 0;
    int desktop_w;
    int desktop_h;
    DisplayDesktopSize(&desktop_w, &desktop_h);
    const W8ScreenSize wanted[] = {{desktop_w, desktop_h},
                                   {g_display_width, g_display_height}, /* currently selected */
                                   {640, 480},   {800, 600},  {1024, 768},
                                   {1280, 960},  {1600, 1200}};
    for (int w = 0; w < static_cast<int>(sizeof(wanted) / sizeof(wanted[0])); ++w) {
        for (int k = 0; k < g_screen_size_count && count < W8_DROPDOWN_ROWS; ++k) {
            if (!chosen[k] && g_screen_sizes[k].width == wanted[w].width &&
                g_screen_sizes[k].height == wanted[w].height) {
                chosen[k] = true;
                ++count;
            }
        }
    }
    for (int k = 0; k < g_screen_size_count && count < W8_DROPDOWN_ROWS; ++k) {
        if (!chosen[k]) {
            chosen[k] = true;
            ++count;
        }
    }
    int at = 0;
    for (int k = 0; k < g_screen_size_count; ++k) {
        if (chosen[k]) {
            g_entry_size_index[at++] = k; /* in list order: from largest to smallest */
        }
    }
    return at;
}

/* The current resolution is not among the monitor modes (entered in the ini by hand):
   the arrow takes the nearest larger (larger) or smaller one by area. The list is from
   largest to smallest. */
int NearestSizeIndex(bool larger)
{
    const int area = g_display_width * g_display_height;
    if (larger) {
        for (int k = g_screen_size_count - 1; k >= 0; --k) {
            if (g_screen_sizes[k].width * g_screen_sizes[k].height > area) {
                return k;
            }
        }
        return 0;
    }
    for (int k = 0; k < g_screen_size_count; ++k) {
        if (g_screen_sizes[k].width * g_screen_sizes[k].height < area) {
            return k;
        }
    }
    return g_screen_size_count - 1;
}

int CurrentSizeIndex(void)
{
    for (int k = 0; k < g_screen_size_count; ++k) {
        if (g_screen_sizes[k].width == g_display_width && g_screen_sizes[k].height == g_display_height) {
            return k;
        }
    }
    return -1;
}

/* Redraw the whole screen, as after closing a dialog: for text of a new
   length the old background is not erased, and redrawing a single panel loses the page
   strip (it is drawn by m_menu_set); a closed list also has to be erased. */
void RefreshOptionsScreen(void)
{
    W8OptionsScreen* screen = g_options_screen;
    if (screen == 0) {
        return;
    }
    screen->m_redraw_pending = true;
    screen->m_controls->Invalidate(0);
    if (screen->m_selected_panel != -1) {
        W8OptionsPanelSet* panel_set = screen->m_panel[screen->m_selected_panel];
        if (panel_set->m_active) {
            (*panel_set->m_panels.GetAt(panel_set->m_current))->Invalidate(0);
        }
        screen->m_menu_set->Invalidate(0);
    }
}

void UpdateDropdown(void)
{
    if (g_dropdown.value == 0) {
        return;
    }
    const bool borderless = g_display_mode_value == W8_DISPLAY_BORDERLESS;
    if (borderless) {
        int desktop_w;
        int desktop_h;
        DisplayDesktopSize(&desktop_w, &desktop_h);
        FormatSize(g_resolution_text, desktop_w, desktop_h);
    } else {
        FormatSize(g_resolution_text, g_display_width, g_display_height);
    }
    g_dropdown.value->m_textBuffer.SetText(g_resolution_text, g_options_detail_font);
    g_dropdown.value->SetEnabled(!borderless);
    g_dropdown.decrement->SetEnabled(!borderless);
    g_dropdown.increment->SetEnabled(!borderless);
}

void SetDropdownOpen(bool open)
{
    g_dropdown.open = open;
    for (int index = 0; index < g_dropdown.entry_count; ++index) {
        g_dropdown.entries[index]->SetActive(open);
        g_dropdown.entries[index]->SetEnabled(open);
        /* the hover highlight must not survive closing the list */
        g_dropdown.entries[index]->m_textBuffer.SetFontStateIndex(-1);
    }
}

/* Save the choice and apply it: immediately if the aspect ratio is the same, otherwise show
   that a restart is needed. */
W8OptionsSelection* g_mode_selection; /* mode group on the page (owned by the panel) */

const wchar_t* RestartNote(void)
{
    return LocalizedOr(L"Restart the game to apply.", L"Neustart erforderlich.",
                       L"Red\x00e9marrez le jeu.", L"Нужен перезапуск игры.");
}

void SetDisplayNote(const wchar_t* text)
{
    if (g_display_note != 0) {
        g_display_note->SetText(const_cast<wchar_t*>(text), g_options_detail_font);
    }
}

void ApplyDisplaySelection(void)
{
    /* Full screen: first ask the device whether the monitor supports
       this mode, and only then save - otherwise the next launch will not
       open. Not supported - revert to the previous choice. */
    if (g_display_mode_value == W8_DISPLAY_FULLSCREEN &&
        !VideoFullscreenSizeSupported(g_display_width, g_display_height)) {
        g_display_mode_value = g_saved_mode;
        g_display_width = g_saved_width;
        g_display_height = g_saved_height;
        if (g_mode_selection != 0) {
            g_mode_selection->SetSelected(g_saved_mode);
        }
        SetDisplayNote(LocalizedOr(L"Not supported by the monitor.", L"Vom Monitor nicht unterst\x00fctzt.",
                                   L"Non pris en charge par l'\x00e9" L"cran.",
                                   L"Монитор не поддерживает этот режим."));
        UpdateDropdown();
        RefreshOptionsScreen();
        return;
    }
    DisplaySaveSettings(g_display_mode_value, g_display_width, g_display_height);
    g_saved_mode = g_display_mode_value;
    g_saved_width = g_display_width;
    g_saved_height = g_display_height;
    const bool applied = VideoApplyDisplaySettings();
    SetDisplayNote(applied ? &g_empty_wide_string : RestartNote());
    UpdateDropdown();
    RefreshOptionsScreen();
}

void SelectSize(int index)
{
    if (index < 0 || index >= g_screen_size_count) {
        return;
    }
    g_display_width = g_screen_sizes[index].width;
    g_display_height = g_screen_sizes[index].height;
    ApplyDisplaySelection();
}

/* Open list row: hover highlight, as in character creation. */
class W8DropdownEntry : public W8TextControl {
public:
    W8DropdownEntry(Controls* owner, int left, int top, int height, int sprite, int text_offset)
        : W8TextControl(owner, 0xffffffff, left, top, left + 0x7e, top + height, W8_CG_FIELD, 0,
                        sprite, sprite, sprite, sprite, sprite)
    {
        AddLayoutFlags(g_W8TextControlLayoutImageAtOrigin);
        m_textBuffer.SetLayoutMode(g_W8TextBufferAlignMiddle | g_W8TextBufferAlignCenter);
        m_pressedTextOffset = 0;
        UpdateTextBounds(left + 4, top + text_offset, left + 0x7a, top + text_offset + 0x16);
    }
    /* When shown, the panel enables all its children, so a closed list
       simply is not drawn and does not react (see g_dropdown.open). */
    virtual void Redraw(bool full_redraw) override
    {
        if (g_dropdown.open) {
            W8TextControl::Redraw(full_redraw);
        }
    }
    virtual void OnMouseEnter(int event) override
    {
        if (!g_dropdown.open) {
            return;
        }
        W8TextControl::OnMouseEnter(event);
        if (m_active && m_enabled) {
            m_textBuffer.SetFontStateIndex(0xd);
            Invalidate(false);
        }
    }
    virtual void OnMouseLeave(int event) override
    {
        W8TextControl::OnMouseLeave(event);
        if (m_active && g_dropdown.open) {
            m_textBuffer.SetFontStateIndex(-1);
            Invalidate(false);
        }
    }
};

class W8DropdownListener : public W8TextControl::Listener {
public:
    virtual void OnPrimary(W8TextControl* control) override
    {
        if (g_display_mode_value == W8_DISPLAY_BORDERLESS) {
            return;
        }
        const int current = CurrentSizeIndex();
        if (!g_dropdown.open && control != g_dropdown.value && control != g_dropdown.increment &&
            control != g_dropdown.decrement) {
            return; /* click on an invisible row of a closed list */
        }
        if (control == g_dropdown.value) {
            SetDropdownOpen(!g_dropdown.open);
            RefreshOptionsScreen();
            return;
        }
        if (g_dropdown.open) {
            SetDropdownOpen(false);
        }
        if (control == g_dropdown.increment) {
            /* right - larger (the list goes from largest to smallest) */
            SelectSize(current < 0 ? NearestSizeIndex(true) : (current == 0 ? 0 : current - 1));
            return;
        }
        if (control == g_dropdown.decrement) {
            SelectSize(current < 0 ? NearestSizeIndex(false)
                                   : (current + 1 < g_screen_size_count ? current + 1 : current));
            return;
        }
        for (int index = 0; index < g_dropdown.entry_count; ++index) {
            if (control == g_dropdown.entries[index]) {
                control->OnMouseLeave(0);
                SelectSize(g_entry_size_index[index]);
                return;
            }
        }
    }
};
W8DropdownListener g_dropdown_listener;

class W8DisplayModeSelection : public W8OptionsSelection {
public:
    W8DisplayModeSelection() : W8OptionsSelection(&g_display_mode_value) {}
    virtual void OnPrimary(W8TextControl* control) override
    {
        W8OptionsSelection::OnPrimary(control);
        if (g_dropdown.open) {
            SetDropdownOpen(false);
        }
        ApplyDisplaySelection();
    }
};

} // namespace

/* Panels deleted (leaving the settings): forget the pointers to their children, otherwise
   a new panel at the old address could match g_dropdown.owner. */
static void ResetDisplaySettingsUi(void)
{
    memset(&g_dropdown, 0, sizeof(g_dropdown));
    g_display_note = 0;
    g_mode_selection = 0;
    g_view_distance_slider = 0;
}

bool OptionsDropdownCoversText(const W8OptionsPanel* panel, const W8TextBuffer* text)
{
    if (panel != g_dropdown.owner) {
        return false; /* the list lives only on its own panel */
    }
    /* labels are drawn over the panel's children: while the list is open, those that
       overlap its rows are not drawn */
    if (!g_dropdown.open) {
        return false;
    }
    const W8ControlsRect& bounds = text->GetLayoutBounds();
    if (bounds.bottom <= g_dropdown.list_top || bounds.top >= g_dropdown.list_bottom) {
        return false;
    }
    /* horizontally - the text itself (row width), taking canvas edges into account:
       labels on the left at the left edge, the list - at the right */
    int width = static_cast<int>(text->m_maxLineWidth);
    if (width <= 0) {
        width = bounds.right - bounds.left; /* not laid out yet - the whole rectangle */
    }
    const bool right_aligned = (text->GetLayoutMode() & g_W8TextBufferAlignRight) != 0;
    int left = right_aligned ? bounds.right - width : bounds.left;
    int right = right_aligned ? bounds.right : bounds.left + width;
    const int shift = CanvasAnchorShift(OptionsTextAnchor(text));
    left += shift;
    right += shift;
    const int list_shift = CanvasAnchorShift(W8_ANCHOR_RIGHT);
    return right > g_dropdown.list_left + list_shift && left < g_dropdown.list_right + list_shift;
}

void W8OptionsPanel::AddDisplaySettings(void)
{
    DisplayPendingSettings(&g_display_mode_value, &g_display_width, &g_display_height);
    CollectScreenSizes();
    memset(&g_dropdown, 0, sizeof(g_dropdown));
    g_dropdown.owner = this;
    g_saved_mode = g_display_mode_value;
    g_saved_width = g_display_width;
    g_saved_height = g_display_height;
    m_content_top += 22;
    static const wchar_t* const en[] = {L"Borderless window", L"Fullscreen", L"Window"};
    static const wchar_t* const ru[] = {L"Окно без рамки", L"Полный экран", L"Окно"};
    static const wchar_t* const de[] = {L"Randloses Fenster", L"Vollbild", L"Fenster"};
    static const wchar_t* const fr[] = {L"Fen\x00eatre sans bordure", L"Plein \x00e9" L"cran",
                                        L"Fen\x00eatre"};
    const wchar_t* choices[3];
    for (int index = 0; index < 3; ++index) {
        choices[index] = LocalizedOr(en[index], de[index], fr[index], ru[index]);
    }
    AddChoicesText(LocalizedOr(L"Display mode", L"Anzeigemodus", L"Mode d'affichage",
                               L"Режим экрана"),
                   3, choices, g_mode_selection = new W8DisplayModeSelection);

    /* resolution: label on the left, selector "◄ [field] ►" at the right edge */
    const int top = m_content_top;
    W8ControlsRect bounds = {m_bounds.left + 20, m_bounds.top + top + 1, m_bounds.right,
                             m_bounds.top + top + 23};
    W8TextBuffer* label = new W8TextBuffer(
        &bounds,
        const_cast<wchar_t*>(LocalizedOr(L"Resolution", L"Aufl\x00f6sung", L"R\x00e9solution",
                                         L"Разрешение")),
        g_options_detail_font, g_W8TextBufferAlignTop | g_W8TextBufferAlignLeft, 4);
    m_text_buffers.Add(label);
    const int x = W8_DROPDOWN_X;
    g_dropdown.decrement =
        new W8TextControl(this, 0xffffffff, x + 1, top, 0, 0, W8_CG_BUTTONS, 0, 0, 2, 1, 4, 3);
    g_dropdown.value =
        new W8TextControl(this, 0xffffffff, x + 0x1c, top + 1, 0, 0, W8_CG_FIELD, 0, 1, 1, 2, 2, 3);
    g_dropdown.value->m_textBuffer.SetLayoutMode(g_W8TextBufferAlignMiddle | g_W8TextBufferAlignCenter);
    g_dropdown.value->m_pressedTextOffset = 0;
    g_dropdown.value->UpdateTextBounds(x + 0x1c + 4, top + 1, x + 0x1c + 0x6e, top + 1 + 0x16);
    g_dropdown.increment =
        new W8TextControl(this, 0xffffffff, x + 0x9f, top, 0, 0, W8_CG_BUTTONS, 0, 5, 7, 6, 9, 8);
    W8TextControl* row[3] = {g_dropdown.decrement, g_dropdown.value, g_dropdown.increment};
    for (int index = 0; index < 3; ++index) {
        row[index]->m_listener = &g_dropdown_listener;
        AnchorOptionsWidget(row[index], W8_ANCHOR_RIGHT);
        row[index]->SetActive(true);
    }

    /* open list: above the field, the bottom row right against the field */
    g_dropdown.entry_count = ChooseDropdownEntries();
    const int n = g_dropdown.entry_count;
    const int list_height = n == 1 ? 0x18 : 0x18 * 2 + (n - 2) * 0x16;
    const int list_top = top + 1 - list_height;
    for (int index = 0; index < n; ++index) {
        int sprite;
        int entry_top;
        int height;
        int text_offset;
        if (index == 0) {
            sprite = 4; /* top row with the frame */
            entry_top = list_top;
            height = 0x18;
            text_offset = 2;
        } else if (index == n - 1) {
            sprite = 6; /* bottom one */
            entry_top = list_top + 2 + index * 0x16;
            height = 0x18;
            text_offset = 0;
        } else {
            sprite = 5;
            entry_top = list_top + 2 + index * 0x16;
            height = 0x16;
            text_offset = 0;
        }
        W8DropdownEntry* entry =
            new W8DropdownEntry(this, x + 0x1c, entry_top, height, sprite, text_offset);
        const W8ScreenSize& size = g_screen_sizes[g_entry_size_index[index]];
        FormatSize(g_entry_texts[index], size.width, size.height);
        entry->m_textBuffer.SetText(g_entry_texts[index], g_options_detail_font);
        entry->m_listener = &g_dropdown_listener;
        AnchorOptionsWidget(entry, W8_ANCHOR_RIGHT);
        g_dropdown.entries[index] = entry;
    }
    g_dropdown.list_top = m_bounds.top + list_top;
    g_dropdown.list_bottom = m_bounds.top + top + 1;
    g_dropdown.list_left = m_bounds.left + x + 0x1c;
    g_dropdown.list_right = m_bounds.left + x + 0x1c + 0x7e;
    SetDropdownOpen(false);
    UpdateDropdown();
    m_content_top += 30;

    /* "restart needed" - only when the screen aspect ratio changed */
    W8ControlsRect note_bounds = {m_bounds.left + 20, m_bounds.top + m_content_top, m_bounds.right,
                                  m_bounds.top + m_content_top + 22};
    g_display_note = new W8TextBuffer(&note_bounds, &g_empty_wide_string, g_options_detail_font,
                                      g_W8TextBufferAlignTop | g_W8TextBufferAlignLeft, 4);
    m_text_buffers.Add(g_display_note);
    /* a choice waiting for a restart (different aspect ratio) - also after reopening
       the settings */
    {
        int pending_w = g_display_width;
        int pending_h = g_display_height;
        if (g_display_mode_value == W8_DISPLAY_BORDERLESS) {
            DisplayDesktopSize(&pending_w, &pending_h);
        }
        if (CanvasWidthForOutput(pending_w, pending_h) != g_canvas_w) {
            SetDisplayNote(RestartNote());
        }
    }
    m_content_top += 22;
}

// FUNCTION: WIZ8 0x005a8980
void W8OptionsPanel::AddChoices(int label, int count, const int* choices, int* value)
{
    W8ControlsRect bounds = {m_bounds.left + 20, m_bounds.top + m_content_top, m_bounds.right,
                             m_bounds.top + m_content_top + 22};
    W8TextBuffer* text = new W8TextBuffer(&bounds, gppStringList[label], g_options_detail_font,
                                          g_W8TextBufferAlignTop | g_W8TextBufferAlignLeft, 4);
    m_text_buffers.Add(text);
    W8OptionsSelection* selection = new W8OptionsSelection(value);
    m_option_selections.Add(selection);
    for (int index = 0; index < count; ++index) {
        selection->AddEntry(AddChoiceButton(choices[index]));
    }
    selection->SetSelected(*selection->m_value);
    m_content_top += 22;
}

// FUNCTION: WIZ8 0x005a7590
void W8OptionsMenuButton::Redraw(bool full_redraw)
{
    if ((full_redraw || m_dirty) && (m_stateFlags & g_W8TextControlStateSecondary) != 0 &&
        m_item_id != -1) {
        DrawCatalogImageAndInvalidate(FRAME_BUFFER, 0xf0, 0, m_item_id, 12, 15,
                                      VO_BLT_SRCTRANSPARENCY, 0);
    }
    W8TextControl::Redraw(full_redraw);
}

// FUNCTION: WIZ8 0x005a75e0
void W8OptionsMenuButton::EnableSecondaryState(bool immediate)
{
    W8TextControl::EnableSecondaryState(immediate);
    Invalidate(immediate);
}

// FUNCTION: WIZ8 0x005a8470
void W8OptionsPanel::Redraw()
{
    bool redraw_text = m_fDirty && m_fEnabled;
    Controls::Redraw();
    if (redraw_text) {
        for (int index = 0; index < m_text_buffers.GetCount(); ++index) {
            W8TextBuffer* text = *m_text_buffers.GetAt(index);
            if (OptionsDropdownCoversText(this, text)) {
                continue; /* mod: under the open resolution list */
            }
            CanvasDrawScope text_scope(OptionsTextAnchor(text)); /* mod: canvas edge */
            text->RenderToTarget(0, true, FRAME_BUFFER);
        }
    }
}

// FUNCTION: WIZ8 0x005a9020
void W8OptionsMenuSet::Redraw()
{
    if (m_fEnabled && (m_fDirty || m_fLayoutDirty)) {
        Controls::Redraw();
        m_page_text->RenderToTarget(0, true, FRAME_BUFFER);
    }
}

/* The shared panel base sizes itself from the chrome sprite of its render
   target and registers one region-set row per concrete panel index. */
// FUNCTION: WIZ8 0x005a81e0
W8OptionsPanel::W8OptionsPanel(int region_index)
    : Controls(0x104, 0, 0, 0, 0xee, 0, 1), m_current(0), m_content_top(0x18)
{
    AcquireRegionSet(g_options_panel_region_sets + region_index);

    short width;
    short height;
    GetCatalogImageSize(0xf2, 0, 0, &width, &height);
    m_bounds.right = m_bounds.left + static_cast<unsigned short>(width);
    m_bounds.bottom = m_bounds.top + static_cast<unsigned short>(height);
    /* Mod (wide windows): the right panel background (options or save frame)
       is stretched from the left column to the right canvas edge by repeating a piece
       (s_subscreen_art_cuts in MainGameLayout.cpp). The contents are laid out
       over it by child anchors (AnchorOptionsWidget, top of the file). Outside the
       canvas layout m_wideMode has no effect. */
    m_wideMode = W8_PANEL_WIDE_STRETCH;
}

/* The menu set builds itself from the shared options font: its panel bounds
   follow the rendered chrome sprite, then the two page arrows are constructed
   and registered with the listener subobject, and the page text starts as the
   shared empty wide-string global until UpdateMenuSet fills it. */
// FUNCTION: WIZ8 0x005a8c90
W8OptionsMenuSet::W8OptionsMenuSet(unsigned int* shared_region_set)
    : Controls(0x11b, 0x1a8, 0, 0, 0xf3, 0, 0), m_pMenuSet(0)
{
    AcquireRegionSet(shared_region_set);

    short width;
    short height;
    GetCatalogImageSize(0xf3, 0, 0, &width, &height);
    m_bounds.right = m_bounds.left + static_cast<unsigned short>(width);
    m_bounds.bottom = m_bounds.top + static_cast<unsigned short>(height);

    m_previous = new W8TextControl(this, 0xffffffff, 3, 3, 0, 0, 0xf4, 0, 0, 2, 1, -1, 3);
    m_previous->m_listener = this;

    m_next = new W8TextControl(this, 0xffffffff, 0x11f, 3, 0, 0, 0xf4, 0, 4, 6, 5, -1, 7);
    m_next->m_listener = this;

    m_page_text = new W8TextBuffer(&m_bounds, &g_empty_wide_string, g_options_detail_font,
                                   g_W8TextBufferAlignCenter | g_W8TextBufferAlignMiddle, 4);
    /* Mod (wide windows): the page strip is stretched together with the panel (covers
       its niche entirely), the arrows move to its ends (region anchors
       are set by W8OptionsScreen::CreateControls), the page number is centered. */
    m_wideMode = W8_PANEL_WIDE_STRETCH;
}

/* The menu-set table has its own deleting destructor; the normal destructor
   clears the inherited Controls children before releasing its page text. */
// FUNCTION: WIZ8 0x005a8e60
W8OptionsMenuSet::~W8OptionsMenuSet()
{
    DestroyAllControls();
    delete m_page_text;
}

// FUNCTION: WIZ8 0x005a8440
void W8OptionsPanel::SetActive(bool active)
{
    EnableRegionSet(active);
    SetEnabled(active);
    Invalidate(0);
}

// FUNCTION: WIZ8 0x005a84c0
void W8OptionsPanel::SetCurrent(int current)
{
    m_current = current;
    Invalidate(0);
}

// FUNCTION: WIZ8 0x005a8b70
void W8OptionsPanelSet::Advance()
{
    int current;

    if (m_page_count == 0) {
        current = m_current;
        if (current < m_panels.GetCount() - 1) {
            (*m_panels.GetAt(current))->SetActive(false);
            current = m_current + 1;
            m_current = current;
            (*m_panels.GetAt(current))->SetActive(true);
        }
    } else if (m_panels[0]->m_current < m_page_count - 1) {
        m_panels[0]->SetCurrent(m_panels[0]->m_current + 1);
    }
}

// FUNCTION: WIZ8 0x005a8c00
void W8OptionsPanelSet::Retreat()
{
    int current;

    if (m_page_count == 0) {
        current = m_current;
        if (current > 0) {
            (*m_panels.GetAt(current))->SetActive(false);
            current = m_current - 1;
            m_current = current;
            (*m_panels.GetAt(current))->SetActive(true);
        }
    } else if (m_panels[0]->m_current > 0) {
        m_panels[0]->SetCurrent(m_panels[0]->m_current - 1);
    }
}

// FUNCTION: WIZ8 0x005a8ed0
void W8OptionsMenuSet::UpdateMenuSet()
{
    W8ControlsRect bounds;
    W8OptionsPanelSet* panel_set;
    int current;
    int count;

    bounds.left = m_bounds.left;
    bounds.top = m_bounds.top;
    bounds.right = m_bounds.right;
    bounds.bottom = m_bounds.bottom;
    int height = m_bounds.bottom - m_bounds.top;
    if (m_pMenuSet == 0) {
        srAssertFail("m_pMenuSet", "C:\\Projects\\Wizardry 8\\Local Screens\\OptionsScreen.cpp",
                     0x5c9, 0);
    }
    panel_set = m_pMenuSet;
    if (panel_set->m_page_count == 0) {
        current = panel_set->m_current;
        count = panel_set->m_panels.GetCount();
    } else {
        current = panel_set->m_panels[0]->m_current;
        count = panel_set->m_page_count;
    }
    bounds.top = panel_set->m_compact_layout != 0 ? 0x180 : 0x1a8;
    bounds.bottom = height + bounds.top;
    SetBounds(bounds.left, bounds.top, bounds.right, bounds.bottom);
    ++bounds.top;
    m_page_text->SetLayoutBounds(&bounds, true, true);
    SetEnabled(panel_set->m_hide_navigation == 0 &&
               (panel_set->m_compact_layout != 0 || count > 1));
    if (m_fEnabled) {
        m_next->SetEnabled(current < count - 1);
        m_previous->SetEnabled(current > 0);
        wchar_t text[0x10];

        swprintf(text, L"%d / %d", current + 1, count);
        m_page_text->SetText(text, g_options_detail_font);
    }
    Invalidate(0);
}

/* The chrome listener is attached at the secondary base subobject.  The
   preceding control walks back and all other primary activations walk forward;
   both paths then recompute bounds, navigation availability, and page text. */
// FUNCTION: WIZ8 0x005a9050
void W8OptionsMenuSet::OnPrimary(W8TextControl* control)
{
    if (control == m_previous) {
        m_pMenuSet->Retreat();
    } else {
        m_pMenuSet->Advance();
    }
    UpdateMenuSet();
}

/* The dialog callback's teardown decision is part of the Options screen's
   state transition: only an accepted close outside combat saves an active
   party, then the common audio-state update runs for every accepted close. */
// FUNCTION: WIZ8 0x005a9ac0
void W8OptionsScreen::OnDialogClosed(bool accepted, int)
{
    if (accepted) {
        if (g_status.game_started && !gXStatus.fCombatMode && AnyCharacterActive()) {
            AutoSaveIfAllowed(true);
        }
        RequestExitScreen();
    }
}

/* The selection-listener slot receives the originating control and its selected
   row.  The retail thunk deliberately ignores the control and chooses the
   panel without emitting another notification. */
// FUNCTION: WIZ8 0x005a98a0
void W8OptionsScreen::OnSelectionChanged(W8ControlSelection*, int selected)
{
    SelectPanel(selected, false);
}

/* State 10 first transfers its option values back to the shared settings
   block, then releases every controller-owned control before returning the
   display and region systems to their common screen boundary. */
// FUNCTION: WIZ8 0x005a9c70
unsigned char OptionsScreenLeave(int)
{
    g_options_values.applying = 1;
    g_options_values.TransferSettings();
    W8OptionsScreen* screen = g_options_screen;
    if (screen != 0) {
        delete screen;
    }
    g_options_screen = 0;
    ResetDisplaySettingsUi(); /* mod: panels deleted */
    /* Mod (wide windows): turn the canvas layout off (the strips are erased). */
    if (CanvasWideLayoutActive()) {
        CanvasSetWideLayout(false);
    }
    NoOp();
    MSYS_Shutdown();
    ResetRegions();
    return 1;
}

// FUNCTION: WIZ8 0x005a9b00
unsigned char OptionsScreenInitialize(void)
{
    if (!g_options_panel_region_sets) {
        g_options_panel_region_sets = new unsigned int[14];
        memset(g_options_panel_region_sets, 0, 0x38);
    }
    return 1;
}

/* The finalizer half of the pair above, and the fifth dword of lifecycle record
   10 - which is what establishes that the record's last slot is the finalizer
   rather than a second initializer: record 10's first slot allocates this exact
   block and its fifth releases it. The release is unguarded and leaves the
   pointer set, so it relies on running once at shutdown. */
// FUNCTION: WIZ8 0x005a9b30
unsigned char OptionsScreenFinalize(void)
{
    delete[] g_options_panel_region_sets;
    return 1;
}

/* State 10 creates the controller after resetting the shared 2D screen
   systems.  The selected panel is determined by the transition mode, with the
   in-game branch preserving the combat and save-state overrides. */
// FUNCTION: WIZ8 0x005a9b50
unsigned char OptionsScreenEnter()
{
    int selected;

    SetViewport(0, 0, 0x280, 0x1e0);
    SetPrimarySurfaceTextureHint2Enabled(false);
    MSYS_Init();
    ResetRegions();
    /* Mod (wide windows): the options window covers the whole canvas (see canvas.h). */
    if (g_canvas_ox > 0 && CanvasWideWindowsEnabled()) {
        CanvasSetWideLayout(true);
    }
    UpdateHeldItemCursor();
    SetFontObjectPalette16BPP(g_wiz_text_font_secondary, g_wiz_text_font_secondary_palette);
    g_options_values.applying = 0;
    g_options_values.TransferSettings();
    g_options_screen = new W8OptionsScreen();
    g_options_screen->CreateControls();

    if (g_current_screen_state.mode == 1) {
        selected = 4;
    } else if (g_current_screen_state.mode == 2) {
        selected = 5;
    } else if (g_current_screen_state.mode == 3) {
        if (g_last_options_panel == 4) {
            selected = 5;
        } else {
            selected = g_last_options_panel;
            if (g_last_options_panel == 5) {
                if (gXStatus.fCombatMode) {
                    selected = 4;
                }
                if (g_status.iron_man) {
                    selected = 0;
                }
            }
        }
    } else {
        selected = 0;
    }
    g_options_screen->SelectPanel(selected, true);
    g_options_first_frame = true;
    return 1;
}

/* The state-10 frame owns modal retirement, input refusal order and redraw
   sequencing.  Options-specific input gets first refusal, followed by shared
   region dispatch; only an unhandled Escape requests the screen transition. */
// FUNCTION: WIZ8 0x005a9cc0
void OptionsScreenFrame()
{
    POINT point;
    POINT current;
    InputAtom input;

    if (g_dev_mode) {
        RequestExitScreen();
    }
    RepositionAmbientSounds(g_world);
    UpdateAmbientSounds(g_world);
    ServiceMusicPlaylist();
    SGPMouseGetPos(&point);

    W8OptionsScreen* screen = g_options_screen;
    if (screen->m_text_editor != 0) {
        SGPMouseGetPos(&current);
        MSYS_SGP_Mouse_Handler_Hook(MOUSE_POS, static_cast<unsigned short>(current.x),
                                    static_cast<unsigned short>(current.y), gfLeftButtonState,
                                    gfRightButtonState);
        screen = g_options_screen;
    }

    W8MessageDialogBase** active_modal = &screen->m_active_modal;
    if (*active_modal != 0) {
        if (!(*active_modal)->ProcessInput()) {
            if (!screen->m_modal_closing) {
                delete *active_modal;
                *active_modal = 0;
            }
            screen->m_redraw_pending = true;
            screen->m_controls->Invalidate(0);
            if (screen->m_selected_panel != -1) {
                W8OptionsPanelSet* panel_set = screen->m_panel[screen->m_selected_panel];
                if (panel_set->m_active) {
                    (*panel_set->m_panels.GetAt(panel_set->m_current))->Invalidate(0);
                }
                screen->m_menu_set->Invalidate(0);
            }
        }
        screen->m_modal_closing = false;
    }

    UpdateRegionMousePosition(point.x, point.y);
    while (DequeueEvent(&input) == 1) {
        if (!screen->ProcessInput(&input) && !DispatchRegionInput(&input) &&
            input.usEvent == KEY_DOWN && input.usParam == VK_ESCAPE) {
            RequestScreenTransition();
        }
    }
    screen->Redraw();
    if (g_options_first_frame) {
        SetRendererAutoFlipEnabled(false);
        RenderFrame();
        SetRendererAutoFlipEnabled(true);
        g_options_first_frame = false;
    }
    RenderFrame();
}

// FUNCTION: WIZ8 0x005A9E70
void SetLastSaveName(const wchar_t* target)
{
    wcsncpy(g_options_last_save_name, target, 0x40);
    reinterpret_cast<char*>(g_options_last_save_name)[0x7e] =
        0; // reinterpret-ok: raw byte view of the wide name buffer
}

// FUNCTION: WIZ8 0x005A9E90
wchar_t* GetLastSaveName(void)
{
    return g_options_last_save_name;
}
