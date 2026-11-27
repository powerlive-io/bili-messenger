/* Exercise the same frame, workers and persistence as the desktop app. Only
   HTTP, the clock and the native window are replaced; there is no live login. */
#include "ui/app.h"
#include "core/http.h"
#include <cui/cui_render.h>
#include <cui_internal.h>

#ifdef NDEBUG
#error "App workflow tests require assertions enabled"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bm_app app;
static cui_ctx *ui;
static int64_t now = 1700000000000LL;
static bool scanned;

#define COVER_URL "https://fixture.invalid/cover.png"
#define RECEIPT "73652d4a-60ea-4435-93fd-b22fa2609b79"

static int64_t clock_ms(void *unused) { (void)unused; return now; }

static bool offline_http(const bm_http_req *req, bm_http_resp *resp, void *unused) {
    (void)unused;
    const char *url = req->url, *body = NULL;
    if (strstr(url, "/releases/latest")) {
        body = "{\"tag_name\":\"v" BM_VERSION "\"}";
    } else if (strstr(url, "/qrcode/generate")) {
        body = "{\"data\":{\"url\":\"https://fixture.invalid/scan\",\"qrcode_key\":\"fixture\"}}";
    } else if (strstr(url, "/qrcode/poll?qrcode_key=fixture")) {
        body = scanned ? "{\"data\":{\"code\":0,\"url\":\"https://fixture.invalid/?"
                         "DedeUserID=42&SESSDATA=test-session&bili_jct=test-csrf\"}}"
                       : "{\"data\":{\"code\":86090}}";
    } else if (strstr(url, "/space/info?uid=42&")) {
        assert(strstr(req->cookie, "DedeUserID=42"));
        body = "{\"code\":0,\"data\":{\"mid\":42,\"uname\":\"Test Anchor\"}}";
    } else if (strstr(url, "getH5InfoByRoom?room_id=123")) {
        body = "{\"code\":0,\"data\":{\"room_info\":{\"room_id\":12345,\"uid\":42,"
               "\"title\":\"Test Room\",\"cover\":\"" COVER_URL "\",\"online\":321,\"live_status\":1},"
               "\"anchor_info\":{\"base_info\":{\"uname\":\"Test Anchor\"},\"relation_info\":{\"attention\":100}}}}";
    } else if (strstr(url, "getH5InfoByRoom?room_id=404")) {
        body = "{\"code\":-1}";
    } else if (strstr(url, "/bulkGuard/42") || strstr(url, "/bulkActiveSome/42")) {
        body = "[{\"time\":1700000000000,\"guardNum\":2,\"follower\":100},"
               "{\"time\":1700086400000,\"guardNum\":5,\"follower\":140},"
               "{\"time\":1700172800000,\"guardNum\":3,\"follower\":120}]";
    } else if (strstr(url, "/day?room=123")) {
        body = "{\"Code\":0,\"Data\":[\"2020-01-01\",\"2020-01-01\"]}";
    } else if (strstr(url, "/topList?roomid=1&page=1&page_size=30&ruid=42")) {
        body = "{\"code\":0,\"data\":{\"info\":{\"page\":2},\"top3\":["
               "{\"uid\":71,\"username\":\"Alice\",\"guard_level\":1}],\"list\":[]}}";
    } else if (strstr(url, "/topList?roomid=1&page=2&page_size=30&ruid=42")) {
        body = "{\"code\":0,\"data\":{\"list\":[{\"uid\":72,\"username\":\"Bob\",\"guard_level\":3}]}}";
    } else if (strstr(url, "/history?room=123&date=2020-01-01")) {
        body = "{\"Code\":0,\"Data\":[{\"Uid\":\"81\",\"Username\":\"Carol\",\"Guard_level\":\"2\"}]}";
    } else if (!strcmp(url, COVER_URL)) {
        assert(bm_read_file("cover.png", &resp->data, &resp->size));
    } else if (!strcmp(url, "https://api.powerlive.io/feedback/prepare")) {
        assert(!req->cookie);
        body = "{\"already_submitted\":true,\"id\":\"" RECEIPT "\"}";
    } else {
        fprintf(stderr, "Unexpected fixture request: %s\n", url);
        abort();
    }
    if (body) {
        resp->data = bm_strdup(body);
        assert(resp->data);
        resp->size = strlen(body);
    }
    resp->ok = true;
    resp->status = 200;
    return true;
}

