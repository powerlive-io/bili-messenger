/* Real page callbacks and roster refresh; jobs/network are deterministic. */
#include "ui/app.h"
#include <cui/cui_render.h>
#include <cui_internal.h>
#ifdef NDEBUG
#error "Message tests require assertions enabled"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *pending_job;
static bm_job_fn pending_worker;
static bm_main_cb pending_callback;
static bool spawn_ok = true;
static int network_sends;
static const char *dialog_path;
static const char *history_path = "bm_test_sent_history.json";

static bool test_spawn(bm_job_fn fn, void *arg) {
    if (!spawn_ok) return false;
    assert(!pending_job);
    pending_job = arg;
    pending_worker = fn;
    return true;
}

static void test_post(bm_main_cb fn, void *arg) {
    assert(arg == pending_job);
    pending_callback = fn;
}

static bool test_send(const bm_login_info *login, uint64_t uid, const char *message,
                      char *key, size_t cap, bm_bili_err *err) {
    assert(uid != 0);
    network_sends++;
    bm_strlcpy(key, "test-key", cap);
    return true;
}

#define bm_job_spawn test_spawn
#define bm_job_post_main test_post
#define bm_bili_send_message test_send
#include "../src/pages/message.c"
#include "../src/pages/guard_list.c"
#undef bm_job_spawn
#undef bm_job_post_main
#undef bm_bili_send_message

cui_texture bm_image_get(cui_ctx *ui, const char *url, int *w, int *h) { return 0; }
cui_texture bm_guard_icon_get(cui_ctx *ui, int level) { return 0; }
void bm_images_shutdown(cui_ctx *ui) {}
bool cui_app_file_dialog(cui_app *app, const cui_file_dialog_opts *opts, char *out, size_t cap) {
    if (opts->kind == CUI_FILE_SAVE) {
        assert(!strcmp(opts->filter, "csv"));
        if (!dialog_path) return false;
    }
    assert(dialog_path);
    bm_strlcpy(out, dialog_path, cap);
    return true;
}
bool cui_app_open_url(const char *url) { assert(!"Unexpected URL"); return false; }

static bm_app *new_app(void) {
    bm_app *app = calloc(1, sizeof *app);
    assert(app);
    bm_store_init(&app->store);
    assert(bm_sent_history_open(&app->history, history_path) && !app->history.count);
    bm_strlcpy(app->store.path, "bm_test_message.json", sizeof app->store.path);
    bm_strlcpy(app->store.room_id, "123", sizeof app->store.room_id);
    app->store.login.has_login = true;
    strcpy(app->store.login.DedeUserID, "42");
    app->filters = (bm_guard_filters){true, true, true};
    bm_store_set_template(&app->store, "test", "Hello {name}: {code}");
    assert(app->store.template_count == 1);
    app->msg_template_idx = 0;
    assert(bm_store_import_codes(&app->store, "CODE-A\nCODE-B\nCODE-C") == 3);
    bm_guard a = {.uid = 1, .username = "Alice", .guard_level = 3};
    bm_guard b = {.uid = 2, .username = "Bob", .guard_level = 3};
    bm_guard c = {.uid = 3, .username = "Governor", .guard_level = 1};
    assert(bm_guard_list_push(&app->store.original_guards, &a));
    assert(bm_guard_list_push(&app->store.original_guards, &b));
    assert(bm_guard_list_push(&app->store.original_guards, &c));
    bm_refresh_guard_stats_and_filters(app, false);
    return app;
}

static void destroy_app(bm_app *app) {
    assert(!pending_job && !app->msg_send_pending);
    bm_store_free(&app->store);
    bm_sent_history_free(&app->history);
    bm_guard_list_free(&app->msg_remain_list);
    free(app);
    assert(remove("bm_test_message.json") == 0);
    remove(history_path); /* Absent if nothing was delivered. */
}

static void start(bm_app *app) {
    app->msg_in_progress = true;
    app->msg_total_count = app->store.guards.count;
    trigger_next_send_step(app);
    assert(pending_job && app->msg_send_pending);
}

static void finish(bm_app *app, bool ok) {
    send_step_job *job = pending_job;
    assert(job);
    pending_job = NULL;
    job->ok = ok;
    if (ok) job->sent.timestamp_ms = bm_now_ms();
    on_send_step_done(job, app);
}

