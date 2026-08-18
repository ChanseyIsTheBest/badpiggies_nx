/* nx_fonts.c -- give Unity's dynamic-font fallback something to fall back to.
 *
 * THE PROBLEM. Hitman GO renders UI text with UnityEngine.Font in DYNAMIC mode
 * -- `get_dynamic`, `HasCharacter` and `RequestCharactersInTexture` all survive
 * IL2CPP stripping, which they would not if the game used only baked atlases.
 * A dynamic font rasterises glyphs at runtime from its embedded face, and for
 * any codepoint that face does not carry, Unity falls back to the OPERATING
 * SYSTEM's fonts. On Android it finds them by parsing a font config and then
 * loading files out of /system/fonts. libunity's .rodata carries exactly those
 * paths:
 *
 *     /etc/fonts.xml  /etc/system_fonts.xml  /etc/fallback_fonts.xml
 *     /vendor/etc/fallback_fonts.xml  /system/fonts/  DroidSansFallback
 *
 * This port supplied none of them, so the fallback list was empty. The game's
 * own face covers Latin, which is why English rendered and Cyrillic, Japanese,
 * Chinese and Korean came out blank -- the glyphs were never anywhere to find.
 *
 * THE FIX. The Switch ships its own system fonts, reachable through libnx's
 * `pl` service, and they cover precisely the scripts that were missing. This
 * module extracts them once, converts them to plain TTF, caches them under the
 * game directory, and synthesises the Android config files that point Unity at
 * them. Nothing is redistributed: the fonts come off the console at runtime.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "config.h"
#include "nx_fonts.h"
#include "util.h"

#define FONT_DIR_NAME ".fonts"

typedef struct {
  PlSharedFontType type;
  const char *file;      /* name exposed as /system/fonts/<file>          */
  const char *scripts;   /* for the log; what this face is here to cover  */
  int         ok;
} FontEntry;

/* Standard already carries Latin, Cyrillic, Greek, kana and the common kanji;
 * the rest extend CJK coverage. Ordered so the broadest face is the first
 * fallback Unity tries. */
static FontEntry g_fonts[] = {
  { PlSharedFontType_Standard,              "NintendoStandard.ttf",  "Latin/Cyrillic/Greek/kana", 0 },
  { PlSharedFontType_ChineseSimplified,     "NintendoChineseS.ttf",  "Chinese (simplified)",      0 },
  { PlSharedFontType_ExtChineseSimplified,  "NintendoChineseSExt.ttf","Chinese (simplified ext)", 0 },
  { PlSharedFontType_ChineseTraditional,    "NintendoChineseT.ttf",  "Chinese (traditional)",     0 },
  { PlSharedFontType_KO,                    "NintendoKorean.ttf",    "Korean",                    0 },
  { PlSharedFontType_NintendoExt,           "NintendoExt.ttf",       "console glyphs",            0 },
};
#define FONT_COUNT ((int)(sizeof g_fonts / sizeof g_fonts[0]))

static char g_dir[256];
static int  g_ready = 0;

/* --------------------------------------------------------------------------
 * BFTTF -> TTF
 *
 * Nintendo ships these fonts as BFTTF: an sfnt XOR'd with a repeating 32-bit
 * key, behind a short header. Published decoders hardcode the key constant,
 * which is exactly the sort of remembered magic number that corrupts silently
 * when it is wrong -- the output would still be written, just not a font.
 *
 * So derive it from KNOWN PLAINTEXT instead. Every sfnt opens with a 4-byte
 * version tag from a tiny known set, so key = first_word XOR expected_tag. Try
 * each plausible data offset and tag, and accept a candidate only if the
 * decoded header is SELF-CONSISTENT: sfnt tables are indexed by a binary-search
 * record whose searchRange/entrySelector/rangeShift are all derivable from
 * numTables, so a wrong key has to satisfy four coupled constraints at once to
 * slip through. That check needs no magic constant at all and fails loudly.
 * ------------------------------------------------------------------------ */
static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Is `hdr` a plausible sfnt header? 12 bytes: version, numTables, searchRange,
 * entrySelector, rangeShift. */
