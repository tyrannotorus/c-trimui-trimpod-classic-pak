/* Trimpod: .m4b audiobook support -- per-file resume positions and embedded
 * chapter extraction (fed into the cuesheet engine).  See trimpod_m4b.c. */

#ifndef TRIMPOD_M4B_H
#define TRIMPOD_M4B_H

#include <stdbool.h>

struct mp3entry;
struct cuesheet;

bool trimpod_is_m4b(const char *path);

/* --- per-file resume positions (persisted in ROCKBOX_DIR) --- */
void trimpod_m4b_resume_init(void);
bool trimpod_m4b_resume_get(const char *path,
                            unsigned long *elapsed, unsigned long *offset);
void trimpod_m4b_resume_note(const struct mp3entry *id3);

/* --- chapters: fill `cue` from the file's chapter track / chpl atom --- */
bool trimpod_m4b_load_chapters(const char *path, struct cuesheet *cue);

#endif