static void test_live_edits(void) {
    bm_app *app = new_app();
    start(app);
    finish(app, false); /* Alice failed; Bob is now in flight. */
    assert(((send_step_job *)pending_job)->sent.target.uid == 2);
    assert(app->store.original_guards.count == 3);
    bm_refresh_guard_stats_and_filters(app, true);
    assert(app->store.guards.count == 2); /* Do not reinsert Alice. */
    app->filters.type1 = false;
    bm_refresh_guard_stats_and_filters(app, true);
    assert(app->store.guards.count == 1);
    bm_guard added = {.uid = 4, .username = "Added during send", .guard_level = 3};
    assert(bm_guard_list_push(&app->store.original_guards, &added));
    bm_refresh_guard_stats_and_filters(app, true);
    assert(app->store.guards.count == 1); /* New additions wait for the next pass. */
    /* Delete the in-flight recipient; it cannot cancel an already-issued request. */
    app->store.guards.items[0].selected = true;
    delete_selected_guards(app);
    assert(app->store.guards.count == 0 && app->stats.total == 3);
    finish(app, true);
    assert(!pending_job && !app->msg_in_progress);
    assert(app->history.count == 1 && app->store.code_count == 2);
    assert(app->store.guards.count == 2 && app->store.guards.items[0].uid == 1);
    assert(app->store.guards.items[1].uid == 4);
    assert(bm_guard_list_find_uid(&app->store.original_guards, 3) >= 0);
    destroy_app(app);

    app = new_app();
    start(app);
    bm_guard_list_clear(&app->store.original_guards);
    bm_refresh_guard_stats_and_filters(app, true);
    assert(app->store.guards.count == 0 && app->stats.total == 0);
    finish(app, true);
    assert(!pending_job && app->store.original_guards.count == 0);
    destroy_app(app);
}

static void test_replaced_roster(void) {
    /* Snapshot, gift stream and file import must all invalidate old callbacks. */
    for (int kind = 0; kind < 3; kind++) {
        for (int ok = 0; ok <= 1; ok++) {
            bm_app *app = new_app();
            start(app);
            bm_guard replacement = {.uid = 1, .username = "New Alice", .guard_level = 3};
            if (kind == 0) {
                snapshot_job_data *job = calloc(1, sizeof *job);
                assert(job);
                job->ok = true;
                assert(bm_guard_list_push(&job->list, &replacement));
                on_snapshot_loaded(job, app);
            } else if (kind == 1) {
                gift_job_data *job = calloc(1, sizeof *job);
                assert(job);
                job->ok = true;
                assert(bm_guard_list_push(&job->list, &replacement));
                on_gift_loaded(job, app);
            } else {
                dialog_path = "bm_test_message_import.csv";
                const char *csv = "舰长,1,NewAlice\n";
                assert(bm_write_file(dialog_path, csv, strlen(csv)));
                open_import_file_dialog(app);
                assert(remove(dialog_path) == 0);
                dialog_path = NULL;
            }
            assert(app->guard_roster_generation == 1);
            assert(!app->msg_in_progress && app->msg_send_pending);
            assert(app->store.guards.count == 1);
            finish(app, ok != 0);
            assert(!pending_job && app->store.original_guards.count == 1);
            assert(app->store.original_guards.items[0].uid == 1);
            assert(app->store.guards.count == 1);
            assert(app->store.code_count == 3 - ok);
            assert(app->history.count == ok);
            bm_store loaded;
            bm_store_init(&loaded);
            assert(bm_store_load(&loaded, app->store.path));
            assert(loaded.original_guards.count == 1 && loaded.code_count == 3 - ok);
            bm_sent_history history = {0};
            assert(bm_sent_history_open(&history, history_path) && history.count == ok);
            bm_sent_history_free(&history);
            bm_store_free(&loaded);
            destroy_app(app);
        }
    }
}

