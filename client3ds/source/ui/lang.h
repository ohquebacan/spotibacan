/* UI strings, Spanish, English and Brazilian Portuguese. One flat enum, one
 * table; tr() reads the current language. The choice persists as lang= in
 * spotify.cfg. */
#pragma once

typedef enum {
	LANG_ES = 0,
	LANG_EN,
	LANG_PT,
	LANG_COUNT
} lang_id;

typedef enum {
	S_NOTHING_PLAYING,
	S_STATE_LOADING,
	S_STATE_PAUSED,
	S_STATE_MUTED,
	S_STATE_PLAYING,
	S_LIKED,
	S_LOADING,
	S_LOADING_DOTS,
	S_EMPTY_LIST,
	S_QUIT_TITLE,
	S_CANCEL,
	S_QUIT,
	S_PLAYLIST_FALLBACK,
	S_RADIO,
	S_LISTS,
	S_SEARCH_VIEW,
	S_SEARCH_HINT,
	S_SIGNIN_FAIL,
	S_PAIR_TITLE,
	S_PAIR_LINE1,
	S_PAIR_LINE2,
	S_CODE,
	S_B_CANCEL,
	S_START_QUIT,
	S_STARTING,
	S_PLAY_FAIL,
	S_PREMIUM_REQ,
	S_LYR_SEARCHING,
	S_LYR_NONE,
	S_LYR_FAIL,
	S_BACK,
	S_LYRICS,
	S_LIBRARY,
	S_SETTINGS,
	S_LANGUAGE,
	S_ACCENT,
	S_UNLINK,
	S_UNLINKED_NOTE,
	S_BACK_MENU,
	S_ACCENT_GREEN,
	S_ACCENT_PURPLE,
	S_ACCENT_BLUE,
	S_ACCENT_ORANGE,
	S_RECENTLY,
	S_RECENT_TITLE,
	S_CLEAR,
	S_HELP_TITLE,
	S_H_SEC_GENERAL,
	S_H_SEC_LIST,
	S_H_SEC_PLAYER,
	S_H_SEC_LYRICS,
	S_H_START,
	S_H_SELECT,
	S_H_DPAD,
	S_H_LR_PAGE,
	S_H_A_OPEN,
	S_H_X_RADIO,
	S_H_SEARCH,
	S_H_HOLD_SEARCH,
	S_H_DRAG_LIST,
	S_H_A_PAUSE,
	S_H_Y_MUTE,
	S_H_SKIP,
	S_H_SCRUB,
	S_H_SHELF,
	S_H_LETRA,
	S_H_B_BACK,
	S_H_DRAG_LYR,
	S_H_LID,
	S_KEY_LENS,
	S_KEY_FINGER,
	S_DOWNLOADS,
	S_H_MODES,
	S_H_HEART,
	S_DL_VIEW,
	S_OFFLINE_A,
	S_QUEUE,
	S_QUEUE_EMPTY,
	S_QUEUE_DLING,
	S_UPNEXT,
	S_COUNT
} str_id;

void    lang_set(lang_id id);
lang_id lang_get(void);
const char *tr(str_id id);

/* The lang= value in spotify.cfg and the name the settings row shows. An
 * unknown code falls back to Spanish. */
const char *lang_code(lang_id id);
const char *lang_name(lang_id id);
lang_id     lang_from_code(const char *code);
