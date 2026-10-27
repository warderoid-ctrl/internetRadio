#pragma once
// Tonal theme tokens (light + dark). See README.md for what each role is for.
// Screens use T->role, never raw hex. Load the theme before building any screen.

#include <Preferences.h>

struct TonalTheme {
  uint32_t bg, surface, surfaceHi, selected, outline;
  uint32_t text, muted;
  uint32_t primary, onPrimary;
  uint32_t secondary, onSecondary;
  uint32_t tertiary, onTertiary;
  uint32_t talk, onTalk;
  uint32_t good, warn, bad;
};

static const TonalTheme THEME_DARK = {
  0x15120E, 0x221E19, 0x2E2923, 0x3D352C, 0x4A4239,
  0xEDE6DD, 0xB3A99D,
  0xFFB59A, 0x3A1305,
  0xD9C2FF, 0x2A1050,
  0xA9DBBC, 0x0B2A1A,
  0xF3D77E, 0x2B2100,
  0x3DDC97, 0xFFC145, 0xFF5C5C,
};

static const TonalTheme THEME_LIGHT = {
  0xEFEEE9, 0xFFFFFF, 0xEFEEE9, 0xE2E1DB, 0xD6D5CF,
  0x121212, 0x6B6A65,
  0xFF6B3D, 0x121212,
  0x121212, 0xFFFFFF,
  0xEFEEE9, 0x121212,
  0x121212, 0xFFFFFF,
  0x1E9E63, 0xB57F00, 0xD93636,   // status colours darkened for contrast on light
};

enum ThemeMode : uint8_t { THEME_MODE_DARK = 0, THEME_MODE_LIGHT = 1 };

static const TonalTheme *T = &THEME_DARK;
static ThemeMode themeMode = THEME_MODE_DARK;

static void themeLoad() {
  Preferences p;
  p.begin("ui", true);
  themeMode = (ThemeMode)p.getUChar("theme", THEME_MODE_DARK);
  p.end();
  T = (themeMode == THEME_MODE_LIGHT) ? &THEME_LIGHT : &THEME_DARK;
}

// First version: persist and restart. Replace with an in-place rebuild later.
static void themeSet(ThemeMode m) {
  Preferences p;
  p.begin("ui", false);
  p.putUChar("theme", (uint8_t)m);
  p.end();
  ESP.restart();
}

// Night clock (ambient.h): the same in both themes
#define T_NIGHT       0xE0503A
#define T_NIGHT_LINE  0x2A0B06
#define T_NIGHT_BG    0x000000

// Layout tokens
#define T_PAD_X      24
#define T_PAD_TOP    20
#define T_PAD_BOTTOM 16
#define T_GAP        12
#define T_R_CARD     36
#define T_R_TILE     28
#define T_R_ROW      20
#define T_R_ACTION   18   // squarer main action button
#define T_NAV_H      76
#define T_TOUCH_MIN  44