static void test_pause_and_invalid_uid(void) {
    bm_app *app = new_app();
    start(app);
    void *first = pending_job;
    app->msg_in_progress = false;
    trigger_next_send_step(app);
    assert(pending_job == first); /* No second chain before the callback. */
    finish(app, true);
    assert(!pending_job && app->store.guards.count == 2);
    start(app);
    assert(((send_step_job *)pending_job)->sent.target.uid == 2);
    app->msg_in_progress = false;
    finish(app, false);
    assert(app->store.guards.count == 2);
    destroy_app(app);

    app = new_app();
    bm_guard invalid = {.uid = 0, .username = "Invalid"};
    assert(bm_guard_list_unshift(&app->store.original_guards, &invalid));
    assert(bm_guard_list_unshift(&app->store.original_guards, &invalid));
    bm_refresh_guard_stats_and_filters(app, false);
    start(app);
    for (int i = 0; i < 2; i++) {
        send_step_job *job = pending_job;
        assert(job->sent.target.uid == 0);
        job->interval_ms = 0;
        pending_worker(job);
        assert(pending_callback == on_send_step_done && !job->ok && network_sends == 0);
        pending_job = NULL;
        pending_callback(job, app);
        pending_callback = NULL;
    }
    assert(((send_step_job *)pending_job)->sent.target.uid == 1);
    assert(app->store.original_guards.count == 5 && app->store.code_count == 3);
    app->msg_in_progress = false;
    finish(app, true);
    assert(app->store.original_guards.count == 4);
    destroy_app(app);
}

static void test_spawn_failure_and_code_exhaustion(void) {
    bm_app *app = new_app();
    spawn_ok = false;
    app->msg_in_progress = true;
    trigger_next_send_step(app);
    assert(!app->msg_in_progress && !app->msg_send_pending && !pending_job);
    assert(app->store.guards.count == 3);
    spawn_ok = true;
    start(app);
    bm_store_clear_codes(&app->store);
    finish(app, true);
    assert(!app->msg_in_progress && !app->msg_send_pending && !pending_job);
    assert(app->store.guards.count == 2 && app->store.original_guards.count == 2);
    destroy_app(app);
}

static void test_refresh_during_recall(void) {
    bm_app *app = new_app();
    app->msg_in_progress = true;
    app->msg_is_recalling = true;
    app->msg_progress_index = 2;
    app->msg_total_count = 5;
    bm_guard_list_clear(&app->store.original_guards);
    bm_guard replacement = {.uid = 99, .username = "New roster"};
    assert(bm_guard_list_push(&app->store.original_guards, &replacement));
    bm_guard_roster_replaced(app);
    assert(app->msg_in_progress && app->msg_is_recalling);
    assert(app->msg_progress_index == 2 && app->msg_total_count == 5);
    assert(app->store.guards.count == 1 && app->store.guards.items[0].uid == 99);
    destroy_app(app);
}

static void test_duplicate_uid_filters(void) {
    for (int governor_first = 0; governor_first <= 1; governor_first++) {
        for (int keep_governor = 0; keep_governor <= 1; keep_governor++) {
            bm_app *app = new_app();
            bm_guard_list_clear(&app->store.original_guards);
            bm_guard captain = {.uid = 100, .username = "Captain", .guard_level = 3};
            bm_guard governor = {.uid = 100, .username = "Governor", .guard_level = 1};
            assert(bm_guard_list_push(&app->store.original_guards, governor_first ? &governor : &captain));
            assert(bm_guard_list_push(&app->store.original_guards, governor_first ? &captain : &governor));
            bm_refresh_guard_stats_and_filters(app, false);
            app->msg_in_progress = true;
            app->filters.type1 = keep_governor != 0;
            app->filters.type3 = keep_governor == 0;
            bm_refresh_guard_stats_and_filters(app, true);
            assert(app->store.guards.count == 1);
            assert(app->store.guards.items[0].guard_level == (keep_governor ? 1 : 3));
            bm_refresh_guard_stats_and_filters(app, true);
            assert(app->store.guards.count == 1);
            /* The same UID still exists, but only in an excluded tier. */
            int retained = bm_guard_list_find_uid(&app->store.original_guards, 100);
            if (app->store.original_guards.items[retained].guard_level != (keep_governor ? 1 : 3)) retained++;
            assert(bm_guard_list_remove_at(&app->store.original_guards, retained));
            bm_refresh_guard_stats_and_filters(app, true);
            assert(app->store.guards.count == 0);
            destroy_app(app);
        }
    }
}

static void frame(cui_ctx *ui, bm_app *app) {
    for (int i = 0; i < 4; i++) {
        cui_begin_frame(ui, 1.0f / 60.0f);
        if (app->page == BM_PAGE_CODES) bm_page_codes(ui, app);
        else bm_page_message(ui, app);
        cui_end_frame(ui);
    }
}