bool cui_app_open_url(const char *url) { (void)url; abort(); }
bool cui_app_file_dialog(cui_app *a, const cui_file_dialog_opts *o, char *out, size_t cap) {
    (void)a; (void)o; (void)out; (void)cap; abort();
}

/* active_count includes queued completions; a completion may start more jobs. */
static void drain_jobs(void) {
    while (bm_jobs_active_count()) {
        bm_jobs_poll_main(&app);
        bm_sleep_ms(1);
    }
}

static void frame(void) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    bm_app_frame(ui, &app);
    cui_end_frame(ui);
    drain_jobs();
}

static cui_rect text_rect(const char *label) {
    for (int i = 0; i < ui->node_count; i++) {
        const cui_node *n = &ui->nodes[i];
        if (n->text && n->text_len == (int)strlen(label) && !memcmp(n->text, label, n->text_len))
            return n->rect;
    }
    fprintf(stderr, "Page %d: expected to see '%s'\n", app.page, label);
    abort();
}

static void click_rect(cui_rect r) {
    assert(r.w > 0 && r.h > 0);
    cui_input_mouse_move(ui, r.x + r.w / 2, r.y + r.h / 2);
    frame();
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    frame();
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    frame();
    frame();
}

static void click(const char *label) { click_rect(text_rect(label)); }

static void test_onboarding(void) {
    assert(!app.store.room_id[0] && !app.store.login.has_login);
    bm_setting_startup_checks(&app);
    frame();
    frame();
    click("前往设置中心");
    frame(); /* Upload the QR generated by the settings worker to the renderer. */
    assert(app.page == BM_PAGE_SETTING && app.qr_tex && !strcmp(app.qr_key, "fixture"));
    now += 2500;
    frame();
    assert(strstr(app.qr_status, "确认登录") && !app.store.login.has_login);
    scanned = true;
    now += 2500;
    frame();
    assert(!app.qr_key[0] && app.store.login.has_login);
    assert(!strcmp(app.account_profile.uname, "Test Anchor"));

    /* An invalid room must not replace the binding or start a roster refresh. */
    strcpy(app.room_input, "404");
    app.room_edited = true;
    frame();
    click("验证并保存");
    assert(!app.room_saving && !app.store.room_id[0] && !app.store.guards.count);
    strcpy(app.room_input, "123");
    click("验证并保存");
    assert(!app.room_edited && !strcmp(app.store.room_id, "123"));
    assert(app.summary_loaded && app.room_info.room_id == 12345 && app.room_info.uid == 42);
    assert(app.valid_dates.count == 2 && bm_date_list_contains(&app.valid_dates, "2020-01-01"));
    assert(app.store.guards.count == 2 && app.store.guards.items[1].uid == 72);
    click("稳健 1500ms");
    assert(app.store.send_interval == 1500 && !strcmp(app.interval_input, "1500"));
}

static void test_room_and_roster(void) {
    click("数据总览");
    assert(app.page == BM_PAGE_SUMMARY);
    text_rect("最低 2 · 最高 5 · 最新 3");
    click("舰长趋势");
    text_rect("区间最高：5");
    click("粉丝趋势");
    text_rect("当前最新：120");
    int w, h;
    cui_texture tex = bm_image_get(ui, COVER_URL, &w, &h);
    assert(tex && w == 640 && h == 64 && bm_image_get(ui, COVER_URL, NULL, NULL) == tex);

    click("管理目标名单");
    assert(app.page == BM_PAGE_GUARD_LIST && app.stats.total == 2);
    click("总督 1");
    assert(!app.filters.type1 && app.store.guards.count == 1 && app.store.guards.items[0].uid == 72);
    assert(app.store.original_guards.count == 2);
    click("总督 1");
    click("获取 / 导入名单");
    click("历史快照");
    app.snapshot_date = (cui_date){2020, 1, 1};
    frame();
    text_rect("有快照");
    click("拉取所选日期快照");
    assert(!app.guard_loading && !app.import_guard_dialog && app.store.guards.count == 1);
    assert(app.store.guards.items[0].uid == 81 && app.stats.type2 == 1);
    text_rect("Carol");
}

