#include "ui/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool save_sent_history(bm_app *app) {
    if (bm_sent_history_save(&app->history)) return true;
    bm_toast_push(app, "发送记录未能保存", "请检查本地配置目录的写入权限和可用空间", CUI_TONE_DANGER);
    return false;
}

static void export_sent_history(bm_app *app) {
    char date[16], name[128], path[1024];
    bm_date_today_str(date, sizeof date);
    snprintf(name, sizeof name, "%s-已发送记录-%s.csv", app->store.room_id, date);
    cui_file_dialog_opts opts = {
        .kind = CUI_FILE_SAVE, .title = "导出已发送记录",
        .filter_name = "CSV 文件", .filter = "csv", .default_name = name
    };
    if (!cui_app_file_dialog(NULL, &opts, path, sizeof path)) return;
    if (bm_sent_history_export_csv(app->history.items, app->history.count, path)) {
        bm_toast_push(app, "导出 CSV 成功", bm_path_filename(path), CUI_TONE_SUCCESS);
    } else {
        bm_toast_push(app, "导出 CSV 失败", "请检查保存位置的写入权限和可用空间", CUI_TONE_DANGER);
    }
}

static void clear_sent_history(bm_app *app) {
    int count = app->history.count;
    app->history.count = 0;
    if (!save_sent_history(app)) app->history.count = count;
}

static const char *current_template_content(const bm_app *app) {
    if (app->msg_template_idx >= 0 && app->msg_template_idx < app->store.template_count) {
        return app->store.templates[app->msg_template_idx].value;
    }
    return "";
}

/* ---- Sequential Send Step Job ---- */

typedef struct {
    bm_login_info login;
    bm_sent_msg sent;
    char message[4096];
    uint64_t roster_generation;
    int interval_ms;
    bool ok;
    bm_bili_err err;
} send_step_job;

static void trigger_next_send_step(bm_app *app);

/* Put the visible queue back in sync with the roster after this pass stops.
   Delivered recipients are already gone from the roster. Failures and people
   who were filtered out of the pass are still in it, so they come back here
   instead of being replaced by the failure list. */
static void release_send_queue(bm_app *app) {
    app->msg_in_progress = false;
    bm_guard_list_clear(&app->msg_remain_list);
    bm_refresh_guard_stats_and_filters(app, true);
}

static void on_send_step_done(void *raw, void *user) {
    send_step_job *d = (send_step_job *)raw;
    bm_app *app = (bm_app *)user;
    app->msg_send_pending = false;
    bool same_roster = d->roster_generation == app->guard_roster_generation;
    bm_diagnostics_event(BM_EVENT_MESSAGE_SEND, d->ok ? BM_LOG_INFO : BM_LOG_ERROR, d->err.code, 1);

    if (!d->ok) {
        char title[160];
        snprintf(title, sizeof title, "向 %s 发送失败", d->sent.target.username);
        bm_toast_push(app, title, d->err.message, CUI_TONE_DANGER);
        if (same_roster) bm_guard_list_push(&app->msg_remain_list, &d->sent.target);
    } else {
        if (!bm_sent_history_append(&app->history, &d->sent)) {
            bm_toast_push(app, "私信已发送，但记录未能保留", "内存不足，群发已暂停", CUI_TONE_DANGER);
            app->msg_in_progress = false;
        } else if (!save_sent_history(app)) {
            app->msg_in_progress = false;
        }
        if (d->sent.code[0]) {
            bm_store_consume_code(&app->store, d->sent.code);
        }
    }

    if (same_roster) {
        bm_guard_note_send_result(&app->store.original_guards, &app->store.guards, &d->sent.target, d->ok);
        app->msg_progress_index++;
    }
    bm_guard_compute_stats(&app->store.original_guards, &app->stats);

    if (!bm_store_save(&app->store)) {
        bm_toast_push(app, "名单和兑换码未能保存", "请检查本地配置目录的写入权限和可用空间", CUI_TONE_DANGER);
        app->msg_in_progress = false;
    }
    free(d);

    if (app->msg_in_progress && !app->msg_is_recalling) {
        trigger_next_send_step(app);
    } else if (!app->msg_is_recalling) {
        release_send_queue(app);
    }
}