static void click_id(cui_ctx *ui, bm_app *app, cui_id id) {
    cui_rect rect = cui_get_rect(ui, id);
    assert(rect.w > 0 && rect.h > 0);
    cui_input_mouse_move(ui, rect.x + rect.w / 2, rect.y + rect.h / 2);
    frame(ui, app);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    frame(ui, app);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    frame(ui, app);
}

static void click(cui_ctx *ui, bm_app *app, const char *label) {
    click_id(ui, app, cui_make_id(ui, label));
}

static cui_id recall_id(cui_ctx *ui, const char *key) {
    cui_push_id(ui, key);
    cui_id id = cui_make_id(ui, "撤回");
    cui_pop_id(ui);
    return id;
}

static void test_history_confirmations(void) {
    cui_sw_renderer *renderer = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer = cui_sw_iface(renderer), .theme = CUI_THEME_LIGHT});
    assert(ui);
    bm_fonts_init(ui);
    cui_set_viewport(ui, 1024, 640, 1);
    cui_set_reduced_motion(ui, true);
    bm_app *app = new_app();
    assert(bm_store_save(&app->store));
    bm_sent_msg m = {.sender_uid = "42", .msg_key = "first", .timestamp_ms = bm_now_ms()};
    assert(bm_sent_history_append(&app->history, &m));
    strcpy(m.msg_key, "second");
    assert(bm_sent_history_append(&app->history, &m));
    frame(ui, app);

    /* Confirmations belong to the message, not the row or the last click. */
    click_id(ui, app, recall_id(ui, "first"));
    click_id(ui, app, recall_id(ui, "second"));
    assert(!pending_job && app->history.count == 2);
    click_id(ui, app, recall_id(ui, "second"));
    recall_step_job *job = pending_job;
    assert(job && job->is_single && !strcmp(job->item.msg_key, "second"));
    pending_job = NULL;
    job->ok = true;
    on_recall_step_done(job, app);
    frame(ui, app);

    click(ui, app, "一键撤回全部消息");
    assert(!pending_job && !app->msg_in_progress && app->history.count == 1);
    click(ui, app, "一键撤回全部消息");
    job = pending_job;
    assert(job && !job->is_single && app->msg_is_recalling);
    pending_job = NULL;
    on_recall_step_done(job, app); /* Failed recall leaves a record to clear. */
    frame(ui, app);

    click(ui, app, "清空记录");
    assert(app->history.count == 1);
    click(ui, app, "清空记录");
    assert(app->history.count == 0 && !pending_job);
    destroy_app(app);
    cui_destroy(ui);
    cui_sw_destroy(renderer);
}

static int64_t clock_now(void *user) { return *(int64_t *)user; }
static void clock_sleep(int ms, void *user) { *(int64_t *)user += ms; }

static void test_sent_export(void) {
    bm_app *app = new_app();
    struct tm local = {.tm_year = 126, .tm_mon = 5, .tm_mday = 2, .tm_hour = 12, .tm_isdst = -1};
    int64_t clock = (int64_t)mktime(&local) * 1000;
    bm_set_time_hooks(clock_now, clock_sleep, &clock);
    app->store.send_interval = 5000;
    bm_strlcpy(app->store.guards.items[0].time, "2026-06-01", sizeof app->store.guards.items[0].time);
    bm_strlcpy(app->store.guards.items[0].username, "Alice, \"A\"", sizeof app->store.guards.items[0].username);
    bm_store_set_template(&app->store, "test", "{name}, \"你好\"\n{code}");
    start(app);
    /* The second send uses the edited template, never the first one. */
    bm_store_set_template(&app->store, "test", "你好 {name}");
    for (int i = 0; i < 2; i++) {
        pending_worker(pending_job);
        void *job = pending_job;
        pending_job = NULL;
        pending_callback(job, app);
        pending_callback = NULL;
    }
    finish(app, false); /* Failed third attempt must not be exported. */
    bm_set_time_hooks(NULL, NULL, NULL);
    /* Start with a fresh store, as on restart: the export below must be
       reconstructed from disk, not the live send jobs or current template. */
    bm_store_free(&app->store);
    bm_store_init(&app->store);
    assert(bm_store_load(&app->store, "bm_test_message.json"));
    bm_sent_history_free(&app->history);
    assert(bm_sent_history_open(&app->history, history_path));
    assert(app->history.count == 2 && app->store.code_count == 2);
    assert(!strcmp(app->history.items[0].msg_key, "test-key"));
    export_sent_history(app); /* Cancel. */
    dialog_path = "./bm_test_sent.csv";
    export_sent_history(app);
    assert(!strcmp(app->toasts[app->toast_count - 1].desc, "bm_test_sent.csv"));
    char *csv = NULL;
    assert(bm_read_file(dialog_path, &csv, NULL));
    assert(!strcmp(csv, "\xEF\xBB\xBF" "航海等级,UID,用户名,上舰时间,发送时间,兑换码\r\n"
                        "舰长,2,Bob,,2026-06-02 12:00:05,\r\n"
                        "舰长,1,\"Alice, \"\"A\"\"\",2026-06-01,2026-06-02 12:00:00,CODE-A\r\n"));
    free(csv);
    assert(remove(dialog_path) == 0);
    dialog_path = NULL;
    char *config = NULL;
    assert(bm_read_file(app->store.path, &config, NULL));
    assert(!strstr(config, "sentHistory") && !strstr(config, "test-key"));
    /* Clearing history must not persist unrelated in-memory settings. */
    app->store.send_interval++;
    clear_sent_history(app);
    char *unchanged = NULL;
    assert(bm_read_file(app->store.path, &unchanged, NULL) && !strcmp(unchanged, config));
    free(unchanged);
    free(config);
    bm_sent_history_free(&app->history);
    assert(bm_sent_history_open(&app->history, history_path) && !app->history.count);
    destroy_app(app);
}

