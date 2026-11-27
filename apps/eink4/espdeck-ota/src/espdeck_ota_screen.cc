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

namespace {

constexpr const char* kTag = "EspdeckOTA";

enum class Cmd : int {
    None = 0,
    Check = 1,
    Upgrade = 2,
};

struct OtaView {
    lv_obj_t* screen = nullptr;
    lv_obj_t* lbl_wifi = nullptr;
    lv_obj_t* lbl_curr = nullptr;
    lv_obj_t* lbl_info = nullptr;
    lv_obj_t* lbl_status = nullptr;
    lv_obj_t* bar = nullptr;
    lv_obj_t* lbl_pct = nullptr;
    lv_obj_t* btn_upgrade = nullptr;
    lv_timer_t* ui_timer = nullptr;

    std::atomic<bool> alive{true};
    std::atomic<int> pending_cmd{static_cast<int>(Cmd::None)};
    std::atomic<bool> busy{false};
    std::atomic<bool> has_release{false};
    std::atomic<int> progress{0};
    std::atomic<bool> ui_dirty{true};

    SemaphoreHandle_t lock = nullptr;
    std::string rel_version;
    std::string rel_sha256;
    std::string rel_bin_url;
    std::string rel_apps;
    size_t rel_size = 0;
    std::string status_text = "准备就绪";
    std::string last_wifi_text;

    ~OtaView() {
        if (lock) {
            vSemaphoreDelete(lock);
            lock = nullptr;
        }
    }
};

std::shared_ptr<OtaView> g_ota;

const lv_font_t* UiFont() {
    const lv_font_t* f = fontpack_lv_font_ui();
    return f ? f : LV_FONT_DEFAULT;
}

struct LockGuard {
    SemaphoreHandle_t h;
    explicit LockGuard(SemaphoreHandle_t s) : h(s) {
        if (h) xSemaphoreTake(h, portMAX_DELAY);
    }
    ~LockGuard() {
        if (h) xSemaphoreGive(h);
    }
};

void setStatus(OtaView* v, const std::string& msg) {
    {
        LockGuard g(v->lock);
        v->status_text = msg;
    }
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
        std::snprintf(buf, sizeof(buf), "连接服务器失败 (0x%x)", http->GetLastError());
        out_err = buf;
        ESP_LOGE(kTag, "GET %s failed: 0x%x", url.c_str(), http->GetLastError());
        return false;
    }

    const int status = http->GetStatusCode();
    if (status != 200) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "服务器返回异常 (%d)", status);
        out_err = buf;
        http->Close();
        return false;
    }

    out_body = http->ReadAll();
    http->Close();
    if (out_body.empty()) {
        out_err = "版本信息为空";
        return false;
    }
    return true;
}

void doCheck(OtaView* v) {
    if (!WifiStation::GetInstance().IsConnected()) {
        setStatus(v, "Wi-Fi 未连接，请先在设置中配网");
        return;
    }

    const std::string base = remoteBaseUrl();
    if (base.empty()) {
        setStatus(v, "未配置在线升级地址");
        return;
    }

    const std::string manifest_url = base + "/eink4/latest.json";
    ESP_LOGI(kTag, "Checking OTA manifest: %s", manifest_url.c_str());
    setStatus(v, "正在检查更新...");

    std::string body, err;
    if (!httpGetString(manifest_url, body, err)) {
        setStatus(v, err);
        return;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        setStatus(v, "版本信息解析失败");
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

    std::string apps_str;
    if (cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "apps"); cJSON_IsArray(arr)) {
        cJSON* elem = nullptr;
        cJSON_ArrayForEach(elem, arr) {
            if (cJSON_IsString(elem) && elem->valuestring) {
                if (!apps_str.empty()) apps_str += ", ";
                apps_str += elem->valuestring;
            }
        }
    }
    cJSON_Delete(root);

    if (ver.empty()) {
        setStatus(v, "版本信息不完整");
        return;
    }

    {
        LockGuard g(v->lock);
        v->rel_version = ver;
        v->rel_sha256 = sha;
        v->rel_bin_url = bin_url;
        v->rel_apps = apps_str;
        v->rel_size = size;
    }

    v->has_release.store(true);
    setStatus(v, "发现可用更新，点击「立即升级」");
}