static void send_step_worker(void *raw) {
    send_step_job *d = (send_step_job *)raw;
    if (d->sent.target.uid == 0) {
        d->err.code = -1;
        bm_strlcpy(d->err.message, "无效的用户 UID，已跳过", sizeof d->err.message);
    } else {
        d->ok = bm_bili_send_message(&d->login, d->sent.target.uid, d->message,
                                     d->sent.msg_key, sizeof d->sent.msg_key, &d->err);
        if (d->ok) d->sent.timestamp_ms = bm_now_ms();
    }
    if (d->interval_ms > 0) {
        bm_sleep_ms((uint32_t)d->interval_ms);
    }
    bm_job_post_main(on_send_step_done, d);
}

static void trigger_next_send_step(bm_app *app) {
    if (app->msg_send_pending) return;
    if (!app->history.path[0]) {
        bm_toast_push(app, "发送记录不可用，未开始群发", "请检查 sent-history.json 后重启", CUI_TONE_WARNING);
        release_send_queue(app);
        return;
    }
    if (app->store.guards.count == 0) {
        int failed = app->msg_remain_list.count;
        bm_diagnostics_event(BM_EVENT_BATCH_FINISH, BM_LOG_INFO, 0, failed);
        app->msg_is_recalling = false;
        release_send_queue(app);
        bm_toast_push(app, "群发任务已完成",
                      failed > 0 ? "部分发送失败的用户已保留在目标列表中" : "全部目标用户私信均已成功送达",
                      failed > 0 ? CUI_TONE_WARNING : CUI_TONE_SUCCESS);
        return;
    }

    const char *tpl = current_template_content(app);
    bool use_code = strstr(tpl, "{code}") != NULL;
    if (use_code && app->store.code_count <= 0) {
        bm_diagnostics_event(BM_EVENT_BATCH_STOP, BM_LOG_WARNING, 1, 0); /* No redemption codes left. */
        release_send_queue(app);
        bm_toast_push(app, "兑换码已耗尽，群发中止", NULL, CUI_TONE_DANGER);
        return;
    }

    send_step_job *d = (send_step_job *)calloc(1, sizeof *d);
    if (!d) {
        release_send_queue(app);
        return;
    }
    d->login = app->store.login;
    bm_strlcpy(d->sent.sender_uid, d->login.DedeUserID, sizeof d->sent.sender_uid);
    d->sent.target = app->store.guards.items[0];
    d->roster_generation = app->guard_roster_generation;
    d->interval_ms = app->store.send_interval > 0 ? app->store.send_interval : 1000;

    if (use_code) bm_strlcpy(d->sent.code, app->store.codes[0].code, sizeof d->sent.code);
    bm_template_process_today(tpl, &d->sent.target, d->sent.code, d->message, sizeof d->message);

    app->msg_send_pending = true;
    if (!bm_job_spawn(send_step_worker, d)) {
        app->msg_send_pending = false;
        free(d);
        release_send_queue(app);
    }
}

/* ---- Recall Jobs (Batch & Single) ---- */

typedef struct {
    bm_login_info login;
    bm_sent_msg item;
    int index;
    int interval_ms;
    bool is_single;
    bool skipped_timeout;
    bool ok;
    bm_bili_err err;
} recall_step_job;

static void trigger_next_recall_step(bm_app *app);

static bool can_recall_msg(const bm_sent_msg *m, const bm_login_info *login, int64_t now) {
    return login->has_login && m->sender_uid[0] && !strcmp(m->sender_uid, login->DedeUserID)
        && m->msg_key[0] && m->timestamp_ms > 0 && now >= m->timestamp_ms && now - m->timestamp_ms <= 120000;
}