static void test_persistent_recall(void) {
    bm_app *app = new_app();
    bm_sent_msg m = {.target = {.uid = 9007199254740993ULL, .custom_title = "粉丝"},
                     .sender_uid = "42", .msg_key = "7391827391827391827", .timestamp_ms = bm_now_ms() - 121000};
    assert(bm_sent_history_append(&app->history, &m)); /* Expired. */
    m.timestamp_ms = bm_now_ms();
    strcpy(m.sender_uid, "24");
    assert(bm_sent_history_append(&app->history, &m)); /* Another account. */
    strcpy(m.sender_uid, "42");
    assert(bm_sent_history_append(&app->history, &m)); /* Request fails. */
    strcpy(m.msg_key, "7391827391827391828");
    assert(bm_sent_history_append(&app->history, &m)); /* Request succeeds. */
    assert(bm_sent_history_save(&app->history));
    assert(bm_store_save(&app->store));
    app->msg_in_progress = app->msg_is_recalling = true;
    trigger_next_recall_step(app);
    for (int i = 2; i <= 3; i++) {
        recall_step_job *job = pending_job;
        pending_job = NULL;
        assert(job && job->index == i && app->msg_recall_pending);
        job->ok = i == 3;
        on_recall_step_done(job, app);
    }
    assert(!pending_job && !app->msg_in_progress && !app->msg_recall_pending);
    bm_sent_history_free(&app->history);
    assert(bm_sent_history_open(&app->history, history_path) && app->history.count == 3);
    for (int i = 0; i < 3; i++) {
        const bm_sent_msg *saved = &app->history.items[i];
        assert(saved->target.uid == 9007199254740993ULL);
        assert(!strcmp(saved->target.custom_title, "粉丝") && !strcmp(saved->msg_key, "7391827391827391827"));
        assert(!strcmp(saved->sender_uid, i == 1 ? "24" : "42"));
    }
    destroy_app(app);
}

static void test_recall_save_failure(void) {
    bm_app *app = new_app();
    assert(bm_store_save(&app->store));
    for (int i = 0; i < 3; i++) {
        bm_sent_msg m = {.target = {.uid = 123}, .sender_uid = "42", .timestamp_ms = bm_now_ms()};
        snprintf(m.msg_key, sizeof m.msg_key, "key-%d", i);
        assert(bm_sent_history_append(&app->history, &m));
    }
    assert(bm_sent_history_save(&app->history));
    /* Use an existing regular file as a parent: opening the temp file must fail. */
    snprintf(app->history.path, sizeof app->history.path, "%s/no-file", history_path);
    for (int single = 0; single <= 1; single++) {
        app->msg_in_progress = app->msg_is_recalling = !single;
        assert(spawn_recall(app, 1, single != 0));
        recall_step_job *job = pending_job;
        pending_job = NULL;
        job->ok = true;
        on_recall_step_done(job, app);
        assert(!pending_job && !app->msg_recall_pending && !app->msg_in_progress);
        assert(app->toasts[app->toast_count - 1].tone == CUI_TONE_WARNING);
        bm_sent_history disk = {0};
        assert(bm_sent_history_open(&disk, history_path));
        assert(disk.count == 3 && app->history.count == 3);
        for (int i = 0; i < disk.count; i++) assert(!strcmp(disk.items[i].msg_key, app->history.items[i].msg_key));
        bm_sent_history_free(&disk);
    }
    destroy_app(app);
}

