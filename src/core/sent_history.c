#include "core/sent_history.h"
#include "core/util.h"
#include "cJSON.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool bm_sent_history_append(bm_sent_history *h, const bm_sent_msg *msg) {
    if (h->count == h->cap) {
        int cap = h->cap ? h->cap * 2 : 32;
        bm_sent_msg *items = realloc(h->items, (size_t)cap * sizeof *items);
        if (!items) return false;
        h->items = items;
        h->cap = cap;
    }
    h->items[h->count++] = *msg;
    return true;
}

void bm_sent_history_free(bm_sent_history *h) {
    free(h->items);
    *h = (bm_sent_history){0};
}

static bool read_string(const cJSON *row, const char *key, char *out, size_t cap) {
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, key));
    return value && bm_strlcpy(out, value, cap) < cap;
}

static bool read_record(const cJSON *row, bm_sent_msg *m) {
    char uid[32], level[sizeof m->target.custom_title];
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(row, "timestamp_ms");
    if (!cJSON_IsNumber(ts) || !(ts->valuedouble >= 1 && ts->valuedouble < (double)INT64_MAX)
        || !read_string(row, "uid", uid, sizeof uid) || !uid[0] || uid[strspn(uid, "0123456789")]) return false;
    errno = 0;
    m->target.uid = strtoull(uid, NULL, 10);
    if (errno || !m->target.uid || !read_string(row, "guard_level", level, sizeof level)) return false;
    m->timestamp_ms = (int64_t)ts->valuedouble;
    bm_guard_set_level_from_tag(&m->target, level);
    return read_string(row, "username", m->target.username, sizeof m->target.username)
        && read_string(row, "time", m->target.time, sizeof m->target.time)
        && read_string(row, "sender_uid", m->sender_uid, sizeof m->sender_uid)
        && read_string(row, "msg_key", m->msg_key, sizeof m->msg_key)
        && read_string(row, "code", m->code, sizeof m->code);
}

bool bm_sent_history_open(bm_sent_history *h, const char *path) {
    if (!path || !path[0] || strlen(path) >= sizeof h->path) return false;
    char *text = NULL;
    if (!bm_read_file(path, &text, NULL)) {
        if (errno != ENOENT) return false;
    } else {
        cJSON *array = cJSON_ParseWithOpts(text, NULL, true);
        free(text);
        bool ok = cJSON_IsArray(array);
        const cJSON *row;
        cJSON_ArrayForEach(row, array) {
            bm_sent_msg m = {0};
            if (!ok || !read_record(row, &m) || !bm_sent_history_append(h, &m)) { ok = false; break; }
        }
        cJSON_Delete(array);
        if (!ok) { bm_sent_history_free(h); return false; }
    }
    bm_strlcpy(h->path, path, sizeof h->path);
    return true;
}

static bool write_record(cJSON *array, const bm_sent_msg *m) {
    char uid[32];
    snprintf(uid, sizeof uid, "%llu", (unsigned long long)m->target.uid);
    cJSON *row = cJSON_CreateObject();
    if (!cJSON_AddItemToArray(array, row)) { cJSON_Delete(row); return false; }
    return cJSON_AddStringToObject(row, "uid", uid)
        && cJSON_AddStringToObject(row, "username", m->target.username)
        && cJSON_AddStringToObject(row, "guard_level", bm_guard_level_str(&m->target))
        && cJSON_AddStringToObject(row, "time", m->target.time)
        && cJSON_AddStringToObject(row, "sender_uid", m->sender_uid)
        && cJSON_AddStringToObject(row, "msg_key", m->msg_key)
        && cJSON_AddStringToObject(row, "code", m->code)
        && cJSON_AddNumberToObject(row, "timestamp_ms", (double)m->timestamp_ms);
}

bool bm_sent_history_save(const bm_sent_history *h) {
    if (!h->path[0]) return false;
    cJSON *array = cJSON_CreateArray();
    bool ok = array != NULL;
    for (int i = 0; ok && i < h->count; i++) ok = write_record(array, &h->items[i]);
    char *text = ok ? cJSON_PrintUnformatted(array) : NULL;
    cJSON_Delete(array);
    if (!text) return false;
    ok = bm_write_private_file_atomic(h->path, text, strlen(text));
    free(text);
    return ok;
}

static void write_field(FILE *f, const char *text) {
    if (!strpbrk(text, ",\"\r\n")) {
        fputs(text, f);
        return;
    }
    fputc('"', f);
    for (; *text; text++) {
        if (*text == '"') fputc('"', f);
        fputc(*text, f);
    }
    fputc('"', f);
}

bool bm_sent_history_export_csv(const bm_sent_msg *items, int count, const char *path) {
    FILE *f = bm_open_write_utf8(path);
    if (!f) return false;
    fputs("\xEF\xBB\xBF" "航海等级,UID,用户名,上舰时间,发送时间,兑换码\r\n", f);
    /* Match the history panel: newest first, including records past the recall window. */
    for (int i = count - 1; i >= 0 && !ferror(f); i--) {
        const bm_sent_msg *m = &items[i];
        char uid[32], sent_at[32] = "";
        snprintf(uid, sizeof uid, "%llu", (unsigned long long)m->target.uid);
        time_t seconds = (time_t)(m->timestamp_ms / 1000);
        struct tm *local = localtime(&seconds);
        if (local) strftime(sent_at, sizeof sent_at, "%Y-%m-%d %H:%M:%S", local);
        const char *fields[] = {bm_guard_level_str(&m->target), uid, m->target.username,
                                m->target.time, sent_at, m->code};
        for (size_t j = 0; j < sizeof fields / sizeof fields[0]; j++) {
            if (j) fputc(',', f);
            write_field(f, fields[j]);
        }
        fputs("\r\n", f);
    }
    bool ok = !ferror(f);
    return fclose(f) == 0 && ok;
}