static void on_recall_step_done(void *raw, void *user) {
    recall_step_job *d = (recall_step_job *)raw;
    bm_app *app = (bm_app *)user;
    app->msg_recall_pending = false;
    bm_diagnostics_event(BM_EVENT_MESSAGE_RECALL, d->ok ? BM_LOG_INFO : BM_LOG_WARNING,
                         d->skipped_timeout ? 120 : d->err.code, 1);

    if (d->skipped_timeout) {
        char title[160];
        snprintf(title, sizeof title, "撤回发给 %s 的消息失败", d->item.target.username);
        bm_toast_push(app, title, "消息发送已超过 2 分钟，无法撤回", CUI_TONE_WARNING);
    } else if (!d->ok) {
        char title[160];
        snprintf(title, sizeof title, "撤回发给 %s 的消息失败", d->item.target.username);
        bm_toast_push(app, title, d->err.message, CUI_TONE_DANGER);
    } else {
        /* One recall in flight. Records can be cleared, but not reordered. */
        if (d->index < app->history.count) {
            bm_sent_msg *at = &app->history.items[d->index];
            memmove(at, at + 1, (size_t)(app->history.count - d->index - 1) * sizeof *at);
            app->history.count--;
            save_sent_history(app);
        }
        if (d->is_single) {
            char title[160];
            snprintf(title, sizeof title, "已撤回发给 %s 的私信", d->item.target.username);
            bm_toast_push(app, title, NULL, CUI_TONE_SUCCESS);
        }
    }

    if (!d->is_single) {
        if (!d->ok) app->msg_recall_cursor++;
        app->msg_progress_index++;
        free(d);
        if (app->msg_in_progress && app->msg_is_recalling) {
            trigger_next_recall_step(app);
        }
    } else {
        free(d);
    }
}

static void recall_step_worker(void *raw) {
    recall_step_job *d = (recall_step_job *)raw;
    int64_t now_ms = bm_now_ms();
    if (!can_recall_msg(&d->item, &d->login, now_ms)) {
        d->skipped_timeout = true;
    } else {
        d->ok = bm_bili_recall_message(&d->login, d->item.target.uid, d->item.msg_key, &d->err);
        if (!d->is_single && d->interval_ms > 0) {
            bm_sleep_ms((uint32_t)d->interval_ms);
        }
    }
    bm_job_post_main(on_recall_step_done, d);
}

static bool spawn_recall(bm_app *app, int idx, bool single) {
    recall_step_job *d = (recall_step_job *)calloc(1, sizeof *d);
    if (!d) return false;
    d->login = app->store.login;
    d->item = app->history.items[idx];
    d->index = idx;
    d->interval_ms = app->store.send_interval > 0 ? app->store.send_interval : 1000;
    d->is_single = single;

    if (!bm_job_spawn(recall_step_worker, d)) {
        free(d);
        return false;
    }
    app->msg_recall_pending = true;
    return true;
}

static void trigger_next_recall_step(bm_app *app) {
    int64_t now = bm_now_ms();
    while (app->msg_recall_cursor < app->history.count &&
           !can_recall_msg(&app->history.items[app->msg_recall_cursor], &app->store.login, now)) {
        app->msg_recall_cursor++;
    }
    if (app->msg_recall_cursor >= app->history.count) {
        bm_toast_push(app, "批量撤回执行完毕", NULL, CUI_TONE_INFO);
    } else if (spawn_recall(app, app->msg_recall_cursor, false)) {
        return;
    } else {
        bm_toast_push(app, "无法启动撤回任务", NULL, CUI_TONE_WARNING);
    }
    app->msg_in_progress = false;
    app->msg_is_recalling = false;
}

static void start_single_recall(bm_app *app, int idx) {
    if (idx < 0 || idx >= app->history.count || app->msg_recall_pending) return;
    if (!can_recall_msg(&app->history.items[idx], &app->store.login, bm_now_ms())) return;
    if (!spawn_recall(app, idx, true)) {
        bm_toast_push(app, "无法启动撤回任务", NULL, CUI_TONE_WARNING);
    }
}

