#include "ui/app.h"
#include "core/http.h"

#include <stdlib.h>
#include <string.h>

int main(void) {
    bm_http_init();
    bm_jobs_init();
    bm_images_init();

    static bm_app app;
    bm_app_init(&app);
    const char *env_page = getenv("BM_PAGE");
    if (env_page && *env_page) {
        int p = atoi(env_page);
        if (p >= 0 && p < BM_PAGE_COUNT) {
            app.page = (bm_page_id)p;
        }
    }
    const char *env_theme = getenv("BM_THEME");
    if (env_theme && *env_theme) {
        app.store.dark_mode = (strcmp(env_theme, "dark") == 0);
    }
    const char *env_accent = getenv("BM_ACCENT");
    if (env_accent && *env_accent) {
        app.accent_idx = atoi(env_accent);
    }
    if (getenv("BM_IMPORT_DIALOG")) {
        app.import_guard_dialog = true;
    }
    if (getenv("BM_FEEDBACK_DIALOG")) {
        app.feedback_dialog = true;
        if (!strcmp(getenv("BM_FEEDBACK_DIALOG"), "preview") && app.feedback.include_log) {
            if (!app.feedback.log[0]) bm_feedback_capture_log(&app.feedback);
            app.feedback_log_preview = true;
        }
    }
    bm_setting_startup_checks(&app);

    cui_app_desc desc = {
        .title = "BiliMessenger",
        .width = 1280,
        .height = 760,
        .min_width = 1024,
        .min_height = 640,
        .theme = app.store.dark_mode ? CUI_THEME_DARK : CUI_THEME_LIGHT,
        .accent = CUI_ACCENT_BLUE
    };

    cui_app *capp = cui_app_create(&desc);
    if (!capp) return 1;
    app.native_window = cui_app_native_window(capp);
    bm_window_setup(app.native_window);
    app.window_initialized = true;

    while (cui_app_frame(capp)) {
        bm_app_frame(cui_app_ui(capp), &app);
    }
    cui_app_destroy(capp);

    bm_app_shutdown(&app, NULL);
    bm_jobs_shutdown();
    bm_http_cleanup();
    return 0;
}
