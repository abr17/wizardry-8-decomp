/* Experimental (2026-10-20): the "Experimental" category of the Options screen. Not part of
   the original game; see include/wiz8/experimental.h for the rules of the experimental
   features. Its page lists the experimental switches.

   The left column gets a 5th category button ("experimental" and the Russian label) and the
   computer picture a USB drive that lights up with it (art approved by the patch author,
   2026-10-20). The art lives in Data\WIDE\ (patch-owned files; the originals stay untouched,
   so the game without the patch is unchanged):
     OPTIONS_BASE_EXP.STI        the column frame with a button area one row taller;
     OPTIONS_COMPUTER_EXP.STI    the computer picture, 239x163, frame 5 = the drive lit;
     OPTIONS_COMPUTER_EXP169.STI the same at the original 239x169 (for the fallback below);
     OPTIONS_BUTTONS_EXP_EN.STI / _RU.STI  the original buttons + 3 sprites of the new one.
   The catalog entries of the three pictures are pointed at these files when the Options
   screen opens. The button pictures carry the labels of one language, so they are used only
   when the game's own options_buttons.sti is the known English or Russian one (CRC32);
   otherwise (other languages, missing files) the original column stays and the drive alone,
   an unlabelled click area, opens the category. */
#include "wiz8/experimental.h"
#include "wiz8/canvas.h"
#include "wiz8/fonts.h"
#include "wiz8/regions.h"
#include "wiz8/russian_text.h"
#include "wiz8/video_object_catalog.h"
#include "wiz8/wiz8_windows.h"
#include "wiz8/local_code/ControlSelection.h"
#include "wiz8/local_code/TextBuffer.h"
#include "wiz8/local_code/TextControl.h"
#include "wiz8/local_screens/OptionsScreen.h"

#include "FileMan.h"
#include "vobject.h"
#include "zlib.h"

#include <string.h>

namespace {

enum { CATALOG_BASE = 0xee, CATALOG_BUTTONS = 0xef, CATALOG_COMPUTER = 0xf0 };

/* CRC32 of the original Data\Options\options_buttons.sti (as the game reads it). */
const unsigned long kButtonsCrcEnglish = 0x61B4571Eul; /* GOG/Steam 1.2.4 DATA.SLF, 199694 bytes */
const unsigned long kButtonsCrcRussian = 0x1D41BD3Cul; /* Wizardry8_rus 1.2.6 DATA.SLF, 199694 bytes */

/* The drive in the computer picture (640-space; the column is left-anchored, so the same at
   every canvas width): body and shadow (37,155)-(89,172). */
const W8ControlsRect kDriveBounds = {37, 155, 90, 173};

/* Layout of the 5 small buttons: the original 4 at these tops (originally 194/220/246/272),
   the new one below them; 24 px tall, 2 px apart. */
const int kButtonTop[5] = {181, 207, 233, 259, 285};

enum Layout { LAYOUT_UNKNOWN, LAYOUT_FIVE_BUTTONS, LAYOUT_DRIVE_ONLY, LAYOUT_PLAIN };
Layout s_layout = LAYOUT_UNKNOWN; /* decided once per session */
const char* s_buttons_file;       /* the language's button file for LAYOUT_FIVE_BUTTONS */
W8OptionsMenuRow s_rows[4];       /* shifted copies of the original rows */
W8OptionsMenuRow s_entry_row;     /* the new category's row */

bool Exists(const char* path)
{
    return FileExists(const_cast<char*>(path)) != FALSE;
}

W8VideoFrame* CatalogFrame(int object)
{
    return &g_video_frames[g_video_slots[object].first_frame];
}

unsigned long FileCrc(const char* path)
{
    HWFILE file = FileOpen(const_cast<char*>(path), FILE_ACCESS_READ, FALSE);
    if (file == 0) {
        return 0;
    }
    UINT32 size = FileGetSize(file);
    unsigned char* data = new unsigned char[size != 0 ? size : 1];
    UINT32 read = 0;
    BOOLEAN ok = FileRead(file, data, size, &read);
    FileClose(file);
    unsigned long crc = ok && read == size ? crc32(crc32(0L, Z_NULL, 0), data, size) : 0;
    delete[] data;
    return crc;
}

/* Point a catalog picture at another file (a loaded copy of the old one is released). */
void PointCatalogAt(int object, const char* path)
{
    W8VideoFrame* frame = CatalogFrame(object);
    if (strcmp(frame->path, path) == 0 || strlen(path) >= sizeof(frame->path)) {
        return;
    }
    if (frame->loaded) {
        DeleteVideoObjectFromIndex(frame->handle);
        frame->handle = 0;
        frame->loaded = false;
    }
    strcpy(frame->path, path);
}

void DecideLayout(void)
{
    if (s_layout != LAYOUT_UNKNOWN) {
        return;
    }
    /* the original button file, read before its catalog entry is ever repointed */
    unsigned long crc = FileCrc(CatalogFrame(CATALOG_BUTTONS)->path);
    const char* buttons = crc == kButtonsCrcEnglish   ? "Data\\WIDE\\OPTIONS_BUTTONS_EXP_EN.STI"
                          : crc == kButtonsCrcRussian ? "Data\\WIDE\\OPTIONS_BUTTONS_EXP_RU.STI"
                                                      : 0;
    if (buttons != 0 && Exists(buttons) && Exists("Data\\WIDE\\OPTIONS_BASE_EXP.STI") &&
        Exists("Data\\WIDE\\OPTIONS_COMPUTER_EXP.STI")) {
        s_layout = LAYOUT_FIVE_BUTTONS;
        s_buttons_file = buttons;
    } else if (Exists("Data\\WIDE\\OPTIONS_COMPUTER_EXP169.STI")) {
        s_layout = LAYOUT_DRIVE_ONLY;
    } else {
        s_layout = LAYOUT_PLAIN;
    }
    char text[160];
    wsprintfA(text, "[Experimental] options_buttons.sti CRC32 %08lx: %s", crc,
              s_layout == LAYOUT_FIVE_BUTTONS ? (crc == kButtonsCrcEnglish ? "5 buttons (English)" : "5 buttons (Russian)")
              : s_layout == LAYOUT_DRIVE_ONLY ? "unknown language or files: the drive only"
                                              : "no art: an unmarked click area");
    CanvasLogLine(text);
}

/* Region set of panel index 14 (one per session, as the original's array entries). */
unsigned int s_region_set;
/* The checkboxes' values (the widget stores its state through this pointer). */
int s_animated_menu_ui;
int s_modern_map_ui;
int s_original_fixes_ui;
int s_frame_rate_fix_ui;

/* A switch saved to wiz8wide.ini as soon as it is clicked. */
class W8ExperimentalCheckbox : public W8OptionsCheckbox {
public:
    W8ExperimentalCheckbox(Controls* owner, int top, int* value, void (*save)(bool))
        : W8OptionsCheckbox(owner, top, value), m_save(save)
    {
    }
    virtual void OnLeftButtonUp(int event) override
    {
        W8OptionsCheckbox::OnLeftButtonUp(event);
        m_save(*m_value != 0);
    }

private:
    void (*m_save)(bool);
};

class W8OptionsExperimentalPanel : public W8OptionsPanel {
public:
    W8OptionsExperimentalPanel() : W8OptionsPanel(W8_EXPERIMENTAL_OPTIONS_PANEL) {}
    virtual void Populate() override;

private:
    /* Text render mode of the options font. */
    enum { TEXT_NORMAL = 4 };

