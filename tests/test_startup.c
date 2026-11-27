/* Drive the native entry point with a one-frame software window, not the user's desktop. */
#include "ui/app.h"
#include "core/http.h"
#include <cui/cui_render.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "Startup tests require assertions enabled"
#endif

static cui_sw_renderer *renderer;
static cui_ctx *ctx;
static int frames;

static cui_app *create_window(const cui_app_desc *desc) {
    assert(!strcmp(desc->title, "BiliMessenger") && desc->theme == CUI_THEME_LIGHT);
    assert(desc->width >= desc->min_width && desc->height >= desc->min_height);
    renderer = cui_sw_create();
    ctx = cui_create(&(cui_ctx_desc){.renderer = cui_sw_iface(renderer), .theme = desc->theme});
    assert(ctx);
    cui_set_viewport(ctx, (float)desc->width, (float)desc->height, 1);
    return (cui_app *)ctx;
}
static void *native_handle(cui_app *window) { (void)window; return NULL; }
static cui_ctx *window_ui(cui_app *window) { assert(window == (cui_app *)ctx); return ctx; }
static bool next_frame(cui_app *window) {
    assert(window == (cui_app *)ctx);
    if (frames) return false;
    cui_begin_frame(ctx, 1.0f / 60.0f);
    return true;
}
static void destroy_window(cui_app *window) {
    assert(window == (cui_app *)ctx && frames == 1);
    cui_end_frame(ctx);
    cui_destroy(ctx);
    cui_sw_destroy(renderer);
}

static bool mock_update(const bm_http_req *req, bm_http_resp *resp, void *unused) {
    (void)unused;
    assert(strstr(req->url, "/releases/latest"));
    resp->ok = true;
    resp->status = 200;
    resp->data = bm_strdup("{\"tag_name\":\"v" BM_VERSION "\"}");
    resp->size = strlen(resp->data);
    return true;
}

static void inspect_frame(cui_ctx *ui, bm_app *app) {
    assert(app->window_initialized && app->page == BM_PAGE_GUARD_LIST);
    assert(!app->store.dark_mode && app->accent_idx == 2);
    assert(app->import_guard_dialog && app->feedback_dialog && app->feedback_log_preview);
    assert(app->feedback.log[0]);
    /* Do not tear down the profile while the startup check is still running. */
    while (bm_jobs_active_count()) {
        bm_jobs_poll_main(app);
        bm_sleep_ms(1);
    }
    bm_app_frame(ui, app);
    assert(!app->update_checking && cui_get_theme_mode(ui) == CUI_THEME_LIGHT);
    frames++;
}

static void init_test_profile(bm_app *app) {
    const char *home = getenv("BM_TEST_PROFILE");
    assert(home && getenv("HOME") && !strcmp(home, getenv("HOME")));
    assert(getenv("APPDATA") && !strcmp(home, getenv("APPDATA")));
    assert(getenv("XDG_CONFIG_HOME") && !strcmp(home, getenv("XDG_CONFIG_HOME")));
    bm_http_set_mock(mock_update, NULL);
    bm_app_init(app);
}

#define bm_app_init init_test_profile
#define cui_app_create create_window
#define cui_app_native_window native_handle
#define cui_app_ui window_ui
#define cui_app_frame next_frame
#define cui_app_destroy destroy_window
#define bm_app_frame inspect_frame
#include "../src/main.c"