void doUpgrade(OtaView* v) {
    if (!v->has_release.load()) {
        setStatus(v, "请先点击「检查更新」");
        return;
    }
    if (!WifiStation::GetInstance().IsConnected()) {
        setStatus(v, "Wi-Fi 已断开");
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
        setStatus(v, "未找到可用升级分区");
        return;
    }

    v->progress.store(0);
    setStatus(v, "正在连接服务器...");

    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        setStatus(v, "网络接口不可用");
        return;
    }
    auto http = network->CreateHttp(0);
    if (!http) {
        setStatus(v, "网络初始化失败");
        return;
    }

    http->SetTimeout(20000);
    if (!http->Open("GET", bin_url)) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "下载连接失败 (0x%x)", http->GetLastError());
        setStatus(v, buf);
        return;
    }

    const int status = http->GetStatusCode();
    if (status != 200) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "下载失败 (%d)", status);
        setStatus(v, buf);
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
        setStatus(v, std::string("升级初始化失败: ") + esp_err_to_name(err));
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
    setStatus(v, "正在下载并写入固件...");

    while (true) {
        const int r = http->Read(buf, sizeof(buf));
        if (r < 0) {
            setStatus(v, "下载中断，已取消升级");
            ok = false;
            break;
        }
        if (r == 0) break;

        err = esp_ota_write(ota_handle, buf, static_cast<size_t>(r));
        if (err != ESP_OK) {
            setStatus(v, std::string("写入失败: ") + esp_err_to_name(err));
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

    if (!expected_sha.empty()) {
        char hex[65] = {};
        for (int i = 0; i < 32; ++i) {
            std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
        }
        std::string exp_low = expected_sha;
        for (auto& ch : exp_low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (exp_low != hex) {
            esp_ota_abort(ota_handle);
            setStatus(v, "固件校验失败，已取消升级");
            ESP_LOGE(kTag, "SHA256 mismatch! expected=%s got=%s", exp_low.c_str(), hex);
            return;
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        setStatus(v, std::string("镜像校验失败: ") + esp_err_to_name(err));
        return;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        setStatus(v, std::string("切换分区失败: ") + esp_err_to_name(err));
        return;
    }

    v->progress.store(100);
    setStatus(v, "升级成功，2 秒后自动重启...");
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

void refreshUi(lv_timer_t* t) {
    auto* v = static_cast<OtaView*>(lv_timer_get_user_data(t));
    if (!v || !v->alive.load()) return;

    const char* wifi_txt = WifiStation::GetInstance().IsConnected()
                               ? "Wi-Fi: 已连接"
                               : "Wi-Fi: 正在连接...";
    if (v->last_wifi_text != wifi_txt) {
        v->last_wifi_text = wifi_txt;
        lv_label_set_text(v->lbl_wifi, wifi_txt);
    }

    if (!v->ui_dirty.exchange(false)) return;

    std::string status, ver, apps;
    size_t sz = 0;
    {
        LockGuard g(v->lock);
        status = v->status_text;
        ver = v->rel_version;
        apps = v->rel_apps;
        sz = v->rel_size;
    }

    if (v->has_release.load()) {
        char info[256];
        std::snprintf(info, sizeof(info), "最新版本: %s\n固件大小: %.2f MB\n包含应用: %s",
                      ver.c_str(), static_cast<double>(sz) / (1024.0 * 1024.0),
                      apps.empty() ? "-" : apps.c_str());
        lv_label_set_text(v->lbl_info, info);
    } else {
        lv_label_set_text(v->lbl_info, "点击「检查更新」获取最新固件");
    }

    lv_label_set_text(v->lbl_status, status.c_str());

    const int pct = v->progress.load();
    lv_bar_set_value(v->bar, pct, LV_ANIM_OFF);
    char pbuf[16];
    std::snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
    lv_label_set_text(v->lbl_pct, pbuf);
}

lv_obj_t* makeBtn(lv_obj_t* parent, const char* text, int w, int h, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    return btn;
}

}  // namespace

lv_obj_t* EspdeckOtaScreen::Create() {
    // Full-screen app without official top status bar: detach stale HomeScreen
    // status/notification widget pointers BEFORE resuming Wi-Fi so WifiBoard's
    // OnConnect/OnConnected -> ShowNotification() never dereferences freed labels.
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }

    PowerPolicy::GetInstance().Acquire(PowerNeed::UiKeepNet);
    PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    auto v = std::make_shared<OtaView>();
    v->screen = scr;
    v->lock = xSemaphoreCreateMutex();
    g_ota = v;

    // Header
    lv_obj_t* btn_back = makeBtn(scr, "< 返回", 104, 48, [](lv_event_t*) {
        if (g_ota && (g_ota->busy.load() || g_ota->pending_cmd.load() != 0)) return;
        ScreenRequestHome();
    });
    lv_obj_set_pos(btn_back, 24, 24);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "系统升级");
    lv_obj_set_style_text_font(title, UiFont(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 152, 34);

    // Device status card
    lv_obj_t* card1 = lv_obj_create(scr);
    lv_obj_set_size(card1, 432, 124);
    lv_obj_set_pos(card1, 24, 108);
    lv_obj_set_style_bg_color(card1, lv_color_white(), 0);
    lv_obj_set_style_border_color(card1, lv_color_black(), 0);
    lv_obj_set_style_border_width(card1, 2, 0);
    lv_obj_set_style_radius(card1, 14, 0);
    lv_obj_set_style_pad_all(card1, 20, 0);
    lv_obj_remove_flag(card1, LV_OBJ_FLAG_SCROLLABLE);

    v->lbl_wifi = lv_label_create(card1);
    lv_obj_set_style_text_font(v->lbl_wifi, UiFont(), 0);
    lv_obj_set_style_text_color(v->lbl_wifi, lv_color_black(), 0);
    lv_obj_set_pos(v->lbl_wifi, 0, 0);

    v->lbl_curr = lv_label_create(card1);
    char cbuf[64];
    std::snprintf(cbuf, sizeof(cbuf), "当前版本: %s", ESPDECK_BUILD_VERSION);
    lv_label_set_text(v->lbl_curr, cbuf);
    lv_obj_set_style_text_font(v->lbl_curr, UiFont(), 0);
    lv_obj_set_style_text_color(v->lbl_curr, lv_color_black(), 0);
    lv_obj_set_pos(v->lbl_curr, 0, 44);

    // Cloud release card
    lv_obj_t* card2 = lv_obj_create(scr);
    lv_obj_set_size(card2, 432, 200);
    lv_obj_set_pos(card2, 24, 260);
    lv_obj_set_style_bg_color(card2, lv_color_white(), 0);
    lv_obj_set_style_border_color(card2, lv_color_black(), 0);
    lv_obj_set_style_border_width(card2, 2, 0);
    lv_obj_set_style_radius(card2, 14, 0);
    lv_obj_set_style_pad_all(card2, 20, 0);
    lv_obj_remove_flag(card2, LV_OBJ_FLAG_SCROLLABLE);

    v->lbl_info = lv_label_create(card2);
    lv_obj_set_width(v->lbl_info, 388);
    lv_obj_set_style_text_font(v->lbl_info, UiFont(), 0);
    lv_obj_set_style_text_color(v->lbl_info, lv_color_black(), 0);
    lv_obj_set_style_text_line_space(v->lbl_info, 12, 0);
    lv_obj_set_pos(v->lbl_info, 0, 0);

    // Progress bar + status
    v->bar = lv_bar_create(scr);
    lv_obj_set_size(v->bar, 360, 26);
    lv_obj_set_pos(v->bar, 24, 512);
    lv_bar_set_range(v->bar, 0, 100);
    lv_obj_set_style_border_color(v->bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(v->bar, 2, 0);
    lv_obj_set_style_bg_color(v->bar, lv_color_white(), 0);
    lv_obj_set_style_bg_color(v->bar, lv_color_black(), LV_PART_INDICATOR);

    v->lbl_pct = lv_label_create(scr);
    lv_label_set_text(v->lbl_pct, "0%");
    lv_obj_set_style_text_font(v->lbl_pct, UiFont(), 0);
    lv_obj_set_style_text_color(v->lbl_pct, lv_color_black(), 0);
    lv_obj_set_pos(v->lbl_pct, 396, 512);

    v->lbl_status = lv_label_create(scr);
    lv_obj_set_width(v->lbl_status, 432);
    lv_obj_set_style_text_font(v->lbl_status, UiFont(), 0);
    lv_obj_set_style_text_color(v->lbl_status, lv_color_black(), 0);
    lv_obj_set_pos(v->lbl_status, 24, 562);

    // Action buttons (idempotent)
    lv_obj_t* btn_check = makeBtn(scr, "检查更新", 206, 68, [](lv_event_t*) {
        if (!g_ota || g_ota->busy.load() || g_ota->pending_cmd.load() != 0) return;
        setStatus(g_ota.get(), "正在检查更新...");
        g_ota->pending_cmd.store(static_cast<int>(Cmd::Check));
    });
    lv_obj_set_pos(btn_check, 24, 672);

    v->btn_upgrade = makeBtn(scr, "立即升级", 206, 68, [](lv_event_t*) {
        if (!g_ota || g_ota->busy.load() || g_ota->pending_cmd.load() != 0) return;
        setStatus(g_ota.get(), "准备下载固件...");
        g_ota->pending_cmd.store(static_cast<int>(Cmd::Upgrade));
    });
    lv_obj_set_pos(v->btn_upgrade, 250, 672);

    auto* holder = new std::shared_ptr<OtaView>(v);
    if (xTaskCreate(workerTask, "espdeck_ota", 12288, holder, 4, nullptr) != pdPASS) {
        delete holder;
        setStatus(v.get(), "后台任务创建失败");
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
