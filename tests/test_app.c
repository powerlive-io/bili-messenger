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
static bool expired_account;
static bool room_offline;
static unsigned feedback_attempts;
static char opened_url[512];

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
        body = expired_account ? "{\"code\":-101,\"message\":\"expired\"}"
                               : "{\"code\":0,\"data\":{\"mid\":42,\"uname\":\"Test Anchor\"}}";
    } else if (strstr(url, "/space/info?uid=77&")) {
        body = "{\"code\":0,\"data\":{\"mid\":77,\"uname\":\"Guest\"}}";
    } else if (strstr(url, "/search/type?keyword=Guest&")) {
        body = "{\"code\":0,\"data\":{\"result\":[{\"mid\":77,\"uname\":\"Guest\"}]}}";
    } else if (strstr(url, "/search/type?keyword=Missing&")) {
        body = "{\"code\":0,\"data\":{\"result\":[]}}";
    } else if (strstr(url, "getH5InfoByRoom?room_id=123")) {
        body = room_offline ? "{\"code\":0,\"data\":{\"room_info\":{\"room_id\":12345,\"uid\":42}}}"
               : "{\"code\":0,\"data\":{\"room_info\":{\"room_id\":12345,\"uid\":42,"
               "\"title\":\"Test Room\",\"cover\":\"" COVER_URL "\",\"online\":321,\"live_status\":1},"
               "\"anchor_info\":{\"base_info\":{\"uname\":\"Test Anchor\"},\"relation_info\":{\"attention\":100}}}}";
    } else if (strstr(url, "getH5InfoByRoom?room_id=404")) {
        body = "{\"code\":-1}";
    } else if (strstr(url, "/bulkGuard/42") || strstr(url, "/bulkActiveSome/42")) {
        body = room_offline ? "[]" : "[{\"time\":1700000000000,\"guardNum\":2,\"follower\":100},"
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
    } else if (strstr(url, "/history?room=123&date=2020-01-02")) {
        body = "{\"Code\":404}";
    } else if (strstr(url, "/giftStream/getReceivedGiftStream")) {
        assert(strstr(req->cookie, "DedeUserID=42"));
        body = "{\"code\":0,\"data\":{\"ready\":1,\"total_page\":1,\"list\":["
               "{\"uid\":\"77\",\"uname\":\"Guest\",\"goods_id\":900,\"name\":\"other\"},"
               "{\"uid\":77,\"uname\":\"Guest\",\"goods_id\":5},"
               "{\"uid\":81,\"uname\":\"Carol\",\"goods_id\":6}]}}";
    } else if (!strcmp(url, COVER_URL)) {
        assert(bm_read_file("cover.png", &resp->data, &resp->size));
    } else if (!strcmp(url, "https://api.powerlive.io/feedback/prepare")) {
        assert(!req->cookie);
        feedback_attempts++;
        body = "{\"upload_token\":\"fixture\",\"expires_at\":\"2099-01-01T00:00:00Z\","
               "\"max_file_bytes\":5242880,\"max_request_bytes\":11534336}";
    } else if (!strcmp(url, "https://api.powerlive.io/feedback")) {
        assert(req->part_count == 2 && !strcmp(req->parts[0].data, "Offline workflow test"));
        assert(!strcmp(req->parts[1].filename, "diagnostics.log") && req->parts[1].size > 0);
        if (feedback_attempts == 1) {
            resp->status = 503;
            resp->retry_after_seconds = 60;
            body = "{\"error\":\"feedback_unavailable\"}";
        } else {
            body = "{\"id\":\"" RECEIPT "\"}";
        }
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
    if (!resp->status) resp->status = 200;
    return true;
}

bool cui_app_open_url(const char *url) {
    assert(!opened_url[0]); /* Each expected navigation must be consumed. */
    bm_strlcpy(opened_url, url, sizeof opened_url);
    return true;
}
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

static void check_link(const char *label, const char *expected) {
    click(label);
    assert(!strcmp(opened_url, expected));
    opened_url[0] = '\0';
}

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
    check_link("主播空间", "https://space.bilibili.com/42");
    check_link("进入直播间", "https://live.bilibili.com/123");
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

static void add_target(const char *keyword, const char *title, bool by_name) {
    click("获取 / 导入名单");
    click("手动添加");
    click(by_name ? "按用户名搜索 (需已登录)" : "按 UID 精确查找");
    strcpy(app.add_guard_keyword, keyword);
    strcpy(app.add_guard_title, title);
    frame();
    click("确认添加并加入列表");
    assert(!app.guard_loading && !app.import_guard_dialog);
}

