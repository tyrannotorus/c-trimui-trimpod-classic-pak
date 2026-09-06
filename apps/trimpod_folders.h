/* Trimpod: user-managed virtual folders -- Music, Podcasts, Audiobooks -- plus
 * the Browse entry (whole filesystem, opens at the card), all in one browser
 * (see trimpod_folders.c).  Each is a virtual folder merging a user-managed set
 * of source folders plus an unremovable default; all three share one engine. */
#ifndef _TRIMPOD_FOLDERS_H
#define _TRIMPOD_FOLDERS_H

/* Settings -> *Folders pages (MENUITEM_FUNCTION targets) */
int trimpod_music_settings(void);
int trimpod_podcast_settings(void);
int trimpod_audiobook_settings(void);

/* root menu virtual-folder entries (root_menu items[] functions) */
int trimpod_music_browse(void *param);
int trimpod_podcast_browse(void *param);
int trimpod_audiobook_browse(void *param);
/* root menu "Browse": the whole filesystem in the same browser */
int trimpod_files_browse(void *param);

/* root menu "Shuffle Songs": build one recursive playlist of every track in the
 * virtual Music folder, shuffle it and start playing.  Returns GO_TO_WPS. */
int trimpod_shuffle_all(void *param);

/* Virtual-folder categories -- indices shared with trimpod_library.c. */
enum { TP_CAT_MUSIC = 0, TP_CAT_PODCAST, TP_CAT_AUDIOBOOK, TP_CAT_COUNT };

/* Load every category's source-folder list from disk (idempotent).  The library
 * scanner calls this before enumerating roots. */
void trimpod_folders_load_all(void);
/* Enumerate a category's configured source roots. */
int trimpod_folders_root_count(int cat);
const char *trimpod_folders_root_path(int cat, int idx);

#endif /* _TRIMPOD_FOLDERS_H */
