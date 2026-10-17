#pragma once
/* Mod: support for the fan Russian translation "Wizardry8_rus(1_2_6)" (not part
   of the original game, added 2026-10-12, see wiz8-widescreen docs/decomp-port.md
   in the workspace, section "Russian translation").

   The translation replaces data (DATA.SLF with fonts, StringData.DAT, databases,
   NPC scripts) and ships its own Wiz8.exe. Everything its exe does with text
   differently from GOG 1.2.4 is reproduced here and is enabled only when Russian
   data is actually present; with English data the game works as before. */

#include <wchar.h>

/* Called once from InitializeGame, after DATA.SLF is open and before
   fonts are loaded. Decides whether to enable Russian mode:
     wiz8wide.ini, [Text] Cyrillic = auto | 0 | 1   (no key = auto);
     auto: Data\Fonts\Wiz_Text_Font.sti has at least 250 cells (the English
           font has 154, the translation's has 252: Cyrillic in cells 184..249).
   Enabling sets up: the font character table (UseCyrillicFontTranslation), the
   Alt+Shift input layout and letter case (sgp/input.cpp), translated strings
   hardcoded in the exe (see RussianOr and russian_text.cpp). */
void InitializeRussianText(void);

/* TRUE when Russian mode is enabled (same as gfCyrillicText in sgp/Font.h). */
bool IsRussianText(void);

/* A string hardcoded in the game code: in Russian mode the Russian variant,
   otherwise the English one as is. russian is written as plain text (sources are
   UTF-8, /source-charset:utf-8, so in L"..." it is Unicode), and is returned in
   the same encoding as all other game text: cp1251 codes in wchar_t (А = 0x00C0).
   The converted string is cached forever (keyed by the address of russian), so
   the result can be stored and passed around like an ordinary literal. */
const wchar_t* RussianOr(const wchar_t* english, const wchar_t* russian);

/* New mod strings in the language of the game data: Russian (as RussianOr), German
   or French (detected from StringData.DAT strings), otherwise English. German and
   French are Latin-1 in wchar_t, as in the texts of those versions. */
const wchar_t* LocalizedOr(const wchar_t* english, const wchar_t* german, const wchar_t* french,
                           const wchar_t* russian);