    /* Name on the left, checkbox at the right edge, 44 px per row (as AddCheckbox). */
    void AddSwitch(const wchar_t* label, int* value, bool enabled, void (*save)(bool))
    {
        AddText(label, 20, 0, 0, 22, g_W8TextBufferAlignLeft, TEXT_NORMAL);
        *value = enabled ? 1 : 0; /* read by the constructor */
        W8OptionsCheckbox* checkbox = new W8ExperimentalCheckbox(this, m_content_top, value, save);
        if (checkbox->m_region != -1) {
            SetRegionLayoutAnchor(static_cast<unsigned int>(checkbox->m_region), W8_ANCHOR_RIGHT);
        }
        m_content_top += 44;
    }
    W8TextBuffer* AddText(const wchar_t* text, int left, int right, int offset, int height,
                          unsigned int align, int mode)
    {
        const int top = m_bounds.top + m_content_top + offset;
        W8ControlsRect bounds = {m_bounds.left + left, top, m_bounds.right - right,
                                 height > 0 ? top + height : m_bounds.bottom};
        W8TextBuffer* buffer = new W8TextBuffer(&bounds, const_cast<wchar_t*>(text), g_options_detail_font,
                                                g_W8TextBufferAlignTop | align, mode);
        m_text_buffers.Add(buffer);
        return buffer;
    }
};

void W8OptionsExperimentalPanel::Populate()
{
    /* heading, like the keyboard pages' titles */
    AddText(LocalizedOr(L"Experimental", L"Experimentell", L"Expérimental", L"Экспериментальные"), 30,
            30, 0, 22, g_W8TextBufferAlignCenter, TEXT_NORMAL);
    m_content_top += 44;

    /* the animated menu is saved at once and used by the next menu; the others apply at once */
    AddSwitch(LocalizedOr(L"Animated Main Menu", L"Animiertes Hauptmenü", L"Menu principal animé",
                          L"Анимированное главное меню"),
              &s_animated_menu_ui, ExperimentalAnimatedMenuEnabled(), ExperimentalSetAnimatedMenu);
    AddSwitch(LocalizedOr(L"Map Mouse Controls", L"Kartensteuerung per Maus", L"Carte à la souris",
                          L"Управление картой мышью"),
              &s_modern_map_ui, ExperimentalModernMapEnabled(), ExperimentalSetModernMap);
    AddSwitch(LocalizedOr(L"Original Bug Fixes", L"Fehlerkorrekturen des Originals",
                          L"Corrections du jeu original", L"Исправления ошибок оригинала"),
              &s_original_fixes_ui, ExperimentalOriginalFixesEnabled(), ExperimentalSetOriginalFixes);
    AddSwitch(LocalizedOr(L"Alternative Game Tick", L"Alternativer Spieltakt",
                          L"Cadence de jeu alternative", L"Альтернативный игровой такт"),
              &s_frame_rate_fix_ui, ExperimentalFrameRateFixEnabled(), ExperimentalSetFrameRateFix);
    m_content_top += 22;

    /* what "experimental" means */
    W8TextBuffer* note = AddText(
        LocalizedOr(L"These features are off by default, may change, and may be removed in a later version.",
                    L"Diese Funktionen sind standardmäßig aus, können sich ändern und in einer "
                    L"späteren Version entfallen.",
                    L"Ces fonctions sont désactivées par défaut, peuvent changer et disparaître "
                    L"dans une version ultérieure.",
                    L"Эти функции по умолчанию выключены, могут измениться и исчезнуть в следующих версиях."),
        30, 30, 0, 0, g_W8TextBufferAlignCenter, TEXT_NORMAL);
    note->SetLineHeight(22);
}

} // namespace