static int sfnt_header_ok(const uint8_t *hdr, size_t avail) {
  if (avail < 12) return 0;
  const uint32_t ver = be32(hdr);
  if (ver != 0x00010000u && ver != 0x4F54544Fu /*OTTO*/ &&
      ver != 0x74727565u /*true*/ && ver != 0x74746366u /*ttcf*/)
    return 0;
  if (ver == 0x74746366u) {                      /* collection: validate numFonts */
    const uint32_t nf = be32(hdr + 8);
    return nf >= 1 && nf <= 64;
  }
  const uint32_t n = ((uint32_t)hdr[4] << 8) | hdr[5];
  if (n == 0 || n > 512) return 0;
  const uint32_t sr = ((uint32_t)hdr[6] << 8) | hdr[7];
  const uint32_t es = ((uint32_t)hdr[8] << 8) | hdr[9];
  const uint32_t rs = ((uint32_t)hdr[10] << 8) | hdr[11];
  uint32_t p2 = 1, sel = 0;
  while (p2 * 2 <= n) { p2 *= 2; sel++; }
  return sr == p2 * 16 && es == sel && rs == n * 16 - sr;
}

/* Returns the offset at which decoded TTF data begins, or -1. Decodes in place. */
static long bfttf_decode(uint8_t *buf, size_t size) {
  /* Candidates for the FIRST FOUR BYTES of the payload. `ttcf` is deliberately
   * absent: a collection header carries no table directory at a fixed offset,
   * so it cannot be validated the way the others can, and including it made
   * every input "decode" -- the key is derived FROM the tag, so a tag that is
   * accepted unconditionally is a tag that always matches. */
  static const uint32_t tags[] = { 0x00010000u, 0x4F54544Fu, 0x74727565u };
  static const size_t   offs[] = { 8, 0 };   /* 8 is the documented layout; 0 covers raw */

  /* Unencrypted already? Check before deriving anything -- a zero key is a
   * legitimate answer that the derivation loop below has to reject (it cannot
   * XOR by nothing and still prove it did the right thing). */
  for (size_t oi = 0; oi < sizeof offs / sizeof offs[0]; oi++) {
    const size_t off = offs[oi];
    if (size > off + 12 && sfnt_header_ok(buf + off, size - off)) {
      debugPrintf("[font] already plaintext at +%u\n", (unsigned)off);
      return (long)off;
    }
  }

  for (size_t oi = 0; oi < sizeof offs / sizeof offs[0]; oi++) {
    const size_t off = offs[oi];
    if (size < off + 16) continue;
    for (size_t ti = 0; ti < sizeof tags / sizeof tags[0]; ti++) {
      /* key such that (stored ^ key) == tag, reading both big-endian */
      const uint32_t stored = be32(buf + off);
      const uint32_t key = stored ^ tags[ti];
      if (!key) continue;                        /* handled by the plaintext pass */
      uint8_t probe[12];
      for (int i = 0; i < 12; i++) {
        const uint8_t kb = (uint8_t)(key >> (24 - 8 * (i & 3)));
        probe[i] = (uint8_t)(buf[off + i] ^ kb);
      }
      if (!sfnt_header_ok(probe, sizeof probe)) continue;
      /* Committed: apply to the whole payload. */
      for (size_t i = off; i + 3 < size; i += 4) {
        buf[i + 0] ^= (uint8_t)(key >> 24);
        buf[i + 1] ^= (uint8_t)(key >> 16);
        buf[i + 2] ^= (uint8_t)(key >> 8);
        buf[i + 3] ^= (uint8_t)(key);
      }
      debugPrintf("[font] BFTTF key 0x%08x derived at +%u\n", key, (unsigned)off);
      return (long)off;
    }
  }
  return -1;
}

/* --------------------------------------------------------------------------
 * Extraction + cache
 * ------------------------------------------------------------------------ */
static int write_all(const char *path, const void *data, size_t len) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  const uint8_t *p = data;
  size_t left = len;
  while (left) {
    ssize_t n = write(fd, p, left);
    if (n <= 0) { close(fd); return -1; }
    p += n; left -= (size_t)n;
  }
  close(fd);
  return 0;
}

