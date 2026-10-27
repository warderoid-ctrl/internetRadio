# Handoff: Crow Hub "Tonal" UI, light and dark

Redesign brief for the Crow Hub firmware (this repo, `warderoid-ctrl/internetRadio`) on the Elecrow CrowPanel Advance 7.0" (800×480, ESP32-S3, LVGL 9.1, GT911 touch).

**Goal:** replace the current look (`ui_common.h` palette, Montserrat, left rail + header) with the *Tonal* design: soft big-radius shapes, colour-filled "container" cards, a pill-shaped bottom nav bar. Ship it with **two themes, dark and light**, selectable in More/Settings and saved in NVS.

## About the design files

`mockups/*.dc.html` are **design references written in HTML/CSS**. They won't render on their own (they need the design canvas runtime), so read them as a spec: every colour, radius, size and gap is in the inline `style="…"` attributes, at the real 800×480 pixel size. Do not port the HTML. Rebuild each screen with LVGL widgets, following the existing patterns in this repo (`panel()`, `mkLabel()`, `textBtn()` in `ui_common.h`, one-translation-unit headers included from `internetRadio.ino`).

| Mockup | Theme | Screen |
|---|---|---|
| `Tonal-Home.dc.html` | dark | Home / Today |
| `Tonal-Podcast.dc.html` | dark | Podcast player + episode list |
| `Tonal-Weather.dc.html` | dark | Weather detail |
| `TonalDay-Home.dc.html` | light | Home / Today |
| `TonalDay-Podcast.dc.html` | light | Podcast player |
| `TonalDay-Weather.dc.html` | light | Weather detail |
| `Daylight-Night.dc.html` | both | Night clock (replaces the current night clock in `ambient.h`) |

Pixel sizes in the mockups are literal: the mockups are 800×480, the same as the panel.

Screens not mocked up (Radio, Find, Home Assistant, Timers, Wi-Fi, Location, More, Rename) should be restyled with the same tokens and components, so they look like siblings of the three above.

## Night clock (both themes)

`Daylight-Night.dc.html` is the night screen whatever the day theme is. It's a StandBy-style clock in **one dim red on pure black**, so the kitchen stays dark at night. It replaces the night-clock look in `ambient.h`; keep that file's existing behaviour (night hours from `config.h`, auto-dim, tap to wake, photo frame by day).

- Background `#000000`. Every element (text, icons, card hairlines) is a single colour, `night` = `#E0503A`.
- Card borders are `#2A0B06`, 1 px.
- The mockup is set in Geist; build it in the Tonal fonts below instead, so it matches the rest of the UI.
- Left: date in small caps (Inter Medium 15, +8% tracking), then the time huge: **Inter Tight Light 236 px**, letter-spacing about −16, digits and `:` only.
- Right column, 200 px wide, two outlined cards, radius 26, padding 16:
  1. Radio sleep timer remaining (`moon` icon, "23 min", station name). Hide this card when no sleep timer is running.
  2. Overnight weather (`cloud` icon, temperature, one-line outlook).
- Below the cards: "TAP TO WAKE" hint (Inter 12).
- Backlight about 8% (`STC_BL` command). Rely on the dimming, not on darker reds: the red is chosen to stay readable when the panel is dimmed.
- No seconds; redraw once a minute, no animation, to avoid burn-in and flicker.

Add `night` (`#E0503A`) and `nightLine` (`#2A0B06`) to the theme. They're identical in both themes.

## Themes: tokens by role

Use **roles**, never raw hex, in screen code. The same role maps to a different colour per theme.

| Role | Dark | Light | Used for |
|---|---|---|---|
| `bg` | `#15120E` | `#EFEEE9` | screen background |
| `surface` | `#221E19` | `#FFFFFF` | cards, nav bar, lists |
| `surfaceHi` | `#2E2923` | `#EFEEE9` | tonal buttons on a surface (prev/next, ±15/30) |
| `selected` | `#3D352C` | `#E2E1DB` | active nav pill, selected list row, slider/progress track |
| `outline` | `#4A4239` | `#D6D5CF` | outlined chips, hairlines |
| `text` | `#EDE6DD` | `#121212` | primary text and icons |
| `muted` | `#B3A99D` | `#6B6A65` | supporting text, inactive nav icons |
| `primary` / `onPrimary` | `#FFB59A` / `#3A1305` | `#FF6B3D` / `#121212` | clock card, main action button, progress fill, "now" chips |
| `secondary` / `onSecondary` | `#D9C2FF` / `#2A1050` | `#121212` / `#FFFFFF` | weather card, podcast artwork tile, selected chip |
| `tertiary` / `onTertiary` | `#A9DBBC` / `#0B2A1A` | `#EFEEE9` / `#121212` | station tile on the now-playing card |
| `talk` / `onTalk` | `#F3D77E` / `#2B2100` | `#121212` / `#FFFFFF` | mic / voice button |
| `good` `warn` `bad` | keep current `C_GOOD` `C_WARN` `C_BAD` | same | status, alarms |

Tints on a filled container (e.g. the "next event" chip inside the clock card, the detail chips inside the weather card) are the container's own `on…` colour at ~10–12% opacity: `bg_color = onPrimary`, `bg_opa = LV_OPA_10`.

`tonal_theme.h` in this folder implements the table above as a runtime struct.

### How theming should work in LVGL

The current palette is `#define`d constants, baked in when widgets are built. To switch themes:

