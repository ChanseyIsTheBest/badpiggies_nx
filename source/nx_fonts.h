/* nx_fonts.h -- Switch system fonts exposed as Android system fonts, so Unity's
 * dynamic-font fallback has somewhere to find Cyrillic/CJK glyphs.
 * See nx_fonts.c for why this is needed. */
#ifndef NX_FONTS_H
#define NX_FONTS_H

/* Extract + cache the console's shared fonts under <game_home>/.fonts and build
 * the font-config XML. Safe to call once at boot, before any module loads.
 * Returns the number of faces available (0 = non-Latin text will not render). */
int nx_fonts_init(const char *game_home);

/* Map an Android font path (the config, /system/fonts, or a face) onto the real
 * cached file; returns `path` unchanged if it is not ours.
 *
 * Call this from EVERY filesystem entry point, not just open(): Unity probes
 * with stat/access/fopen before reading, and a redirect that only covers open()
 * is never reached -- which is exactly why the first version cached six fonts
 * and Unity never asked for one. */
const char *nx_fonts_resolve(const char *path);

/* Is this one of the Android font-config paths we synthesize? */
int nx_fonts_is_config_path(const char *path);

/* Generated fonts.xml content (first family = default, rest = fallbacks). */
const char *nx_fonts_config_xml(void);

#endif /* NX_FONTS_H */
