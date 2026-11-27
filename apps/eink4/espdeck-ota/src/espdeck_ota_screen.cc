#include "espdeck_ota_screen.h"
#include "espdeck_ota_config.h"

#include "board.h"
#include "fontpack_lvgl.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include "screen_common.h"
#include "vk_key_handler.h"
#include <wifi_station.h>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* kTag = "EspdeckOTA";

// 4-level e-ink monochrome palette
inline lv_color_t ColInk() { return lv_color_black(); }
inline lv_color_t ColPaper() { return lv_color_white(); }
inline lv_color_t ColMuted() { return lv_color_hex(0x555555); }
inline lv_color_t ColRule() { return lv_color_hex(0xBDBDBD); }

const lv_font_t* FontHero() {
    if (const lv_font_t* f = fontpack_lv_font_get(30, 4)) return f;
    if (const lv_font_t* f = fontpack_lv_font_get(30, 2)) return f;
    if (const lv_font_t* f = fontpack_lv_font_ui()) return f;
    return LV_FONT_DEFAULT;
}

const lv_font_t* FontBodyBold() {
    if (const lv_font_t* f = fontpack_lv_font_get(25, 4)) return f;
    if (const lv_font_t* f = fontpack_lv_font_ui()) return f;
    return LV_FONT_DEFAULT;
}

const lv_font_t* FontBody() {
    if (const lv_font_t* f = fontpack_lv_font_ui()) return f;
    return LV_FONT_DEFAULT;
}

const lv_font_t* FontSmall() {
    if (const lv_font_t* f = fontpack_lv_font_get(18, 2)) return f;
    return FontBody();
}

enum class Cmd : int {
    None = 0,
    Check = 1,
    Upgrade = 2,
};

enum class UiPhase : int {
    Idle = 0,
    Checking = 1,
    Ready = 2,
    Upgrading = 3,
    Error = 4,
    Done = 5,
    UpToDate = 6,
};

struct OtaView {
    lv_obj_t* screen = nullptr;

    // Top bar
    lv_obj_t* btn_back = nullptr;
    lv_obj_t* wifi_dot = nullptr;
    lv_obj_t* lbl_wifi = nullptr;

    // Hero block
    lv_obj_t* emblem_box = nullptr;
    lv_obj_t* badge_pill = nullptr;
    lv_obj_t* lbl_badge = nullptr;
    lv_obj_t* lbl_hero = nullptr;
    lv_obj_t* lbl_sub = nullptr;

    // Middle panels (mutually exclusive by phase)
    lv_obj_t* panel_idle = nullptr;
    lv_obj_t* panel_release = nullptr;
    lv_obj_t* lbl_rel_size = nullptr;
    lv_obj_t* apps_chips_wrap = nullptr;

    lv_obj_t* panel_progress = nullptr;
    lv_obj_t* lbl_pct = nullptr;
    lv_obj_t* bar = nullptr;
    lv_obj_t* lbl_prog_hint = nullptr;

    // Status message line (for errors / checking / notes)
    lv_obj_t* lbl_notice = nullptr;

    // Bottom action dock
    lv_obj_t* btn_primary = nullptr;
    lv_obj_t* lbl_primary = nullptr;
    lv_obj_t* btn_secondary = nullptr;
    lv_obj_t* lbl_secondary = nullptr;

    lv_timer_t* ui_timer = nullptr;

    std::atomic<bool> alive{true};
    std::atomic<int> pending_cmd{static_cast<int>(Cmd::None)};
    std::atomic<bool> busy{false};
    std::atomic<bool> has_release{false};
    std::atomic<int> phase{static_cast<int>(UiPhase::Idle)};
    std::atomic<int> progress{0};
    std::atomic<bool> ui_dirty{true};

    SemaphoreHandle_t lock = nullptr;
    std::string rel_version;
    std::string rel_sha256;
    std::string rel_bin_url;
    std::vector<std::string> rel_apps;
    size_t rel_size = 0;
    std::string notice_text;

    int last_wifi_state = -1;
    int last_rendered_phase = -1;
    std::string last_rendered_ver;

    ~OtaView() {
        if (lock) {
            vSemaphoreDelete(lock);
            lock = nullptr;
        }
    }
};

std::shared_ptr<OtaView> g_ota;

struct LockGuard {
    SemaphoreHandle_t h;
    explicit LockGuard(SemaphoreHandle_t s) : h(s) {
        if (h) xSemaphoreTake(h, portMAX_DELAY);
    }
    ~LockGuard() {
        if (h) xSemaphoreGive(h);
    }
};

void setPhase(OtaView* v, UiPhase p, const std::string& notice = "") {
    {
        LockGuard g(v->lock);
        v->notice_text = notice;
    }
    v->phase.store(static_cast<int>(p));
    v->ui_dirty.store(true);
}