int nx_fonts_init(const char *game_home) {
  if (g_ready) return g_ready;
  snprintf(g_dir, sizeof g_dir, "%s/" FONT_DIR_NAME, game_home);
  mkdir(g_dir, 0777);

  Result rc = plInitialize(PlServiceType_User);
  if (R_FAILED(rc)) {
    debugPrintf("[font] plInitialize failed (0x%x) -- no system fonts, non-Latin "
                "text will not render\n", rc);
    return 0;
  }

  int made = 0;
  for (int i = 0; i < FONT_COUNT; i++) {
    char path[320];
    snprintf(path, sizeof path, "%s/%s", g_dir, g_fonts[i].file);

    /* Cached from a previous boot? Decoding is not free, and this runs before
     * the first frame. */
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) {
      g_fonts[i].ok = 1; made++;
      continue;
    }

    PlFontData fd;
    rc = plGetSharedFontByType(&fd, g_fonts[i].type);
    if (R_FAILED(rc) || !fd.address || fd.size < 16) {
      debugPrintf("[font] type %d (%s) unavailable (0x%x)\n",
                  (int)g_fonts[i].type, g_fonts[i].scripts, rc);
      continue;
    }
    uint8_t *buf = malloc(fd.size);
    if (!buf) { debugPrintf("[font] out of memory for %s\n", g_fonts[i].file); continue; }
    memcpy(buf, fd.address, fd.size);

    const long off = bfttf_decode(buf, fd.size);
    if (off < 0) {
      /* Loud, and nothing written: a half-decoded file on disk would be served
       * to Unity as a font and fail somewhere much less obvious. */
      debugPrintf("[font] *** %s: could not decode BFTTF (no candidate key gave a "
                  "consistent sfnt header) -- skipped\n", g_fonts[i].file);
      free(buf);
      continue;
    }
    if (write_all(path, buf + off, fd.size - (size_t)off) == 0) {
      g_fonts[i].ok = 1; made++;
      debugPrintf("[font] %s <- shared font %d (%s), %u KB\n",
                  g_fonts[i].file, (int)g_fonts[i].type, g_fonts[i].scripts,
                  (unsigned)((fd.size - (size_t)off) >> 10));
    } else {
      debugPrintf("[font] could not write %s\n", path);
    }
    free(buf);
  }
  plExit();

  g_ready = made;
  if (made) {
    /* Materialise the config as a REAL FILE next to the faces. Every filesystem
     * entry point can then be served by simple path substitution, instead of
     * each one needing its own synthetic-content special case -- which is what
     * the first version got wrong: the redirect lived only in open_fake, and
     * Unity probes with stat/access/fopen first, got ENOENT, and never called
     * open() at all. */
    char xp[320];
    snprintf(xp, sizeof xp, "%s/fonts.xml", g_dir);
    const char *xml = nx_fonts_config_xml();
    if (write_all(xp, xml, strlen(xml)) != 0)
      debugPrintf("[font] could not write %s -- Unity will not find the fallback list\n", xp);
  }
  debugPrintf("[font] %d/%d system fonts available for Unity's dynamic fallback\n",
              made, FONT_COUNT);
  return made;
}

/* --------------------------------------------------------------------------
 * The Android side of the illusion
 * ------------------------------------------------------------------------ */
/* Map an Android font path onto the real cached file, or return `path`
 * unchanged. Called from EVERY filesystem entry point (open/openat/stat/
 * access/fopen/opendir), because Unity does not commit to one: it probes before
 * it reads, and a redirect that only covers open() is never reached.
 *
 * Directories resolve too, so opendir("/system/fonts") enumerates the cache
 * naturally rather than needing a synthetic listing. */
static void nx_fonts_note_probe(const char *want, const char *got);  /* below */

