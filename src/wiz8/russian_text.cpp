/* Mod: support for the fan Russian translation "Wizardry8_rus(1_2_6)". Not part
   of the original game (added 2026-10-12), described in include/wiz8/russian_text.h. */
#include "wiz8/russian_text.h"

#include "Font.h"
#include "FileMan.h"
#include "wiz8/local_code/Strings.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FormationAndFacing.cpp: formation row names for messages "X переходит в ..." ("X moves to ..."). */
extern wchar_t g_formation_row_names[5][20];

namespace {

/* Number of cells (sub-images) in an STI file: STCI header, 16-bit field
   usNumberOfSubImages at offset 28 (SGP format, as in Jagged Alliance 2). */
int CountStiGlyphs(const char* path)
{
    unsigned char header[64];
    UINT32 read = 0;
    HWFILE file = FileOpen(const_cast<char*>(path), FILE_ACCESS_READ, FALSE);
    if (file == 0) {
        return -1;
    }
    BOOLEAN ok = FileRead(file, header, sizeof(header), &read);
    FileClose(file);
    if (!ok || read != sizeof(header) || memcmp(header, "STCI", 4)) {
        return -1;
    }
    return header[28] | (header[29] << 8);
}

void PathNextToExe(const char* name, char* out, size_t size)
{
    char module[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, module, sizeof(module));
    if (length == 0 || length >= sizeof(module)) {
        module[0] = '\0';
    } else {
        char* slash = strrchr(module, '\\');
        if (slash != 0) {
            slash[1] = '\0';
        } else {
            module[0] = '\0';
        }
    }
    _snprintf(out, size, "%s%s", module, name);
    out[size - 1] = '\0';
}

/* Unicode -> "cp1251 codes in wchar_t", like the rest of the game text. */
void ToGameText(const wchar_t* unicode, wchar_t* out, size_t count)
{
    size_t i = 0;
    for (; unicode[i] != 0 && i + 1 < count; ++i) {
        wchar_t c = unicode[i];
        if (c >= 0x80) {
            char byte = '?';
            WideCharToMultiByte(1251, 0, &c, 1, &byte, 1, NULL, NULL);
            c = static_cast<unsigned char>(byte);
        }
        out[i] = c;
    }
    out[i] = 0;
}

struct CachedText {
    const wchar_t* russian;
    wchar_t* game_text;
};

CachedText g_cache[96];
int g_cache_count;

} // namespace

bool IsRussianText(void)
{
    return gfCyrillicText != FALSE;
}

const wchar_t* RussianOr(const wchar_t* english, const wchar_t* russian)
{
    if (!gfCyrillicText) {
        return english;
    }
    for (int i = 0; i < g_cache_count; ++i) {
        if (g_cache[i].russian == russian) {
            return g_cache[i].game_text;
        }
    }
    if (g_cache_count == sizeof(g_cache) / sizeof(g_cache[0])) {
        return english; // should not happen: fewer strings than slots
    }
    size_t count = wcslen(russian) + 1;
    wchar_t* text = static_cast<wchar_t*>(malloc(count * sizeof(wchar_t)));
    if (text == 0) {
        return english;
    }
    ToGameText(russian, text, count);
    g_cache[g_cache_count].russian = russian;
    g_cache[g_cache_count].game_text = text;
    ++g_cache_count;
    return text;
}

/* Data language for new mod strings. Russian - by the font (above); German and
   French - by the settings and save menu strings in StringData.DAT
   (0x800..0x8ff): «Spiel» / «partie», «jeu». Otherwise English. */
static bool ContainsText(const wchar_t* text, const wchar_t* word)
{
    for (; *text != 0; ++text) {
        const wchar_t* a = text;
        const wchar_t* b = word;
        while (*b != 0 && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == 0) {
            return true;
        }
    }
    return false;
}

static int DetectDataLanguage(void)
{
    static int language = -1; /* 0 en, 1 de, 2 fr */
    if (language >= 0) {
        return language;
    }
    language = 0;
    if (gppStringList == 0) {
        return language;
    }
    int german = 0;
    int french = 0;
    for (int id = 0x800; id < 0x900; ++id) {
        const wchar_t* text = gppStringList[id];
        if (text == 0) {
            continue;
        }
        german += ContainsText(text, L"Spiel") || ContainsText(text, L"spiel");
        french += ContainsText(text, L"partie") || ContainsText(text, L"Partie") ||
                  ContainsText(text, L"jeu") || ContainsText(text, L"Jeu");
    }
    if (german >= 2 && german > french) {
        language = 1;
    } else if (french >= 2) {
        language = 2;
    }
    return language;
}

const wchar_t* LocalizedOr(const wchar_t* english, const wchar_t* german, const wchar_t* french,
                           const wchar_t* russian)
{
    if (gfCyrillicText) {
        return RussianOr(english, russian);
    }
    switch (DetectDataLanguage()) {
    case 1:
        return german;
    case 2:
        return french;
    default:
        return english;
    }
}

void InitializeRussianText(void)
{
    char ini[MAX_PATH];
    char value[16];
    PathNextToExe("wiz8wide.ini", ini, sizeof(ini));
    GetPrivateProfileStringA("Text", "Cyrillic", "auto", value, sizeof(value), ini);

    int glyphs = CountStiGlyphs("Data\\Fonts\\Wiz_Text_Font.sti");
    bool enable;
    if (_stricmp(value, "1") == 0) {
        enable = true;
    } else if (_stricmp(value, "0") == 0) {
        enable = false;
    } else {
        /* Cyrillic is drawn in cells 184..249: without them it must not be enabled,
           otherwise Russian letters would refer to nonexistent cells. */
        enable = glyphs >= 250;
    }

    if (enable) {
        UseCyrillicFontTranslation();
        /* As in the translation's exe: these names live in the exe's data, not in
           StringData.DAT. Everywhere else - RussianOr directly in code. */
        static const wchar_t* const kRowNames[5] = {
            L"Фронт", L"Правый фланг", L"Тыл", L"Левый фланг", L"Центр",
        };
        for (int row = 0; row < 5; ++row) {
            ToGameText(kRowNames[row], g_formation_row_names[row], 20);
        }
    }

    /* Line for the wide canvas log (Canvas.cpp creates it earlier). */
    char log_path[MAX_PATH];
    PathNextToExe("wiz8wide.log", log_path, sizeof(log_path));
    FILE* log = fopen(log_path, "a");
    if (log != 0) {
        fprintf(log, "text: Cyrillic=%s, Data\\Fonts\\Wiz_Text_Font.sti glyphs=%d -> %s\n", value,
                glyphs, gfCyrillicText ? "Russian (cp1251 font table, Alt+Shift layout)" : "English");
        fclose(log);
    }
}