std::string remoteBaseUrl() {
    std::string url = ESPDECK_OTA_REMOTE_URL;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

void ensureWifiUp() {
    auto& wifi = WifiStation::GetInstance();
    if (wifi.IsConnected()) return;
    if (wifi.IsLpPaused()) {
        if (wifi.ResumeFromLp() != ESP_OK) {
            wifi.Start();
        }
    } else {
        wifi.Start();
    }
}

bool httpGetString(const std::string& url, std::string& out_body, std::string& out_err) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        out_err = "网络接口不可用";
        return false;
    }
    auto http = network->CreateHttp(0);
    if (!http) {
        out_err = "网络初始化失败";
        return false;
    }

    http->SetTimeout(10000);
    if (!http->Open("GET", url)) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "无法连接服务器 (0x%x)", http->GetLastError());
        out_err = buf;
        ESP_LOGE(kTag, "GET %s failed: 0x%x", url.c_str(), http->GetLastError());
        return false;
    }

    const int status = http->GetStatusCode();
    if (status != 200) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "服务器异常 (%d)", status);
        out_err = buf;
        http->Close();
        return false;
    }

    out_body = http->ReadAll();
    http->Close();
    if (out_body.empty()) {
        out_err = "未获取到版本信息";
        return false;
    }
    return true;
}

void doCheck(OtaView* v) {
    if (!WifiStation::GetInstance().IsConnected()) {
        setPhase(v, UiPhase::Error, "Wi-Fi 未连接，请先在设置中配网");
        return;
    }

    const std::string base = remoteBaseUrl();
    if (base.empty()) {
        setPhase(v, UiPhase::Error, "未配置在线升级地址");
        return;
    }

    setPhase(v, UiPhase::Checking, "正在连接云端检查最新版本...");

    std::string body, err;
    if (!httpGetString(base + "/eink4/latest.json", body, err)) {
        setPhase(v, UiPhase::Error, err);
        return;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        setPhase(v, UiPhase::Error, "版本清单解析失败");
        return;
    }

    auto getStr = [&](const char* key) -> std::string {
        cJSON* it = cJSON_GetObjectItemCaseSensitive(root, key);
        return (cJSON_IsString(it) && it->valuestring) ? std::string(it->valuestring) : "";
    };

    const std::string ver = getStr("version");
    const std::string sha = getStr("sha256");
    std::string bin_url = getStr("url");
    if (bin_url.empty()) {
        bin_url = base + "/eink4/firmware.bin";
    }
    size_t size = 0;
    if (cJSON* sz = cJSON_GetObjectItemCaseSensitive(root, "size"); cJSON_IsNumber(sz) && sz->valuedouble > 0) {
        size = static_cast<size_t>(sz->valuedouble);
    }

    std::vector<std::string> apps;
    if (cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "apps"); cJSON_IsArray(arr)) {
        cJSON* elem = nullptr;
        cJSON_ArrayForEach(elem, arr) {
            if (cJSON_IsString(elem) && elem->valuestring && elem->valuestring[0] != '\0') {
                apps.emplace_back(elem->valuestring);
            }
        }
    }
    cJSON_Delete(root);

    if (ver.empty()) {
        setPhase(v, UiPhase::Error, "版本信息缺少版本号");
        return;
    }

    // Compare YYYY.MM.DD-HHMMSS lexicographically against running firmware version
    const std::string curr_ver = ESPDECK_BUILD_VERSION;
    if (ver <= curr_ver) {
        v->has_release.store(false);
        setPhase(v, UiPhase::UpToDate, "当前固件已是最新版本，无需升级");
        return;
    }

    {
        LockGuard g(v->lock);
        v->rel_version = ver;
        v->rel_sha256 = sha;
        v->rel_bin_url = bin_url;
        v->rel_apps = std::move(apps);
        v->rel_size = size;
    }

    v->has_release.store(true);
    setPhase(v, UiPhase::Ready, "");
}