static void test_edit_roster(void) {
    /* A failed snapshot must not discard the working list. */
    bm_guard_list_fetch_snapshot(&app, "2020-01-02");
    drain_jobs();
    assert(app.store.guards.count == 1 && app.store.guards.items[0].uid == 81);
    add_target("   ", "", false);
    add_target("not-a-number", "", false);
    add_target("Missing", "", true);
    assert(app.store.guards.count == 1);
    add_target(" 77 ", "Moderator", false);
    assert(app.store.guards.count == 2 && app.store.guards.items[0].uid == 77);
    assert(!strcmp(app.store.guards.items[0].custom_title, "Moderator"));
    add_target("77", "", false);
    add_target("Guest", "", true);
    assert(app.store.original_guards.count == 2); /* Don't add duplicate by either key. */
    strcpy(app.guard_search, "modEr");
    frame();
    text_rect("筛选出 1 / 2 人 · 已选 0 人");
    check_link("空间", "https://space.bilibili.com/77");
    check_link("私信", "https://message.bilibili.com/#/whisper/mid77");
    click("全选当前");
    click("删除选中 (1)");
    assert(app.store.original_guards.count == 1 && app.store.guards.items[0].uid == 81);
    click("清除搜索");
    assert(!app.guard_search[0]);

    click("获取 / 导入名单");
    click("上舰流水");
    click("开始拉取区间上舰流水");
    assert(app.store.original_guards.count == 3 && app.stats.type3 == 1);
    click("一键去重");
    assert(app.store.original_guards.count == 2 && app.stats.type1 == 1 && app.stats.type3 == 0);
    strcpy(app.guard_search, "77");
    frame();
    click("全选当前");
    click("删除选中 (1)");
    assert(app.store.guards.count == 1 && app.store.guards.items[0].uid == 81);
    click("清除搜索");
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
    check_link("关注 @轴伊Joi_Channel", "https://space.bilibili.com/61639371");
    check_link("GitHub 仓库", "https://github.com/powerlive-io/bili-messenger");
    check_link("作者 Bilibili (@Xinrea)", "https://space.bilibili.com/475210");
    check_link("PowerLive 官网", "https://powerlive.io/");
    click("问题反馈");
    assert(app.feedback_dialog && !app.feedback_load_failed);
    strcpy(app.feedback.message, "Offline workflow test");
    frame();
    click("提交反馈");
    assert(!app.feedback_busy && app.feedback_result.retryable && !app.feedback.receipt[0]);
    assert(app.feedback.retry_at_ms == now + 60000);
    char id[sizeof app.feedback.submission_id];
    strcpy(id, app.feedback.submission_id);
    click("重试原反馈");
    assert(feedback_attempts == 1); /* Cooldown disables the button. */
    now += 60000;
    frame();
    click("重试原反馈");
    assert(feedback_attempts == 2 && !strcmp(app.feedback.submission_id, id));
    assert(!app.feedback_busy && app.feedback_result.ok && !strcmp(app.feedback.receipt, RECEIPT));
    text_rect(RECEIPT);
    bm_app_shutdown(&app, ui);

    bm_app_init(&app);
    assert(app.store.login.has_login && !strcmp(app.room_input, "123"));
    assert(app.store.send_interval == 1500 && app.stats.type2 == 1);
    assert(app.msg_template_idx == 0 && !strcmp(app.template_value_buf, "{name}{code}"));
    assert(app.store.code_count == 1 && !strcmp(app.store.codes[0].code, "KEEP"));
    assert(!app.feedback_load_failed && !strcmp(app.feedback.receipt, RECEIPT));
    expired_account = true;
    bm_setting_refresh_account(&app);
    drain_jobs();
    assert(!app.store.login.has_login && !app.account_profile.uname[0]);
    bm_app_shutdown(&app, ui);
    bm_app_init(&app);
    assert(!app.store.login.has_login && app.store.guards.count == 1 && app.store.template_count == 1);
}

