/* config.c -- Bad Piggies config.txt reader/writer.
 * MIT (see LICENSE). Forked from the laytonbmr_nx / colorsheep_nx config loader.
 *
 * Resolution is FORCED to 1920x1080 in main.c (see config.h BP_FORCE_SCREEN_*), so it is
 * deliberately NOT a config key -- there is nothing for the user to set. The only key is
 * `language`.
 */
#include <stdio.h>
#include <string.h>
#include "config.h"

int screen_width  = 0;
int screen_height = 0;

Config config = {
  .language       = "auto",
  .cheats         = 0,
  .unlock_field_of_dreams = 0,
};

/* Returns 0 = read clean, -1 = missing, 1 = present but missing/extra keys
 * (caller rewrites so the file self-heals to the current schema). */
int read_config(const char *file) {
  FILE *f = fopen(file, "r");
  if (!f) return -1;
  int seen = 0;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;

    int ival;
    if (sscanf(line, "cheats=%d", &ival) == 1)         { config.cheats = ival;         seen |= 2; continue; }
    if (sscanf(line, "unlock_field_of_dreams=%d", &ival) == 1) { config.unlock_field_of_dreams = ival; seen |= 8; continue; }

    char sval[64];
    if (sscanf(line, "language=%63s", sval) == 1) {
      size_t n = strlen(sval);
      while (n && (sval[n-1] == '\r' || sval[n-1] == ' ')) sval[--n] = '\0';
      snprintf(config.language, sizeof config.language, "%s", sval);
      seen |= 1;
      continue;
    }
  }
  fclose(f);
  return (seen == 0xB) ? 0 : 1;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (!f) return -1;
  fprintf(f,
    "# Bad Piggies (badpiggies_nx) config\n"
    "\n"
    "# language: 'auto' follows the Switch system language. Or force one of the seven the\n"
    "# game supports: en fr de it es ja zh_CN.\n"
    "language=%s\n"
    "\n"
    "# unlock_field_of_dreams: 1 unlocks the Field of Dreams sandbox (id \"S-F\").\n"
    "#   Same effect as the well-known SetSandboxUnlocked(\"S-F\", true) edit: that flips a\n"
    "#   flag which GameProgress.GetSandboxUnlocked() reads, and this patches that getter.\n"
    "#   NOTE: currently unlocks ALL sandbox levels, not only Field of Dreams.\n"
    "unlock_field_of_dreams=%d\n"

    "\n"
    "# cheats: 1 flips the game's built-in developer cheat flag.\n"
    "#   *** KNOWN BROKEN: the flag is set, but the cheat menu cannot be opened -- its\n"
    "#   entry does not respond in this build's UI. Left in for completeness only.\n"
    "#   Use unlock_field_of_dreams instead. ***\n"
    "cheats=%d\n",
    config.language[0] ? config.language : "auto",
    config.unlock_field_of_dreams, config.cheats);
  fclose(f);
  return 0;
}