void ExperimentalOptionsPrepareArt(void)
{
    DecideLayout();
    if (s_layout == LAYOUT_FIVE_BUTTONS) {
        PointCatalogAt(CATALOG_BASE, "Data\\WIDE\\OPTIONS_BASE_EXP.STI");
        PointCatalogAt(CATALOG_BUTTONS, s_buttons_file);
        PointCatalogAt(CATALOG_COMPUTER, "Data\\WIDE\\OPTIONS_COMPUTER_EXP.STI");
    } else if (s_layout == LAYOUT_DRIVE_ONLY) {
        PointCatalogAt(CATALOG_COMPUTER, "Data\\WIDE\\OPTIONS_COMPUTER_EXP169.STI");
    }
}

const W8OptionsMenuRow* ExperimentalOptionsRow(int index, const W8OptionsMenuRow* original)
{
    if (s_layout != LAYOUT_FIVE_BUTTONS || index < 0 || index >= 4) {
        return original;
    }
    s_rows[index] = *original;
    s_rows[index].bounds.top = kButtonTop[index];
    return &s_rows[index];
}

void ExperimentalOptionsAddEntry(Controls* controls, W8ControlSelection* selection)
{
    static const W8ControlsRect kNone = {-1, -1, -1, -1};
    W8OptionsMenuRow* row = &s_entry_row;
    /* item id = the computer picture frame drawn while the category is selected: 5 is the
       drive lit (only in the patch's picture), 0 nothing lit */
    row->item_id = s_layout == LAYOUT_PLAIN ? 0 : 5;
    row->child_bounds[0] = kNone;
    row->child_bounds[1] = kNone;
    for (int i = 0; i < 5; ++i) {
        row->image_indices[i] = -1;
    }
    if (s_layout == LAYOUT_FIVE_BUTTONS) {
        /* a button like the other four (sprites 26 normal, 27 selected, 28 pressed), and the
           drive picture as its second click area, as the tower is for "General" */
        W8ControlsRect bounds = {16, kButtonTop[4], 0, 0};
        row->bounds = bounds;
        row->child_bounds[0] = kDriveBounds;
        row->image_indices[0] = 26;
        row->image_indices[1] = 28;
        row->image_indices[2] = 27;
    } else {
        row->bounds = kDriveBounds; /* an unlabelled click area on the drive (or its spot) */
    }
    W8OptionsMenuButton* entry = new W8OptionsMenuButton(controls, row);
    if (s_layout != LAYOUT_FIVE_BUTTONS) {
        entry->AddLayoutFlags(g_W8TextControlSilentHover);
    }
    selection->AddEntry(entry); /* selection index 6 */
}

W8OptionsPanel* ExperimentalOptionsCreatePanel(void)
{
    return new W8OptionsExperimentalPanel();
}

unsigned int* ExperimentalOptionsRegionSet(void)
{
    return &s_region_set;
}

/* ---------------- settings: wiz8wide.ini [Experimental] ---------------- */

void ExperimentalIniPath(char* out, unsigned long size)
{
    PathNextToExe("wiz8wide.ini", out, size);
}

bool ExperimentalReadSwitch(const char* key)
{
    char ini[MAX_PATH];
    ExperimentalIniPath(ini, sizeof(ini));
    return GetPrivateProfileIntA("Experimental", key, 0, ini) != 0;
}

void ExperimentalWriteSwitch(const char* key, bool enabled)
{
    char ini[MAX_PATH];
    ExperimentalIniPath(ini, sizeof(ini));
    WritePrivateProfileStringA("Experimental", key, enabled ? "1" : "0", ini);
}