static void test_independent_history_file(void) {
    bm_app *app = new_app();
    assert(bm_write_file(history_path, "{bad", 4));
    bm_sent_history_free(&app->history);
    assert(!bm_sent_history_open(&app->history, history_path));
    assert(!bm_sent_history_save(&app->history));
    app->msg_in_progress = true;
    trigger_next_send_step(app);
    assert(!pending_job && !app->msg_in_progress && app->store.guards.count == 3);
    assert(bm_store_save(&app->store));
    char *text = NULL;
    assert(bm_read_file(history_path, &text, NULL) && !strcmp(text, "{bad"));
    free(text);
    destroy_app(app);
}

static void test_write_failure_stops_chain(void) {
    for (int history_fails = 0; history_fails <= 1; history_fails++) {
        bm_app *app = new_app();
        assert(bm_store_save(&app->store));
        start(app);
        /* The parent is an existing regular file, so this fails on every OS. */
        if (history_fails)
            strcpy(app->history.path, "bm_test_message.json/history");
        else
            strcpy(app->store.path, "bm_test_message.json/config");
        finish(app, true);
        assert(!app->msg_in_progress && !app->msg_send_pending && !pending_job);
        assert(app->history.count == 1 && app->history.items[0].target.uid == 1);
        assert(app->store.guards.count == 2 && app->store.code_count == 2);
        assert(app->toast_count > 0 && app->toasts[app->toast_count - 1].tone == CUI_TONE_DANGER);
        destroy_app(app);
    }
}

static void test_recall_expires_in_queue(void) {
    int64_t now = 1700000000000LL;
    bm_set_time_hooks(clock_now, clock_sleep, &now);
    bm_app *app = new_app();
    bm_sent_msg m = {.sender_uid = "42", .msg_key = "first", .timestamp_ms = now - 120000};
    assert(bm_sent_history_append(&app->history, &m));
    assert(bm_sent_history_save(&app->history) && bm_store_save(&app->store));
    spawn_ok = false;
    start_single_recall(app, 0);
    assert(!pending_job && !app->msg_recall_pending && app->history.count == 1);
    app->msg_in_progress = app->msg_is_recalling = true;
    trigger_next_recall_step(app);
    assert(!pending_job && !app->msg_is_recalling && !app->msg_in_progress);
    spawn_ok = true;
    start_single_recall(app, 0);
    assert(pending_job && app->msg_recall_pending);
    now++; /* Eligible when enqueued, but not when the worker actually runs. */
    recall_step_job *job = pending_job;
    pending_worker(job);
    assert(job->skipped_timeout && pending_callback == on_recall_step_done);
    pending_job = NULL;
    pending_callback(job, app);
    pending_callback = NULL;
    assert(!app->msg_recall_pending && app->history.count == 1);
    assert(app->toasts[app->toast_count - 1].tone == CUI_TONE_WARNING);
    destroy_app(app);
    bm_set_time_hooks(NULL, NULL, NULL);
}

static void test_long_toast_layout(void) {
    cui_sw_renderer *renderer = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer = cui_sw_iface(renderer), .theme = CUI_THEME_LIGHT});
    assert(ui);
    bm_fonts_init(ui);
    cui_set_viewport(ui, 1024, 720, 1);
    cui_set_reduced_motion(ui, true);
    bm_app *app = calloc(1, sizeof *app);
    assert(app);
    char filename[512] = "21484828-";
    for (int i = 0; i < 26; i++) strcat(filename, "已发送记录-");
    strcat(filename, "2026-06-02.csv");
    bm_toast_push(app, "导出 CSV 成功", filename, CUI_TONE_SUCCESS);
    for (int i = 0; i < 4; i++) {
        cui_begin_frame(ui, 1.0f / 60.0f);
        bm_toast_flush(app, ui);
        cui_end_frame(ui);
    }
    cui_rect bounds = cui_get_rect(ui, 0x70A57u);
    bool found = false;
    for (int i = 0; i < ui->node_count; i++) {
        cui_node *node = &ui->nodes[i];
        if (!node->text || strncmp(node->text, "21484828-", 9)) continue;
        assert(strstr(node->text, "…") && node->text_lines == 1);
        /* Check the laid-out text width, not just the text node's allocated box. */
        assert(node->rect.x + node->lines[0].width <= bounds.x + bounds.w - 24);
        found = true;
    }
    assert(found);
    free(app);
    cui_destroy(ui);
    cui_sw_destroy(renderer);
}