static void test_account_recovery(void) {
    expired_account = false;
    app.page = BM_PAGE_SETTING;
    app.login_tab = 1;
    frame();
    frame();
    click("验证并保存 Cookie 登录");
    assert(!app.store.login.has_login);
    strcpy(app.manual_sessdata, "test-session");
    strcpy(app.manual_uid, "42");
    strcpy(app.manual_jct, "test-csrf");
    frame();
    click("验证并保存 Cookie 登录");
    assert(app.store.login.has_login && !strcmp(app.account_profile.uname, "Test Anchor"));
    check_link("个人空间", "https://space.bilibili.com/42");
    click("重新校验状态");
    assert(app.store.login.has_login && app.account_profile.mid == 42);
    click("保守 2500ms");
    assert(app.store.send_interval == 2500);
    click("标准 1000ms");
    assert(app.store.send_interval == 1000);
    click("浅色模式");
    assert(!app.store.dark_mode && cui_get_theme_mode(ui) == CUI_THEME_LIGHT);
    click("琥珀金");
    assert(app.accent_idx == 1);
    click("紫罗兰");
    assert(app.accent_idx == 3);
    click("退出登录");
    assert(app.store.login.has_login); /* First click only arms confirmation. */
    click_rect(cui_get_rect(ui, cui_make_id(ui, "退出登录")));
    assert(!app.store.login.has_login && !app.account_profile.uname[0]);
    assert(app.store.original_guards.count == 1 && !strcmp(app.store.room_id, "123"));
}

static void run_command(const char *title) {
    click("命令");
    assert(app.cmd_palette_open);
    cui_input_text(ui, title);
    frame();
    frame();
    click(title);
    assert(!app.cmd_palette_open);
}

static void test_workspace_commands(void) {
    run_command("前往：目标列表");
    assert(app.page == BM_PAGE_GUARD_LIST);
    run_command("动作：拉取今日实时舰长快照");
    assert(app.store.original_guards.count == 2 && app.stats.type1 == 1);
    bm_guard duplicate = app.store.original_guards.items[0];
    assert(bm_guard_list_push(&app.store.original_guards, &duplicate));
    run_command("动作：目标列表一键去重");
    assert(app.store.original_guards.count == 2 && app.store.guards.count == 2);
    run_command("动作：切换深色 / 浅色主题");
    assert(app.store.dark_mode && cui_get_theme_mode(ui) == CUI_THEME_DARK);
    room_offline = true;
    run_command("动作：刷新直播间与趋势数据");
    click("房间 123");
    assert(app.page == BM_PAGE_SUMMARY && app.room_info.live_status == 0);
    text_rect("暂无 vtbs.moe 舰长历史数据");
    click("舰长趋势");
    text_rect("暂无 vtbs.moe 舰长历史数据");
    click("粉丝趋势");
    text_rect("暂无 vtbs.moe 粉丝历史数据");
    click("前往群发私信");
    assert(app.page == BM_PAGE_MESSAGE);
    text_rect("兑换码不足 (1/2)");
    click("跳过队首用户");
    assert(app.store.guards.count == 1 && app.store.guards.items[0].uid == 72);
}

static void test_template_removal(void) {
    click("编辑模板");
    assert(app.page == BM_PAGE_TEMPLATE && app.store.template_count == 1);
    click("新建模板");
    click("保存模板");
    assert(app.store.template_count == 2 && app.template_select_idx == 1);
    assert(app.msg_template_idx == 0); /* Editing a second template doesn't switch the send queue. */
    click("新模板 1");
    assert(app.template_select_idx == 0 && !strcmp(app.template_value_buf, "{name}{code}"));
    click("提督");
    text_rect("示例提督伙伴KEEP");
    click("删除");
    assert(app.store.template_count == 2);
    click_rect(cui_get_rect(ui, cui_make_id(ui, "删除")));
    assert(app.store.template_count == 1 && app.template_select_idx == 0);
    assert(!strcmp(app.msg_template_name, "新模板 2"));
    click("删除");
    click_rect(cui_get_rect(ui, cui_make_id(ui, "删除")));
    assert(!app.store.template_count && app.msg_template_idx == -1 && !app.msg_template_name[0]);
    click("私信群发");
    text_rect("未选择有效模板");
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
    test_edit_roster();
    test_codes_and_template();
    test_feedback_and_reopen();
    test_account_recovery();
    test_workspace_commands();
    test_template_removal();
    bm_app_shutdown(&app, ui);
    cui_destroy(ui);
    cui_sw_destroy(renderer);
    bm_jobs_shutdown();
    return 0;
}
