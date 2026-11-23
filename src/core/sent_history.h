#ifndef BM_SENT_HISTORY_H
#define BM_SENT_HISTORY_H

#include "core/guards.h"

/* Snapshot of one delivery; independent of subsequent roster and template edits. */
typedef struct bm_sent_msg {
    bm_guard target;
    char sender_uid[64];
    char code[256];
    char msg_key[64];
    int64_t timestamp_ms;
} bm_sent_msg;

typedef struct bm_sent_history {
    char path[1100]; /* Set only after opening the history successfully. */
    bm_sent_msg *items;
    int count, cap;
} bm_sent_history;

/* Open an independent history file into a zero-initialized history. A missing
   file is an empty history; a malformed or unreadable file is not writable. */
bool bm_sent_history_open(bm_sent_history *h, const char *path);
void bm_sent_history_free(bm_sent_history *h);
bool bm_sent_history_append(bm_sent_history *h, const bm_sent_msg *msg);
bool bm_sent_history_save(const bm_sent_history *h);
bool bm_sent_history_export_csv(const bm_sent_msg *items, int count, const char *path);

#endif /* BM_SENT_HISTORY_H */