static void test_pause_controls(void) {
    const int width = 1280, height = 760;
    cui_sw_renderer *renderer = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer = cui_sw_iface(renderer), .theme = CUI_THEME_LIGHT});
    assert(ui);
    bm_fonts_init(ui);
    cui_set_viewport(ui, width, height, 1);
    cui_set_reduced_motion(ui, true);
    bm_app *app = new_app();
    bm_sent_msg sent = {.target = {.uid = 99, .username = "Previously sent"},
                        .msg_key = "old-key", .sender_uid = "42", .timestamp_ms = bm_now_ms()};
    assert(bm_sent_history_append(&app->history, &sent));
    start(app);
    frame(ui, app);
    click(ui, app, "暂停群发");
    assert(!app->msg_in_progress && app->msg_send_pending);

    /* Optional actual page render, with identical fixture/viewport for PR evidence. */
    const char *path = getenv("BM_MESSAGE_SCREENSHOT");
    if (path && path[0]) {
        void *pixels = calloc((size_t)width * height, 4);
        assert(pixels);
        cui_sw_render(renderer, cui_get_draw_data(ui), pixels, width, height, width * 4, CUI_SW_RGBA);
        assert(cui_write_png(path, pixels, width, height, width * 4));
        free(pixels);
    }

    void *first = pending_job;
    click(ui, app, "开始群发私信");
    assert(pending_job == first && !app->msg_in_progress);
    click(ui, app, "一键撤回全部消息");
    assert(pending_job == first && !app->msg_in_progress && !app->msg_is_recalling);
    dialog_path = "bm_test_export_button.csv";
    click(ui, app, "导出 CSV");
    assert(remove(dialog_path) == 0 && app->history.count == 1);
    dialog_path = NULL;
    click(ui, app, "跳过队首用户");
    assert(app->store.original_guards.count == 3);
    finish(app, true);
    frame(ui, app);
    click(ui, app, "开始群发私信");
    assert(app->msg_in_progress && app->msg_send_pending);
    app->msg_in_progress = false;
    finish(app, true);
    destroy_app(app);
    cui_destroy(ui);
    cui_sw_destroy(renderer);
}

static void test_unicode_codes_and_history(void) {
    bm_app *app = new_app();
    app->page = BM_PAGE_CODES;
    cui_sw_renderer *renderer = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer = cui_sw_iface(renderer)});
    assert(ui);
    cui_set_viewport(ui, 1280, 760, 1);
    cui_set_reduced_motion(ui, true);
    frame(ui, app);
    dialog_path = "中文目录/六月 福利/兑换码.txt";
    click(ui, app, "从 TXT 文件批量导入...");
    assert(app->store.code_count == 4 && !strcmp(app->store.codes[3].code, "六月-001"));
    cui_destroy(ui);
    cui_sw_destroy(renderer);

    bm_sent_msg m = {.target = {.uid = 123, .username = "小林", .guard_level = 3},
                     .code = "六月-001", .timestamp_ms = bm_now_ms()};
    assert(bm_sent_history_append(&app->history, &m));
    dialog_path = "中文目录/六月 福利/已发送记录.csv";
    export_sent_history(app);
    assert(!strcmp(app->toasts[app->toast_count - 1].title, "导出 CSV 成功"));
    dialog_path = NULL;
    destroy_app(app);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--unicode-files")) {
        test_unicode_codes_and_history();
        return 0;
    }
    test_live_edits();
    test_replaced_roster();
    test_pause_and_invalid_uid();
    test_spawn_failure_and_code_exhaustion();
    test_refresh_during_recall();
    test_duplicate_uid_filters();
    test_sent_export();
    test_persistent_recall();
    test_recall_save_failure();
    test_independent_history_file();
    test_write_failure_stops_chain();
    test_recall_expires_in_queue();
    test_long_toast_layout();
    test_history_confirmations();
    test_pause_controls();
    puts("PASS send callbacks, live roster edits, imports, pause, invalid UID and stop paths");
    return 0;
}
