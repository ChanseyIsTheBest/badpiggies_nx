/* config.c -- Color Sheep config.txt reader/writer.
 * MIT (see LICENSE). Forked from the laytonbmr_nx config loader.
 */
#include <stdio.h>
#include <string.h>
#include "config.h"

int screen_width  = 0;
int screen_height = 0;

Config config = {
  .screen_width  = -1,   /* auto */
  .screen_height = -1,
};

/* Returns 0 = read clean, -1 = missing, 1 = present but missing/extra keys
 * (caller rewrites so the file self-heals to the current schema). */
int read_config(const char *file) {
  FILE *f = fopen(file, "r");
  if (!f) return -1;
  int seen = 0;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    char key[64]; int val;
    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, "%63[^=]=%d", key, &val) == 2) {
      if      (!strcmp(key, "screen_width"))  { config.screen_width = val;  seen |= 1; }
      else if (!strcmp(key, "screen_height")) { config.screen_height = val; seen |= 2; }
    }
  }
  fclose(f);
  return (seen == 0x3) ? 0 : 1;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (!f) return -1;
  fprintf(f,
    "# Color Sheep (colorsheep_nx) config\n"
    "# screen_width/height: render resolution; <=0 = auto.\n"
    "screen_width=%d\n"
    "screen_height=%d\n",
    config.screen_width, config.screen_height);
  fclose(f);
  return 0;
}