void bm_page_message(cui_ctx *ui, bm_app *app) {
    if (!app->store.room_id[0]) {
        bm_render_setting_alert(ui, app);
        return;
    }

    const char *tpl = current_template_content(app);
    bool use_code = strstr(tpl, "{code}") != NULL;
    int guard_cnt = app->store.guards.count;
    int code_cnt = app->store.code_count;
    bool tpl_ready = (app->msg_template_idx >= 0 && tpl[0] != '\0');
    bool code_ready = !use_code || (code_cnt >= guard_cnt && guard_cnt > 0);

    /* Top Pre-Flight Check Bar */
    cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 8, .wrap = true) {
        cui_heading(ui, "私信群发控制台", 3);
        cui_spacer(ui);

        cui_badge(ui, app->store.login.has_login ? "账号已登录" : "账号未登录",
                  .tone = app->store.login.has_login ? CUI_TONE_SUCCESS : CUI_TONE_DANGER, .dot = true);

        char g_badge[64];
        snprintf(g_badge, sizeof g_badge, "待发目标：%d 人", guard_cnt);
        cui_badge(ui, g_badge, .tone = guard_cnt > 0 ? CUI_TONE_SUCCESS : CUI_TONE_WARNING, .dot = true);

        cui_badge(ui, tpl_ready ? "模板已就绪" : "未选择有效模板",
                  .tone = tpl_ready ? CUI_TONE_SUCCESS : CUI_TONE_WARNING, .dot = true);

        if (use_code) {
            char c_badge[64];
            if (code_ready) {
                snprintf(c_badge, sizeof c_badge, "兑换码充足 (%d/%d)", code_cnt, guard_cnt);
            } else {
                snprintf(c_badge, sizeof c_badge, "兑换码不足 (%d/%d)", code_cnt, guard_cnt);
            }
            cui_badge(ui, c_badge, .tone = code_ready ? CUI_TONE_SUCCESS : CUI_TONE_DANGER, .dot = true);
        } else {
            cui_badge(ui, "模板不含兑换码", .tone = CUI_TONE_NEUTRAL);
        }
    }

    /* Main Two-Column Mission Control */
    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 14, .align = CUI_ALIGN_STRETCH) {
        /* Left Column: Configuration + Active Dispatch Queue (GROW 6) */
        cui_column(ui, .width = CUI_GROW(6), .height = CUI_FILL, .gap = 12) {
            /* Card 1: Template & Interval Config */
            cui_card(ui, .width = CUI_FILL, .pad = 14, .gap = 10) {
                cui_row(ui, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
                    cui_icon(ui, CUI_ICON_SLIDERS_HORIZONTAL, 16, CUI_COLOR_ACCENT_STRONG);
                    cui_gap(ui, 6);
                    cui_label(ui, "发送模板与参数配置");
                    cui_spacer(ui);
                    char intv_txt[48];
                    snprintf(intv_txt, sizeof intv_txt, "发送间隔：%d ms", app->store.send_interval);
                    cui_badge(ui, intv_txt, .tone = CUI_TONE_NEUTRAL);
                    if (cui_button(ui, "编辑模板", .icon = CUI_ICON_PENCIL, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST)) {
                        app->page = BM_PAGE_TEMPLATE;
                    }
                }

                const char *names[256];
                int tpl_n = app->store.template_count < 256 ? app->store.template_count : 256;
                for (int i = 0; i < tpl_n; i++) {
                    names[i] = app->store.templates[i].name;
                }
                if (cui_select(ui, "msg_tpl_sel", names, tpl_n, &app->msg_template_idx,
                               .placeholder = "请选择用于群发的私信模板", .width = CUI_FILL, .disabled = app->msg_in_progress)) {
                    if (app->msg_template_idx >= 0 && app->msg_template_idx < app->store.template_count) {
                        bm_strlcpy(app->msg_template_name, app->store.templates[app->msg_template_idx].name, sizeof app->msg_template_name);
                    }
                }
            }

            /* Card 2: Live Progress, Queue & Next Message Preview */
            cui_card(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 16, .gap = 12) {
                float pct = 0.0f;
                if (app->msg_total_count > 0) {
                    pct = (float)app->msg_progress_index / (float)app->msg_total_count;
                    if (pct > 1.0f) pct = 1.0f;
                }

                cui_row(ui, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
                    cui_icon(ui, CUI_ICON_SEND, 16, CUI_COLOR_ACCENT_STRONG);
                    cui_gap(ui, 6);
                    char prog_txt[96];
                    snprintf(prog_txt, sizeof prog_txt, "%s进度：%d / %d",
                             app->msg_is_recalling ? "撤回" : "群发",
                             app->msg_progress_index, app->msg_total_count);
                    cui_label(ui, prog_txt);
                    cui_spacer(ui);
                    char pct_txt[32];
                    snprintf(pct_txt, sizeof pct_txt, "%.0f%%", pct * 100.0f);
                    cui_badge(ui, pct_txt, .tone = app->msg_in_progress ? CUI_TONE_INFO : CUI_TONE_NEUTRAL);
                }

                cui_progress(ui, pct, .tone = CUI_TONE_WARNING, .height = 8, .width = CUI_FILL);

                /* Target Queue Strip */
                cui_row(ui, .gap = 6, .align = CUI_ALIGN_CENTER, .wrap = true, .width = CUI_FILL) {
                    cui_muted(ui, "待发队列：");
                    int show_n = guard_cnt < 5 ? guard_cnt : 5;
                    for (int i = 0; i < show_n; i++) {
                        cui_badge(ui, app->store.guards.items[i].username,
                                  .tone = i == 0 ? CUI_TONE_INFO : CUI_TONE_NEUTRAL,
                                  .dot = (i == 0));
                    }
                    if (guard_cnt > 5) {
                        char more_txt[32];
                        snprintf(more_txt, sizeof more_txt, "+%d 人", guard_cnt - 5);
                        cui_badge(ui, more_txt, .tone = CUI_TONE_NEUTRAL);
                    }
                    if (guard_cnt == 0) {
                        cui_muted(ui, "队列为空（请先在「目标列表」中添加或拉取目标用户）");
                    }
                }

                /* Live Next Message Bubble Preview */
                char preview[4096] = { 0 };
                if (guard_cnt > 0 && tpl[0]) {
                    const char *code_str = (use_code && code_cnt > 0) ? app->store.codes[0].code : "";
                    bm_template_process_today(tpl, &app->store.guards.items[0], code_str, preview, sizeof preview);
                }

                cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 12, .radius = 10,
                        .bg = CUI_COLOR_SURFACE_MUTED, .gap = 8, .scroll_y = true) {
                    if (guard_cnt > 0) {
                        bm_guard *first_g = &app->store.guards.items[0];
                        cui_row(ui, .align = CUI_ALIGN_CENTER, .gap = 8, .width = CUI_FILL) {
                            cui_texture ftex = bm_image_get(ui, first_g->face, NULL, NULL);
                            cui_avatar(ui, first_g->username, .size = 24, .image = ftex);
                            char next_hdr[160];
                            snprintf(next_hdr, sizeof next_hdr, "下一条将发送给：%s (UID %llu)",
                                     first_g->username, (unsigned long long)first_g->uid);
                            cui_text(ui, next_hdr, .font = CUI_FONT_MEDIUM, .size = 12);
                        }
                        cui_separator(ui);
                    }
                    cui_chat_message(ui, preview[0] ? preview : "（选择模板并准备好目标列表后，此处将显示即将发送的下一条私信内容）",
                                     .mine = true,
                                     .status = app->msg_in_progress ? "正在群发中..." : "待发送预览");
                }

                /* Action Bar */
                cui_row(ui, .gap = 10, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
                    if (!app->msg_in_progress) {
                        if (cui_button(ui, "开始群发私信", .icon = CUI_ICON_SEND, .variant = CUI_BUTTON_PRIMARY,
                                       .disabled = app->msg_send_pending || app->msg_recall_pending || guard_cnt == 0 || !tpl_ready || !app->store.login.has_login)) {
                            if (use_code && code_cnt < guard_cnt) {
                                bm_toast_push(app, "兑换码数量不足", "请先补充兑换码或移除模板中的 {code} 标签", CUI_TONE_DANGER);
                            } else {
                                app->msg_in_progress = true;
                                app->msg_is_recalling = false;
                                app->msg_progress_index = 0;
                                app->msg_total_count = guard_cnt;
                                bm_guard_list_clear(&app->msg_remain_list);
                                trigger_next_send_step(app);
                            }
                        }
                    } else if (!app->msg_is_recalling) {
                        if (cui_button(ui, "暂停群发", .icon = CUI_ICON_PAUSE, .variant = CUI_BUTTON_DANGER)) {
                            app->msg_in_progress = false;
                            bm_toast_push(app, "已暂停群发任务", "等待已发出的请求完成后可继续，剩余用户会保留", CUI_TONE_INFO);
                        }
                    }

                    if (cui_button(ui, "跳过队首用户", .icon = CUI_ICON_SKIP_FORWARD, .size = CUI_SIZE_SM,
                                   .variant = CUI_BUTTON_OUTLINE, .disabled = app->msg_in_progress || app->msg_send_pending || guard_cnt == 0)) {
                        uint64_t skip_uid = app->store.guards.items[0].uid;
                        char skip_name[128];
                        bm_strlcpy(skip_name, app->store.guards.items[0].username, sizeof skip_name);
                        bm_guard_list_remove_uid(&app->store.original_guards, skip_uid);
                        bm_refresh_guard_stats_and_filters(app, true);
                        bm_toast_push(app, "已跳过并移除队首用户", skip_name, CUI_TONE_INFO);
                    }

                    cui_spacer(ui);
                    cui_muted(ui, "已发用户与兑换码自动出队");
                }
            }
        }

        /* Right Column: Sent History & Recall Console (GROW 5) */
        cui_card(ui, .width = CUI_GROW(5), .height = CUI_FILL, .pad = 0, .gap = 0) {
            int64_t now_ms = bm_now_ms();
            int recallable_cnt = 0;
            for (int i = 0; i < app->history.count; i++) {
                if (can_recall_msg(&app->history.items[i], &app->store.login, now_ms)) {
                    recallable_cnt++;
                }
            }

            if (recallable_cnt > 0) {
                cui_request_wakeup(ui, 1.0f);
            }

            /* Header inside Card */
            cui_column(ui, .width = CUI_FILL, .pad = 14, .gap = 8, .bg = CUI_COLOR_SURFACE_MUTED) {
                cui_row(ui, .align = CUI_ALIGN_CENTER, .width = CUI_FILL, .gap = 8) {
                    cui_icon(ui, CUI_ICON_ROTATE_CCW, 16, CUI_COLOR_ACCENT_STRONG);
                    cui_label(ui, "已发送记录与撤回中心");
                    cui_spacer(ui);
                    char sent_b[48];
                    snprintf(sent_b, sizeof sent_b, "共 %d 条记录", app->history.count);
                    cui_badge(ui, sent_b, .tone = CUI_TONE_NEUTRAL);
                    if (recallable_cnt > 0) {
                        char rec_b[48];
                        snprintf(rec_b, sizeof rec_b, "%d 条可撤回", recallable_cnt);
                        cui_badge(ui, rec_b, .tone = CUI_TONE_SUCCESS, .dot = true);
                    }
                }

                cui_muted(ui, "Bilibili 仅支持撤回发送后 2 分钟（120 秒）内的私信，支持单条撤回或一键全部撤回");

                cui_row(ui, .gap = 8, .width = CUI_FILL, .wrap = true) {
                    if (!app->msg_in_progress) {
                        cui_disabled_scope(ui, app->msg_send_pending || app->msg_recall_pending || recallable_cnt == 0) {
                            if (cui_confirm_button(ui, "一键撤回全部消息", .confirm_label = "确认全部撤回",
                                                   .icon = CUI_ICON_ROTATE_CCW, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_DANGER)) {
                                app->msg_in_progress = true;
                                app->msg_is_recalling = true;
                                app->msg_progress_index = 0;
                                app->msg_total_count = recallable_cnt;
                                app->msg_recall_cursor = 0;
                                trigger_next_recall_step(app);
                            }
                        }
                    } else if (app->msg_is_recalling) {
                        if (cui_button(ui, "停止批量撤回", .icon = CUI_ICON_PAUSE, .size = CUI_SIZE_SM,
                                       .variant = CUI_BUTTON_SECONDARY, .width = CUI_GROW(1))) {
                            app->msg_in_progress = false;
                            app->msg_is_recalling = false;
                        }
                    }

                    if (cui_button(ui, "导出 CSV", .icon = CUI_ICON_DOWNLOAD, .size = CUI_SIZE_SM,
                                   .variant = CUI_BUTTON_OUTLINE, .disabled = app->history.count == 0)) {
                        export_sent_history(app);
                    }
                    cui_disabled_scope(ui, app->history.count == 0) {
                        if (cui_confirm_button(ui, "清空记录", .confirm_label = "确认清空", .icon = CUI_ICON_TRASH_2, .size = CUI_SIZE_SM)) {
                            clear_sent_history(app);
                        }
                    }
                }
            }

            cui_separator(ui);

            if (app->history.count == 0) {
                cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                    cui_empty_state(ui, "暂无已发送私信记录", .icon = CUI_ICON_MESSAGE_SQUARE,
                                    .description = "群发私信开始后，每一条送达的私信都会在此处实时显示，并可在 120 秒内随时撤回");
                }
            } else {
                int first = 0, last = 0;
                int single_recall_idx = -1;
                cui_virtual_list(ui, "sent_msgs_vlist", app->history.count, 52.0f, CUI_FILL, &first, &last) {
                    for (int i = first; i < last; i++) {
                        /* Show newest first */
                        int r_idx = app->history.count - 1 - i;
                        const bm_sent_msg *sm = &app->history.items[r_idx];
                        int64_t elapsed_s = sm->timestamp_ms > 0 ? (now_ms - sm->timestamp_ms) / 1000 : 999;
                        bool can_recall = can_recall_msg(sm, &app->store.login, now_ms);

                        cui_push_id(ui, sm->msg_key);
                        cui_row(ui, .width = CUI_FILL, .height = CUI_PX(52.0f), .pad_x = 14, .gap = 8, .align = CUI_ALIGN_CENTER) {
                            cui_column(ui, .width = CUI_FILL, .gap = 2) {
                                cui_row(ui, .gap = 6, .align = CUI_ALIGN_CENTER) {
                                    cui_text(ui, sm->target.username, .font = CUI_FONT_MEDIUM, .size = 13, .nowrap = true);
                                    char uid_str[32];
                                    snprintf(uid_str, sizeof uid_str, "UID %llu", (unsigned long long)sm->target.uid);
                                    cui_muted(ui, uid_str);
                                }
                                char key_str[96];
                                snprintf(key_str, sizeof key_str, "msg_key: %s", sm->msg_key[0] ? sm->msg_key : "-");
                                cui_muted(ui, key_str);
                            }

                            if (can_recall) {
                                char rem_txt[48];
                                snprintf(rem_txt, sizeof rem_txt, "剩余 %llds", (long long)(120 - elapsed_s));
                                cui_badge(ui, rem_txt, .tone = CUI_TONE_SUCCESS);
                                cui_disabled_scope(ui, app->msg_in_progress || app->msg_send_pending || app->msg_recall_pending) {
                                    if (cui_confirm_button(ui, "撤回", .confirm_label = "确认撤回", .icon = CUI_ICON_ROTATE_CCW,
                                                           .size = CUI_SIZE_SM, .variant = CUI_BUTTON_OUTLINE)) {
                                        single_recall_idx = r_idx;
                                    }
                                }
                            } else {
                                cui_badge(ui, elapsed_s >= 120 ? "已超 2 分钟" : "不可撤回", .tone = CUI_TONE_NEUTRAL);
                            }
                        }
                        cui_pop_id(ui);
                    }
                }
                if (single_recall_idx >= 0) {
                    start_single_recall(app, single_recall_idx);
                }
            }
        }
    }
}