void doUpgrade(OtaView* v) {
    if (!v->has_release.load()) {
        setPhase(v, UiPhase::Error, "请先检查更新");
        return;
    }
    if (!WifiStation::GetInstance().IsConnected()) {
        setPhase(v, UiPhase::Error, "Wi-Fi 已断开，无法下载");
        return;
    }

    std::string bin_url;
    std::string expected_sha;
    size_t expected_size = 0;
    {
        LockGuard g(v->lock);
        bin_url = v->rel_bin_url;
        expected_sha = v->rel_sha256;
        expected_size = v->rel_size;
    }

    const esp_partition_t* update_part = esp_ota_get_next_update_partition(nullptr);
    if (!update_part) {
        setPhase(v, UiPhase::Error, "未找到可用升级分区");
        return;
    }

    v->progress.store(0);
    setPhase(v, UiPhase::Upgrading, "正在连接固件服务器...");

    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        setPhase(v, UiPhase::Error, "网络接口不可用");
        return;
    }
    auto http = network->CreateHttp(0);
    if (!http) {
        setPhase(v, UiPhase::Error, "网络初始化失败");
        return;
    }

    http->SetTimeout(20000);
    if (!http->Open("GET", bin_url)) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "下载连接失败 (0x%x)", http->GetLastError());
        setPhase(v, UiPhase::Error, buf);
        return;
    }

    const int status = http->GetStatusCode();
    if (status != 200) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "下载失败 (%d)", status);
        setPhase(v, UiPhase::Error, buf);
        http->Close();
        return;
    }

    size_t content_len = http->GetBodyLength();
    if (content_len == 0 && expected_size > 0) {
        content_len = expected_size;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        setPhase(v, UiPhase::Error, std::string("初始化失败: ") + esp_err_to_name(err));
        http->Close();
        return;
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    char buf[512];
    size_t total = 0;
    int last_pct = -1;
    bool ok = true;
    setPhase(v, UiPhase::Upgrading, "正在下载并写入固件，请勿断电");

    while (true) {
        const int r = http->Read(buf, sizeof(buf));
        if (r < 0) {
            setPhase(v, UiPhase::Error, "下载中断，已安全取消");
            ok = false;
            break;
        }
        if (r == 0) break;

        err = esp_ota_write(ota_handle, buf, static_cast<size_t>(r));
        if (err != ESP_OK) {
            setPhase(v, UiPhase::Error, std::string("写入失败: ") + esp_err_to_name(err));
            ok = false;
            break;
        }
        mbedtls_sha256_update(&sha_ctx, reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(r));
        total += static_cast<size_t>(r);

        if (content_len > 0) {
            const int pct = static_cast<int>((total * 100ULL) / content_len);
            if (pct == 100 || pct - last_pct >= 10) {
                last_pct = pct;
                v->progress.store(pct);
                v->ui_dirty.store(true);
            }
        }
    }

    http->Close();

    uint8_t digest[32] = {};
    mbedtls_sha256_finish(&sha_ctx, digest);
    mbedtls_sha256_free(&sha_ctx);

    if (!ok || total == 0) {
        esp_ota_abort(ota_handle);
        return;
    }

    setPhase(v, UiPhase::Upgrading, "正在校验固件完整性...");

    if (!expected_sha.empty()) {
        char hex[65] = {};
        for (int i = 0; i < 32; ++i) {
            std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
        }
        std::string exp_low = expected_sha;
        for (auto& ch : exp_low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (exp_low != hex) {
            esp_ota_abort(ota_handle);
            setPhase(v, UiPhase::Error, "SHA-256 校验失败，已取消升级");
            ESP_LOGE(kTag, "SHA256 mismatch! expected=%s got=%s", exp_low.c_str(), hex);
            return;
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        setPhase(v, UiPhase::Error, std::string("镜像校验失败: ") + esp_err_to_name(err));
        return;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        setPhase(v, UiPhase::Error, std::string("切换启动分区失败: ") + esp_err_to_name(err));
        return;
    }

    v->progress.store(100);
    setPhase(v, UiPhase::Done, "2 秒后自动重启进入新系统");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

void workerTask(void* arg) {
    auto* holder = static_cast<std::shared_ptr<OtaView>*>(arg);
    std::shared_ptr<OtaView> v = *holder;
    delete holder;

    ensureWifiUp();

    while (v->alive.load()) {
        const int cmd = v->pending_cmd.exchange(static_cast<int>(Cmd::None));
        if (cmd == static_cast<int>(Cmd::Check)) {
            v->busy.store(true);
            doCheck(v.get());
            v->busy.store(false);
        } else if (cmd == static_cast<int>(Cmd::Upgrade)) {
            v->busy.store(true);
            doUpgrade(v.get());
            v->busy.store(false);
        } else {
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }

    v.reset();
    vTaskDelete(nullptr);
}

// ---------- UI Builders (Editorial E-Ink Style) ----------

lv_obj_t* makeHairline(lv_obj_t* parent, int x, int y, int w, int h = 1) {
    lv_obj_t* line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_size(line, w, h);
    lv_obj_set_style_bg_color(line, ColRule(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    return line;
}

lv_obj_t* makeSpecRow(lv_obj_t* parent, int y, const char* key, const char* val, lv_obj_t** out_val = nullptr) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, 416, 54);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_font(k, FontSmall(), 0);
    lv_obj_set_style_text_color(k, ColMuted(), 0);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* v = lv_label_create(row);
    lv_label_set_text(v, val);
    lv_obj_set_style_text_font(v, FontBody(), 0);
    lv_obj_set_style_text_color(v, ColInk(), 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);

    makeHairline(row, 0, 53, 416, 1);

    if (out_val) *out_val = v;
    return row;
}

void buildEmblem(lv_obj_t* box) {
    // Crisp minimalist monochrome emblem: rounded square + geometric upward arrow + base tray
    static lv_point_precise_t shaft[] = {{34, 18}, {34, 44}};
    static lv_point_precise_t head[] = {{23, 29}, {34, 18}, {45, 29}};
    static lv_point_precise_t tray[] = {{20, 50}, {48, 50}};

    auto addStroke = [&](lv_point_precise_t* pts, uint32_t cnt, int width) {
        lv_obj_t* l = lv_line_create(box);
        lv_line_set_points(l, pts, cnt);
        lv_obj_set_style_line_color(l, ColInk(), 0);
        lv_obj_set_style_line_width(l, width, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
    };
    addStroke(shaft, 2, 3);
    addStroke(head, 3, 3);
    addStroke(tray, 2, 3);
}

void rebuildAppChips(lv_obj_t* wrap, const std::vector<std::string>& apps) {
    lv_obj_clean(wrap);
    if (apps.empty()) {
        lv_obj_t* none = lv_label_create(wrap);
        lv_label_set_text(none, "-");
        lv_obj_set_style_text_font(none, FontSmall(), 0);
        lv_obj_set_style_text_color(none, ColMuted(), 0);
        return;
    }
    for (const auto& name : apps) {
        lv_obj_t* chip = lv_obj_create(wrap);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, 32);
        lv_obj_set_style_pad_hor(chip, 12, 0);
        lv_obj_set_style_pad_ver(chip, 4, 0);
        lv_obj_set_style_bg_color(chip, ColPaper(), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(chip, ColInk(), 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_radius(chip, 16, 0);

        lv_obj_t* lbl = lv_label_create(chip);
        lv_label_set_text(lbl, name.c_str());
        lv_obj_set_style_text_font(lbl, FontSmall(), 0);
        lv_obj_set_style_text_color(lbl, ColInk(), 0);
        lv_obj_center(lbl);
    }
}

void setVisible(lv_obj_t* obj, bool show) {
    if (!obj) return;
    if (show) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void refreshUi(lv_timer_t* t) {
    auto* v = static_cast<OtaView*>(lv_timer_get_user_data(t));
    if (!v || !v->alive.load()) return;

    // 1. Wi-Fi indicator (only mutate on state transition)
    const int wifi_now = WifiStation::GetInstance().IsConnected() ? 1 : 0;
    if (wifi_now != v->last_wifi_state) {
        v->last_wifi_state = wifi_now;
        if (wifi_now == 1) {
            lv_obj_set_style_bg_color(v->wifi_dot, ColInk(), 0);
            lv_obj_set_style_bg_opa(v->wifi_dot, LV_OPA_COVER, 0);
            lv_label_set_text(v->lbl_wifi, "Wi-Fi 已连接");
        } else {
            lv_obj_set_style_bg_color(v->wifi_dot, ColPaper(), 0);
            lv_obj_set_style_bg_opa(v->wifi_dot, LV_OPA_COVER, 0);
            lv_label_set_text(v->lbl_wifi, "正在连接网络");
        }
    }

    if (!v->ui_dirty.exchange(false)) return;

    const auto phase = static_cast<UiPhase>(v->phase.load());
    std::string notice, ver;
    std::vector<std::string> apps;
    size_t sz = 0;
    {
        LockGuard g(v->lock);
        notice = v->notice_text;
        ver = v->rel_version;
        apps = v->rel_apps;
        sz = v->rel_size;
    }

    // 2. Hero copy & badge
    char sub_buf[96];
    switch (phase) {
        case UiPhase::Idle:
            setVisible(v->badge_pill, false);
            lv_label_set_text(v->lbl_hero, "系统升级");
            std::snprintf(sub_buf, sizeof(sub_buf), "当前版本  %s", ESPDECK_BUILD_VERSION);
            lv_label_set_text(v->lbl_sub, sub_buf);
            break;
        case UiPhase::Checking:
            setVisible(v->badge_pill, false);
            lv_label_set_text(v->lbl_hero, "检查更新中");
            lv_label_set_text(v->lbl_sub, "正在同步云端版本清单");
            break;
        case UiPhase::Ready:
            setVisible(v->badge_pill, true);
            lv_label_set_text(v->lbl_badge, "发现新版本");
            lv_label_set_text(v->lbl_hero, ver.c_str());
            std::snprintf(sub_buf, sizeof(sub_buf), "当前版本  %s", ESPDECK_BUILD_VERSION);
            lv_label_set_text(v->lbl_sub, sub_buf);
            break;
        case UiPhase::Upgrading:
            setVisible(v->badge_pill, true);
            lv_label_set_text(v->lbl_badge, "请勿断电");
            lv_label_set_text(v->lbl_hero, "正在升级");
            std::snprintf(sub_buf, sizeof(sub_buf), "目标版本  %s", ver.c_str());
            lv_label_set_text(v->lbl_sub, sub_buf);
            break;
        case UiPhase::Done:
            setVisible(v->badge_pill, true);
            lv_label_set_text(v->lbl_badge, "校验通过");
            lv_label_set_text(v->lbl_hero, "升级完成");
            lv_label_set_text(v->lbl_sub, "即将自动重启设备");
            break;
        case UiPhase::Error:
            setVisible(v->badge_pill, false);
            lv_label_set_text(v->lbl_hero, "系统升级");
            std::snprintf(sub_buf, sizeof(sub_buf), "当前版本  %s", ESPDECK_BUILD_VERSION);
            lv_label_set_text(v->lbl_sub, sub_buf);
            break;
        case UiPhase::UpToDate:
            setVisible(v->badge_pill, true);
            lv_label_set_text(v->lbl_badge, "已是最新");
            lv_label_set_text(v->lbl_hero, ESPDECK_BUILD_VERSION);
            lv_label_set_text(v->lbl_sub, "当前固件已是最新版本");
            break;
    }

    // 3. Middle panel visibility & content
    const bool show_idle = (phase == UiPhase::Idle || phase == UiPhase::Checking ||
                            phase == UiPhase::Error || phase == UiPhase::UpToDate);
    const bool show_rel = (phase == UiPhase::Ready);
    const bool show_prog = (phase == UiPhase::Upgrading || phase == UiPhase::Done);

    setVisible(v->panel_idle, show_idle);
    setVisible(v->panel_release, show_rel);
    setVisible(v->panel_progress, show_prog);

    if (show_rel && v->last_rendered_ver != ver) {
        v->last_rendered_ver = ver;
        char sz_buf[32];
        std::snprintf(sz_buf, sizeof(sz_buf), "%.2f MB", static_cast<double>(sz) / (1024.0 * 1024.0));
        lv_label_set_text(v->lbl_rel_size, sz_buf);
        rebuildAppChips(v->apps_chips_wrap, apps);
    }

    if (show_prog) {
        const int pct = v->progress.load();
        char pbuf[16];
        std::snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
        lv_label_set_text(v->lbl_pct, pbuf);
        lv_bar_set_value(v->bar, pct, LV_ANIM_OFF);
        lv_label_set_text(v->lbl_prog_hint, notice.c_str());
    }

    // 4. Notice line (only shown when non-empty and not in progress panel)
    const bool show_notice = (!show_prog && !notice.empty());
    setVisible(v->lbl_notice, show_notice);
    if (show_notice) {
        lv_label_set_text(v->lbl_notice, notice.c_str());
    }

    // 5. Bottom action dock
    if (phase == UiPhase::Idle || phase == UiPhase::Error) {
        setVisible(v->btn_primary, true);
        lv_obj_set_pos(v->btn_primary, 32, 680);
        lv_label_set_text(v->lbl_primary, "检查更新");
        setVisible(v->btn_secondary, false);
    } else if (phase == UiPhase::UpToDate) {
        setVisible(v->btn_primary, true);
        lv_obj_set_pos(v->btn_primary, 32, 680);
        lv_label_set_text(v->lbl_primary, "重新检查");
        setVisible(v->btn_secondary, false);
    } else if (phase == UiPhase::Ready) {
        setVisible(v->btn_primary, true);
        lv_obj_set_pos(v->btn_primary, 32, 622);
        lv_label_set_text(v->lbl_primary, "立即升级");
        setVisible(v->btn_secondary, true);
        lv_label_set_text(v->lbl_secondary, "重新检查");
    } else {
        // Checking / Upgrading / Done: lock out actions cleanly
        setVisible(v->btn_primary, false);
        setVisible(v->btn_secondary, false);
    }

    v->last_rendered_phase = static_cast<int>(phase);
}

}  // namespace

lv_obj_t* EspdeckOtaScreen::Create() {
    // Full-screen app: detach stale HomeScreen status widgets before resuming Wi-Fi
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }

    PowerPolicy::GetInstance().Acquire(PowerNeed::UiKeepNet);
    PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, ColPaper(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    auto v = std::make_shared<OtaView>();
    v->screen = scr;
    v->lock = xSemaphoreCreateMutex();
    g_ota = v;

    // ── Top Bar (y = 22..72) ─────────────────────────────────────────────────
    v->btn_back = lv_button_create(scr);
    lv_obj_remove_style_all(v->btn_back);
    lv_obj_set_pos(v->btn_back, 24, 22);
    lv_obj_set_size(v->btn_back, 96, 44);
    lv_obj_add_event_cb(v->btn_back, [](lv_event_t*) {
        if (g_ota && (g_ota->busy.load() || g_ota->pending_cmd.load() != 0)) return;
        ScreenRequestHome();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl_back = lv_label_create(v->btn_back);
    lv_label_set_text(lbl_back, "< 返回");
    lv_obj_set_style_text_font(lbl_back, FontBody(), 0);
    lv_obj_set_style_text_color(lbl_back, ColInk(), 0);
    lv_obj_align(lbl_back, LV_ALIGN_LEFT_MID, 8, 0);

    // Right-aligned Wi-Fi pill
    lv_obj_t* wifi_row = lv_obj_create(scr);
    lv_obj_remove_style_all(wifi_row);
    lv_obj_set_size(wifi_row, LV_SIZE_CONTENT, 32);
    lv_obj_align(wifi_row, LV_ALIGN_TOP_RIGHT, -32, 28);
    lv_obj_set_flex_flow(wifi_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wifi_row, 8, 0);

    v->wifi_dot = lv_obj_create(wifi_row);
    lv_obj_remove_style_all(v->wifi_dot);
    lv_obj_set_size(v->wifi_dot, 10, 10);
    lv_obj_set_style_radius(v->wifi_dot, 5, 0);
    lv_obj_set_style_border_color(v->wifi_dot, ColInk(), 0);
    lv_obj_set_style_border_width(v->wifi_dot, 2, 0);
    lv_obj_set_style_bg_color(v->wifi_dot, ColPaper(), 0);
    lv_obj_set_style_bg_opa(v->wifi_dot, LV_OPA_COVER, 0);

    v->lbl_wifi = lv_label_create(wifi_row);
    lv_label_set_text(v->lbl_wifi, "正在连接网络");
    lv_obj_set_style_text_font(v->lbl_wifi, FontSmall(), 0);
    lv_obj_set_style_text_color(v->lbl_wifi, ColMuted(), 0);

    makeHairline(scr, 32, 82, 416, 1);

    // ── Hero Block (y = 114..296) ────────────────────────────────────────────
    v->emblem_box = lv_obj_create(scr);
    lv_obj_remove_style_all(v->emblem_box);
    lv_obj_set_size(v->emblem_box, 68, 68);
    lv_obj_align(v->emblem_box, LV_ALIGN_TOP_MID, 0, 114);
    lv_obj_set_style_bg_color(v->emblem_box, ColPaper(), 0);
    lv_obj_set_style_bg_opa(v->emblem_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(v->emblem_box, ColInk(), 0);
    lv_obj_set_style_border_width(v->emblem_box, 2, 0);
    lv_obj_set_style_radius(v->emblem_box, 18, 0);
    buildEmblem(v->emblem_box);

    v->badge_pill = lv_obj_create(scr);
    lv_obj_remove_style_all(v->badge_pill);
    lv_obj_set_size(v->badge_pill, LV_SIZE_CONTENT, 28);
    lv_obj_align(v->badge_pill, LV_ALIGN_TOP_MID, 0, 198);
    lv_obj_set_style_pad_hor(v->badge_pill, 14, 0);
    lv_obj_set_style_bg_color(v->badge_pill, ColInk(), 0);
    lv_obj_set_style_bg_opa(v->badge_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(v->badge_pill, 14, 0);
    lv_obj_add_flag(v->badge_pill, LV_OBJ_FLAG_HIDDEN);

    v->lbl_badge = lv_label_create(v->badge_pill);
    lv_label_set_text(v->lbl_badge, "发现新版本");
    lv_obj_set_style_text_font(v->lbl_badge, FontSmall(), 0);
    lv_obj_set_style_text_color(v->lbl_badge, ColPaper(), 0);
    lv_obj_center(v->lbl_badge);

    v->lbl_hero = lv_label_create(scr);
    lv_obj_set_width(v->lbl_hero, 416);
    lv_obj_set_style_text_align(v->lbl_hero, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(v->lbl_hero, FontHero(), 0);
    lv_obj_set_style_text_color(v->lbl_hero, ColInk(), 0);
    lv_obj_align(v->lbl_hero, LV_ALIGN_TOP_MID, 0, 236);

    v->lbl_sub = lv_label_create(scr);
    lv_obj_set_width(v->lbl_sub, 416);
    lv_obj_set_style_text_align(v->lbl_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(v->lbl_sub, FontSmall(), 0);
    lv_obj_set_style_text_color(v->lbl_sub, ColMuted(), 0);
    lv_obj_align(v->lbl_sub, LV_ALIGN_TOP_MID, 0, 280);

    // ── Middle Panel A: Idle Spec List (y = 340..510) ────────────────────────
    v->panel_idle = lv_obj_create(scr);
    lv_obj_remove_style_all(v->panel_idle);
    lv_obj_set_pos(v->panel_idle, 32, 340);
    lv_obj_set_size(v->panel_idle, 416, 170);
    lv_obj_remove_flag(v->panel_idle, LV_OBJ_FLAG_SCROLLABLE);

    makeHairline(v->panel_idle, 0, 0, 416, 1);
    makeSpecRow(v->panel_idle, 1,   "设备", "Metalio E-INK4");
    makeSpecRow(v->panel_idle, 55,  "方式", "HTTPS 在线升级");
    makeSpecRow(v->panel_idle, 109, "保护", "SHA-256 校验 · 双分区回滚");

    // ── Middle Panel B: Release Spec + App Chips (y = 340..540) ──────────────
    v->panel_release = lv_obj_create(scr);
    lv_obj_remove_style_all(v->panel_release);
    lv_obj_set_pos(v->panel_release, 32, 340);
    lv_obj_set_size(v->panel_release, 416, 200);
    lv_obj_remove_flag(v->panel_release, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(v->panel_release, LV_OBJ_FLAG_HIDDEN);

    makeHairline(v->panel_release, 0, 0, 416, 1);
    makeSpecRow(v->panel_release, 1, "固件大小", "-", &v->lbl_rel_size);

    lv_obj_t* apps_title = lv_label_create(v->panel_release);
    lv_label_set_text(apps_title, "包含应用");
    lv_obj_set_style_text_font(apps_title, FontSmall(), 0);
    lv_obj_set_style_text_color(apps_title, ColMuted(), 0);
    lv_obj_set_pos(apps_title, 0, 74);

    v->apps_chips_wrap = lv_obj_create(v->panel_release);
    lv_obj_remove_style_all(v->apps_chips_wrap);
    lv_obj_set_pos(v->apps_chips_wrap, 0, 108);
    lv_obj_set_size(v->apps_chips_wrap, 416, 86);
    lv_obj_set_flex_flow(v->apps_chips_wrap, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(v->apps_chips_wrap, 10, 0);
    lv_obj_set_style_pad_row(v->apps_chips_wrap, 10, 0);

    // ── Middle Panel C: Focused Progress Module (y = 360..520) ───────────────
    v->panel_progress = lv_obj_create(scr);
    lv_obj_remove_style_all(v->panel_progress);
    lv_obj_set_pos(v->panel_progress, 32, 360);
    lv_obj_set_size(v->panel_progress, 416, 160);
    lv_obj_remove_flag(v->panel_progress, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(v->panel_progress, LV_OBJ_FLAG_HIDDEN);

    v->lbl_pct = lv_label_create(v->panel_progress);
    lv_label_set_text(v->lbl_pct, "0%");
    lv_obj_set_style_text_font(v->lbl_pct, FontHero(), 0);
    lv_obj_set_style_text_color(v->lbl_pct, ColInk(), 0);
    lv_obj_align(v->lbl_pct, LV_ALIGN_TOP_MID, 0, 0);

    v->bar = lv_bar_create(v->panel_progress);
    lv_obj_set_size(v->bar, 416, 12);
    lv_obj_align(v->bar, LV_ALIGN_TOP_MID, 0, 56);
    lv_bar_set_range(v->bar, 0, 100);
    lv_obj_set_style_radius(v->bar, 6, 0);
    lv_obj_set_style_radius(v->bar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(v->bar, ColInk(), 0);
    lv_obj_set_style_border_width(v->bar, 2, 0);
    lv_obj_set_style_bg_color(v->bar, ColPaper(), 0);
    lv_obj_set_style_bg_color(v->bar, ColInk(), LV_PART_INDICATOR);

    v->lbl_prog_hint = lv_label_create(v->panel_progress);
    lv_obj_set_width(v->lbl_prog_hint, 416);
    lv_obj_set_style_text_align(v->lbl_prog_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(v->lbl_prog_hint, FontSmall(), 0);
    lv_obj_set_style_text_color(v->lbl_prog_hint, ColMuted(), 0);
    lv_obj_align(v->lbl_prog_hint, LV_ALIGN_TOP_MID, 0, 92);

    // ── Notice Line (y = 556) ────────────────────────────────────────────────
    v->lbl_notice = lv_label_create(scr);
    lv_obj_set_width(v->lbl_notice, 416);
    lv_obj_set_style_text_align(v->lbl_notice, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(v->lbl_notice, FontSmall(), 0);
    lv_obj_set_style_text_color(v->lbl_notice, ColInk(), 0);
    lv_obj_align(v->lbl_notice, LV_ALIGN_TOP_MID, 0, 556);
    lv_obj_add_flag(v->lbl_notice, LV_OBJ_FLAG_HIDDEN);

    // ── Bottom Action Dock ───────────────────────────────────────────────────
    // Primary solid-black CTA button
    v->btn_primary = lv_button_create(scr);
    lv_obj_set_size(v->btn_primary, 416, 64);
    lv_obj_set_pos(v->btn_primary, 32, 680);
    lv_obj_set_style_bg_color(v->btn_primary, ColInk(), 0);
    lv_obj_set_style_bg_opa(v->btn_primary, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(v->btn_primary, 0, 0);
    lv_obj_set_style_radius(v->btn_primary, 16, 0);
    lv_obj_set_style_shadow_width(v->btn_primary, 0, 0);
    lv_obj_add_event_cb(v->btn_primary, [](lv_event_t*) {
        if (!g_ota || g_ota->busy.load() || g_ota->pending_cmd.load() != 0) return;
        const auto p = static_cast<UiPhase>(g_ota->phase.load());
        if (p == UiPhase::Ready) {
            setPhase(g_ota.get(), UiPhase::Upgrading, "正在准备下载...");
            g_ota->pending_cmd.store(static_cast<int>(Cmd::Upgrade));
        } else {
            setPhase(g_ota.get(), UiPhase::Checking, "正在连接云端检查最新版本...");
            g_ota->pending_cmd.store(static_cast<int>(Cmd::Check));
        }
    }, LV_EVENT_CLICKED, nullptr);

    v->lbl_primary = lv_label_create(v->btn_primary);
    lv_label_set_text(v->lbl_primary, "检查更新");
    lv_obj_set_style_text_font(v->lbl_primary, FontBodyBold(), 0);
    lv_obj_set_style_text_color(v->lbl_primary, ColPaper(), 0);
    lv_obj_center(v->lbl_primary);

    // Secondary crisp outline button (shown only when a release is ready)
    v->btn_secondary = lv_button_create(scr);
    lv_obj_set_size(v->btn_secondary, 416, 52);
    lv_obj_set_pos(v->btn_secondary, 32, 704);
    lv_obj_set_style_bg_color(v->btn_secondary, ColPaper(), 0);
    lv_obj_set_style_bg_opa(v->btn_secondary, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(v->btn_secondary, ColInk(), 0);
    lv_obj_set_style_border_width(v->btn_secondary, 1, 0);
    lv_obj_set_style_radius(v->btn_secondary, 14, 0);
    lv_obj_set_style_shadow_width(v->btn_secondary, 0, 0);
    lv_obj_add_flag(v->btn_secondary, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(v->btn_secondary, [](lv_event_t*) {
        if (!g_ota || g_ota->busy.load() || g_ota->pending_cmd.load() != 0) return;
        g_ota->has_release.store(false);
        setPhase(g_ota.get(), UiPhase::Checking, "正在重新检查...");
        g_ota->pending_cmd.store(static_cast<int>(Cmd::Check));
    }, LV_EVENT_CLICKED, nullptr);

    v->lbl_secondary = lv_label_create(v->btn_secondary);
    lv_label_set_text(v->lbl_secondary, "重新检查");
    lv_obj_set_style_text_font(v->lbl_secondary, FontSmall(), 0);
    lv_obj_set_style_text_color(v->lbl_secondary, ColInk(), 0);
    lv_obj_center(v->lbl_secondary);

    // ── Background Worker + UI Timer ─────────────────────────────────────────
    auto* holder = new std::shared_ptr<OtaView>(v);
    if (xTaskCreate(workerTask, "espdeck_ota", 12288, holder, 4, nullptr) != pdPASS) {
        delete holder;
        setPhase(v.get(), UiPhase::Error, "后台任务创建失败");
        ESP_LOGE(kTag, "xTaskCreate(espdeck_ota) failed");
    }

    v->ui_timer = lv_timer_create(refreshUi, 800, v.get());
    refreshUi(v->ui_timer);

    VkKeyScreenDesc desc{};
    desc.factory = Create;
    VkKey_AttachScreen(scr, "espdeck_ota", desc);

    auto* del_holder = new std::shared_ptr<OtaView>(v);
    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        auto* h = static_cast<std::shared_ptr<OtaView>*>(lv_event_get_user_data(e));
        std::shared_ptr<OtaView> view = *h;
        delete h;
        view->alive.store(false);
        if (view->ui_timer) {
            lv_timer_delete(view->ui_timer);
            view->ui_timer = nullptr;
        }
        if (g_ota == view) g_ota.reset();
        PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
        PowerPolicy::GetInstance().Release(PowerNeed::UiKeepNet);
    }, LV_EVENT_DELETE, del_holder);

    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestNextFullRefresh();
    }
    return scr;
}