static void test_codes_and_template(void) {
    click("兑换码库");
    text_rect("兑换码库存为空");
    strcpy(app.code_import_buf, "KEEP\nREMOVE\n\nKEEP");
    frame();
    text_rect("待入库 3 行");
    click("立即导入入库");
    assert(!app.code_import_buf[0] && app.store.code_count == 2);
    strcpy(app.code_search, "move");
    frame();
    text_rect("显示 1 / 2 个 · 已选 0 个");
    click("全选当前");
    click("删除选中 (1)");
    assert(app.store.code_count == 1 && !strcmp(app.store.codes[0].code, "KEEP"));

    click("模板管理");
    assert(app.page == BM_PAGE_TEMPLATE && !app.store.template_count);
    click("新建模板");
    assert(app.template_changed && !app.store.template_count);
    click("清空正文");
    assert(!app.template_value_buf[0]);
    /* Compose a template using the chips, then check the actual preview. */
    click("{name}");
    click("{code}");
    assert(!strcmp(app.template_value_buf, "{name}{code}"));
    text_rect("CarolKEEP");
    click("总督");
    text_rect("示例总督大佬KEEP");
    click("保存模板");
    assert(!app.template_changed && app.template_select_idx == 0 && app.msg_template_idx == 0);
    assert(!strcmp(bm_store_get_template(&app.store, app.msg_template_name), "{name}{code}"));
    assert(app.store.code_count == 1); /* Previews do not consume codes. */
    click("私信群发");
    assert(app.page == BM_PAGE_MESSAGE && !app.msg_in_progress);
    text_rect("私信群发控制台");
}

static void test_feedback_and_reopen(void) {
    click("关于软件");
    click("问题反馈");
    assert(app.feedback_dialog && !app.feedback_load_failed);
    strcpy(app.feedback.message, "Offline workflow test");
    frame();
    click("提交反馈");
    assert(!app.feedback_busy && app.feedback_result.ok && !strcmp(app.feedback.receipt, RECEIPT));
    text_rect(RECEIPT);
    bm_app_shutdown(&app, ui);

    bm_app_init(&app);
    assert(app.store.login.has_login && !strcmp(app.room_input, "123"));
    assert(app.store.send_interval == 1500 && app.stats.type2 == 1);
    assert(app.msg_template_idx == 0 && !strcmp(app.template_value_buf, "{name}{code}"));
    assert(app.store.code_count == 1 && !strcmp(app.store.codes[0].code, "KEEP"));
    assert(!app.feedback_load_failed && !strcmp(app.feedback.receipt, RECEIPT));
    bm_app_shutdown(&app, ui);
}

int main(int argc, char **argv) {
    /* Refuse to load a real user profile if invoked without the CTest fixture. */
    assert(argc == 2);
    const char *vars[] = {"APPDATA", "HOME", "XDG_CONFIG_HOME"};
    for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++)
        assert(getenv(vars[i]) && !strcmp(getenv(vars[i]), argv[1]));

    static unsigned char cover[800 * 80 * 4];
    assert(cui_write_png("cover.png", cover, 800, 80, 800 * 4));
    bm_http_set_mock(offline_http, NULL);
    bm_set_time_hooks(clock_ms, NULL, NULL);
    bm_jobs_init();
    bm_images_init();
    bm_app_init(&app);
    cui_sw_renderer *renderer = cui_sw_create();
    ui = cui_create(&(cui_ctx_desc){.renderer = cui_sw_iface(renderer)});
    assert(ui);
    cui_set_viewport(ui, 1600, 1080, 1);
    cui_set_reduced_motion(ui, true);

    test_onboarding();
    test_room_and_roster();
    test_codes_and_template();
    test_feedback_and_reopen();
    cui_destroy(ui);
    cui_sw_destroy(renderer);
    bm_jobs_shutdown();
    return 0;
}