1. Replace the `C_*` defines with a pointer to the active theme (`tonal_theme.h` shows the shape). Keep the old names working during migration if that's easier, e.g. `#define C_BG (T->bg)`.
2. Load the saved theme from NVS (`Preferences`, key `theme`: 0 dark, 1 light) **before** any screen is built.
3. Changing theme = save to NVS, then rebuild the screens (or simply `ESP.restart()` as a first version; a rebuild-in-place can come later).
4. Optional later: auto-switch to dark during the existing ambient/night hours from `config.h`.

## Typography

Montserrat is replaced. Generate LVGL fonts with `lv_font_conv` (4 bpp) from Google Fonts:

| Token | Font | Size | Used for |
|---|---|---|---|
| `F_CLOCK` | Inter Tight SemiBold | 128 | home clock (digits and `:` only) |
| `F_NIGHT` | Inter Tight Light | 236 | night clock (digits and `:` only) |
| `F_HERO` | Inter Tight SemiBold | 112 / 72 | weather temperature (digits, `°`, `-`) |
| `F_TITLE` | Inter Tight SemiBold | 34 / 26 / 22 | episode title, artwork label, station tile |
| `F_L` | Inter SemiBold | 20 | card titles |
| `F_M` | Inter SemiBold | 15–17 | buttons, list rows, nav label |
| `F_S` | Inter Regular/Medium | 13–14 | supporting text |

Rules:
- Big display sizes: generate **only the glyphs used** (digits, `:`, `°`, `−`, `%`) to keep flash use down.
- Body sizes: Basic Latin + Latin-1 (so `°`, `·`, `é` etc. work, and `copyAscii()` can stop stripping them).
- Tracking: the mockups use negative letter-spacing on the big numbers (−0.05em); in LVGL set `text_letter_space` to about −6 at 128 px, −4 at 72–112 px.
- Numbers that tick (clock, progress times) should use tabular figures: generate with `--no-kerning` or check digits are equal-width.

## Icons

**Lucide** (lucide.dev), the same set as terraDial: 24-unit grid, round caps/joins, stroke 2 at 20–24 px, 1.75 at 26–34 px, 1.25–1.5 for the large weather glyph. Bake them as A8 alpha bitmaps so they recolour with `img_recolor` to the role colour. terraDial already has `tools/gen_lucide_icon.py` for exactly this; reuse it.

Icons used: `grid-2x2` (Today), `radio`, `podcast`, `cloud-sun`, `cloud-rain`, `cloud`, `sun`, `house`, `timer`, `image`, `mic`, `calendar`, `play`, `pause`, `skip-back`, `skip-forward`, `chevron-left`, `moon`, `wind`, `umbrella`, `droplets`, `sunrise`, `lightbulb`, `plug`, `thermometer`, `lock`, `tv`, `volume-2`, `wifi`, `search`, `settings`.

## Layout and components

Grid: 24 px outer side padding, 20 px top, 16–20 px bottom, **12 px gap** between cards. The old left rail (`RAIL_W`) and header (`HDR_H`) go away.

- **Container card:** radius 36, padding 20–26, filled with a role colour (`primary`, `secondary`) or `surface`.
- **Bottom nav bar** (Home screen): 76 px tall, radius 38, `surface` fill, 8 px padding. The active destination is a 60 px pill (`selected` fill) with icon + label; the others are icon-only, `muted`. Far right: the talk button, 92×60, radius 20, `talk` fill. Destinations: Today, Radio, Podcasts, Weather, Home Assistant, Timers, Photos.
- **Sub-screens** (Podcast, Weather, …) have no nav bar. They show a 48 px round back button top-left (`surfaceHi`, or the `on…` tint inside a container).
- **Buttons:** 48–64 px tall (minimum touch target 44 px).
  - The main action is `primary`, radius 16–22 (squarer).
  - Secondary actions are `surfaceHi`, fully round (pill).
  - Pressed state: radius morphs rounder (pills stay round) plus `LV_OPA_80`. Use a 120 ms style transition.
- **Chips:** 44 px tall, radius 22. Selected chips are `secondary`; unselected chips are 1 px `outline`.
- **List rows:** min 68 px, radius 20. The selected row is `selected` fill.
- **Progress / scrub:** 8 px track (`selected`), `primary` fill, 6×28 handle (`primary` dark, `text` light).
- **Forecast range bars:** 10 px track, `primary` segment from low to high temperature.

No gradients, shadows or blur anywhere (cheap to render, and it's the look).

## Content

All content comes from the existing modules; nothing new is needed for this pass:
- Clock and date: `rtc.h`
- Next event: `agenda.h`
- Weather: `weather.h`
- Now playing: `radio_ui.h` / `stations.h`
- Podcasts: `podcasts.h`
- Tiles: `config.h` `HA_TILES[]`

Placeholders in the mockups like `[Next calendar event]` and `[Episode title]` are live data.

## Suggested order of work

1. Add `tonal_theme.h`, the NVS theme load, and swap `ui_common.h` to role colours. Build and flash: everything should still work, just recoloured.
2. Generate fonts and icons, and add them to the build.
3. Rebuild Home (layout + bottom nav), then Podcast, then Weather to match the mockups.
4. Restyle the remaining screens with the same components.
5. Add the theme toggle to More, then check both themes on hardware. Check light mode at low backlight; pure white `surface` may want to drop to `#F7F6F2` on this panel.