const char *nx_fonts_resolve(const char *path) {
  if (!g_ready || !path) return path;
  static char out[320];

  if (nx_fonts_is_config_path(path)) {
    snprintf(out, sizeof out, "%s/fonts.xml", g_dir);
    nx_fonts_note_probe(path, out);
    return out;
  }
  if (!strcmp(path, "/system/fonts") || !strcmp(path, "/system/fonts/")) {
    snprintf(out, sizeof out, "%s", g_dir);
    nx_fonts_note_probe(path, out);
    return out;
  }
  static const char *pfx = "/system/fonts/";
  if (!strncmp(path, pfx, strlen(pfx))) {
    const char *name = path + strlen(pfx);
    for (int i = 0; i < FONT_COUNT; i++) {
      if (g_fonts[i].ok && !strcmp(name, g_fonts[i].file)) {
        snprintf(out, sizeof out, "%s/%s", g_dir, g_fonts[i].file);
        nx_fonts_note_probe(path, out);
        return out;
      }
    }
    /* Unity asked for a face we do not have (DroidSansFallback, Roboto...).
     * Serve the broadest one we do have rather than nothing: the fallback list
     * is about glyph coverage, not about the exact file name. */
    for (int i = 0; i < FONT_COUNT; i++) {
      if (g_fonts[i].ok) {
        snprintf(out, sizeof out, "%s/%s", g_dir, g_fonts[i].file);
        nx_fonts_note_probe(path, out);
        return out;
      }
    }
  }
  return path;
}

/* Log the first hit on each distinct path. Boot 1 of the font work produced
 * "6/6 system fonts available" and then complete silence -- fonts present,
 * nothing asking for them -- which gave no way to tell "Unity never looks" from
 * "Unity looked through a call we do not intercept". These lines answer that. */
static void nx_fonts_note_probe(const char *want, const char *got) {
  static char seen[12][64];
  static unsigned n;
  for (unsigned i = 0; i < n; i++)
    if (!strcmp(seen[i], want)) return;
  if (n < sizeof seen / sizeof seen[0]) {
    snprintf(seen[n], sizeof seen[n], "%s", want);
    n++;
  }
  debugPrintf("[font] serving %s -> %s\n", want, got);
}

/* Android's fonts.xml. The first named family is what Unity treats as the
 * default face; every subsequent UNNAMED family is a fallback, tried in order
 * for codepoints the previous faces lack -- which is exactly the behaviour the
 * missing glyphs need. Written in the modern (v21+) schema; the legacy
 * system_fonts.xml/fallback_fonts.xml pair is served the same content, since
 * Unity accepts whichever it finds first and a parser that rejects one will
 * simply move on. */
const char *nx_fonts_config_xml(void) {
  static char xml[2048];
  static int built = 0;
  if (built) return xml;
  int o = snprintf(xml, sizeof xml,
                   "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<familyset version=\"21\">\n");
  int first = 1;
  for (int i = 0; i < FONT_COUNT && o > 0 && o < (int)sizeof xml; i++) {
    if (!g_fonts[i].ok) continue;
    if (first) {
      o += snprintf(xml + o, sizeof xml - o,
                    "  <family name=\"sans-serif\">\n"
                    "    <font weight=\"400\" style=\"normal\">%s</font>\n"
                    "  </family>\n", g_fonts[i].file);
      first = 0;
    } else {
      o += snprintf(xml + o, sizeof xml - o,
                    "  <family>\n"
                    "    <font weight=\"400\" style=\"normal\">%s</font>\n"
                    "  </family>\n", g_fonts[i].file);
    }
  }
  o += snprintf(xml + o, sizeof xml - o, "</familyset>\n");
  built = 1;
  return xml;
}

/* Which config paths we answer. Unity tries several; serving all of them costs
 * nothing and avoids depending on which one it reaches first. */
int nx_fonts_is_config_path(const char *path) {
  if (!g_ready || !path) return 0;
  return !strcmp(path, "/etc/fonts.xml") ||
         !strcmp(path, "/system/etc/fonts.xml") ||
         !strcmp(path, "/etc/system_fonts.xml") ||
         !strcmp(path, "/system/etc/system_fonts.xml") ||
         !strcmp(path, "/etc/fallback_fonts.xml") ||
         !strcmp(path, "/system/etc/fallback_fonts.xml") ||
         !strcmp(path, "/vendor/etc/fallback_fonts.xml");
}
