#include "espdeck_ota_screen.h"
#include "espdeck_ota_config.h"

#include "fontpack_lvgl.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include "screen_common.h"
#include "vk_key_handler.h"
#include <wifi_station.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr const char* kTag = "EspdeckOTA";

enum class SourceMode : int {
    Lan = 0,
    Remote = 1,
};

enum class Cmd : int {
    None = 0,
    Check = 1,
    Upgrade = 2,
};

struct OtaView {
    lv_obj_t* screen = nullptr;
    lv_obj_t* lbl_wifi = nullptr;
    lv_obj_t* lbl_curr = nullptr;
    lv_obj_t* lbl_mode = nullptr;
    lv_obj_t* lbl_url = nullptr;
    lv_obj_t* lbl_info = nullptr;
    lv_obj_t* lbl_status = nullptr;
    lv_obj_t* bar = nullptr;
    lv_obj_t* lbl_pct = nullptr;
    lv_obj_t* btn_upgrade = nullptr;
    lv_timer_t* ui_timer = nullptr;

    std::atomic<bool> alive{true};
    std::atomic<int> mode{
        (ESPDECK_OTA_REMOTE_URL[0] != '\0') ? static_cast<int>(SourceMode::Remote)
                                            : static_cast<int>(SourceMode::Lan)};
    std::atomic<int> pending_cmd{static_cast<int>(Cmd::None)};
    std::atomic<bool> busy{false};
    std::atomic<bool> has_release{false};
    std::atomic<int> progress{0};
    std::atomic<bool> ui_dirty{true};

    SemaphoreHandle_t lock = nullptr;
    std::string lan_url = ESPDECK_OTA_LAN_URL;
    std::string remote_url = ESPDECK_OTA_REMOTE_URL;
    std::string rel_version;
    std::string rel_sha256;
    std::string rel_bin_url;
    std::string rel_apps;
    std::string rel_notes;
    size_t rel_size = 0;
    std::string status_text = "等待检查更新";

    TaskHandle_t worker = nullptr;
};

OtaView* g_ota = nullptr;

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

std::string activeBaseUrl(OtaView* v) {
    const bool is_lan = (v->mode.load() == static_cast<int>(SourceMode::Lan));
    std::string url;
    {
        LockGuard g(v->lock);
        url = is_lan ? v->lan_url : v->remote_url;
    }
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
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 8000;
    cfg.buffer_size = 2048;
    if (url.rfind("https://", 0) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        out_err = "HTTP 初始化失败";
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        out_err = std::string("连接失败: ") + esp_err_to_name(err);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "HTTP 状态码 %d", status);
        out_err = buf;
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    out_body.clear();
    char chunk[512];
    while (true) {
        const int r = esp_http_client_read(client, chunk, sizeof(chunk));
        if (r < 0) {
            out_err = "读取 manifest 失败";
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        if (r == 0) break;
        out_body.append(chunk, static_cast<size_t>(r));
        if (out_body.size() > 16384) break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return true;
}

void doCheck(OtaView* v) {
    if (!WifiStation::GetInstance().IsConnected()) {
        setStatus(v, "Wi-Fi 未连接，请先在设置中配网");
        return;
    }

    const std::string base = activeBaseUrl(v);
    if (base.empty()) {
        setStatus(v, v->mode.load() == static_cast<int>(SourceMode::Lan)
                         ? "未发现局域网服务器，请运行 espdeck ota serve"
                         : "尚未配置公网 OTA 地址");
        return;
    }

    const std::string manifest_url = base + "/eink4/latest.json";
    setStatus(v, "正在检查: " + manifest_url);

    std::string body, err;
    if (!httpGetString(manifest_url, body, err)) {
        setStatus(v, err);
        return;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        setStatus(v, "manifest JSON 解析失败");
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
    const std::string notes = getStr("notes");
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
        setStatus(v, "manifest 缺少 version 字段");
        return;
    }

    {
        LockGuard g(v->lock);
        v->rel_version = ver;
        v->rel_sha256 = sha;
        v->rel_bin_url = bin_url;
        v->rel_apps = apps_str;
        v->rel_notes = notes;
        v->rel_size = size;
    }

    v->has_release.store(true);
    setStatus(v, "发现可用固件，点击「立即升级」开始");
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
        setStatus(v, "未找到可用 OTA 分区");
        return;
    }

    v->progress.store(0);
    setStatus(v, "正在连接固件服务器...");

    esp_http_client_config_t cfg = {};
    cfg.url = bin_url.c_str();
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 4096;
    if (bin_url.rfind("https://", 0) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        setStatus(v, "HTTP 客户端创建失败");
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        setStatus(v, std::string("下载连接失败: ") + esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "下载失败 HTTP %d", status);
        setStatus(v, buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }
    if (content_len <= 0 && expected_size > 0) {
        content_len = static_cast<int64_t>(expected_size);
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        setStatus(v, std::string("OTA 初始化失败: ") + esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    uint8_t* buf = static_cast<uint8_t*>(malloc(4096));
    if (!buf) {
        esp_ota_abort(ota_handle);
        mbedtls_sha256_free(&sha_ctx);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        setStatus(v, "内存不足");
        return;
    }

    size_t total = 0;
    int last_pct = -1;
    bool ok = true;
    setStatus(v, "正在下载并写入新固件...");

    while (true) {
        const int r = esp_http_client_read(client, reinterpret_cast<char*>(buf), 4096);
        if (r < 0) {
            setStatus(v, "下载中断，已安全回退");
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
        mbedtls_sha256_update(&sha_ctx, buf, static_cast<size_t>(r));
        total += static_cast<size_t>(r);

        if (content_len > 0) {
            const int pct = static_cast<int>((total * 100) / static_cast<size_t>(content_len));
            if (pct == 100 || pct - last_pct >= 5) {
                last_pct = pct;
                v->progress.store(pct);
                v->ui_dirty.store(true);
            }
        }
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

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
            setStatus(v, "SHA-256 校验失败，已拒绝刷入");
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
        setStatus(v, std::string("切换启动分区失败: ") + esp_err_to_name(err));
        return;
    }

    v->progress.store(100);
    setStatus(v, "升级成功！2 秒后自动重启...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

void workerTask(void* arg) {
    auto* v = static_cast<OtaView*>(arg);

    // Non-blocking UDP socket on ESPDECK_OTA_UDP_PORT for zero-config LAN server discovery
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0) {
        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        struct timeval tv = {};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(ESPDECK_OTA_UDP_PORT);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(sock);
            sock = -1;
        }
    }

    while (v->alive.load()) {
        // Poll UDP discovery beacon when in LAN mode and idle
        if (sock >= 0 && !v->busy.load() && v->mode.load() == static_cast<int>(SourceMode::Lan)) {
            char rx[192] = {};
            const int n = recvfrom(sock, rx, sizeof(rx) - 1, 0, nullptr, nullptr);
            if (n > 0) {
                rx[n] = '\0';
                // Expected format: ESPDECK_OTA|eink4|http://192.168.x.x:8787
                const std::string pkt(rx);
                const std::string prefix = "ESPDECK_OTA|eink4|";
                if (pkt.rfind(prefix, 0) == 0) {
                    std::string discovered = pkt.substr(prefix.size());
                    while (!discovered.empty() && (discovered.back() == '\r' || discovered.back() == '\n' || discovered.back() == ' ')) {
                        discovered.pop_back();
                    }
                    bool changed = false;
                    {
                        LockGuard g(v->lock);
                        if (!discovered.empty() && v->lan_url != discovered) {
                            v->lan_url = discovered;
                            changed = true;
                        }
                    }
                    if (changed) {
                        ESP_LOGI(kTag, "Discovered LAN OTA server: %s", discovered.c_str());
                        setStatus(v, "已自动发现局域网 OTA 服务");
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(150));
        }

        const int cmd = v->pending_cmd.exchange(static_cast<int>(Cmd::None));
        if (cmd == static_cast<int>(Cmd::Check)) {
            v->busy.store(true);
            doCheck(v);
            v->busy.store(false);
        } else if (cmd == static_cast<int>(Cmd::Upgrade)) {
            v->busy.store(true);
            doUpgrade(v);
            v->busy.store(false);
        }
    }

    if (sock >= 0) {
        close(sock);
    }
    v->worker = nullptr;
    vTaskDelete(nullptr);
}

void refreshUi(lv_timer_t* t) {
    auto* v = static_cast<OtaView*>(lv_timer_get_user_data(t));
    if (!v || !v->alive.load()) return;

    auto& wifi = WifiStation::GetInstance();
    char wifi_buf[128];
    if (wifi.IsConnected()) {
        std::snprintf(wifi_buf, sizeof(wifi_buf), "Wi-Fi: 已连接 (%s)", wifi.GetIpAddress().c_str());
    } else {
        std::snprintf(wifi_buf, sizeof(wifi_buf), "Wi-Fi: 正在连接...");
    }
    lv_label_set_text(v->lbl_wifi, wifi_buf);

    if (!v->ui_dirty.exchange(false)) return;

    const bool is_lan = (v->mode.load() == static_cast<int>(SourceMode::Lan));
    lv_label_set_text(v->lbl_mode, is_lan ? "升级源: 局域网 (自动发现)" : "升级源: 公网 HTTPS");

    const std::string base = activeBaseUrl(v);
    lv_label_set_text(v->lbl_url, base.empty() ? "地址: 未发现 / 未配置" : ("地址: " + base).c_str());

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
        std::snprintf(info, sizeof(info), "新版本: %s\n大小: %.2f MB\n包含应用: %s",
                      ver.c_str(), static_cast<double>(sz) / (1024.0 * 1024.0),
                      apps.empty() ? "-" : apps.c_str());
        lv_label_set_text(v->lbl_info, info);
    } else {
        lv_label_set_text(v->lbl_info, "尚未获取新版本信息\n点击「检查更新」拉取 latest.json");
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
    PowerPolicy::GetInstance().Acquire(PowerNeed::UiKeepNet);
    PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
    ensureWifiUp();

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    auto* v = new OtaView{};
    v->screen = scr;
    v->lock = xSemaphoreCreateMutex();
    g_ota = v;

    // Header
    lv_obj_t* btn_back = makeBtn(scr, "< 返回", 104, 48, [](lv_event_t*) {
        if (g_ota && g_ota->busy.load()) return;
        ScreenRequestHome();
    });
    lv_obj_set_pos(btn_back, 24, 24);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "ESPDeck 无线升级");
    lv_obj_set_style_text_font(title, UiFont(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 148, 34);

    // Current device card
    lv_obj_t* card1 = lv_obj_create(scr);
    lv_obj_set_size(card1, 432, 172);
    lv_obj_set_pos(card1, 24, 96);
    lv_obj_set_style_bg_color(card1, lv_color_white(), 0);
    lv_obj_set_style_border_color(card1, lv_color_black(), 0);
    lv_obj_set_style_border_width(card1, 2, 0);
    lv_obj_set_style_radius(card1, 14, 0);
    lv_obj_set_style_pad_all(card1, 16, 0);
    lv_obj_remove_flag(card1, LV_OBJ_FLAG_SCROLLABLE);

    v->lbl_wifi = lv_label_create(card1);
    lv_obj_set_style_text_font(v->lbl_wifi, UiFont(), 0);
    lv_obj_set_style_text_color(v->lbl_wifi, lv_color_black(), 0);
    lv_obj_set_pos(v->lbl_wifi, 0, 0);

    v->lbl_curr = lv_label_create(card1);
    const esp_partition_t* running = esp_ota_get_running_partition();
    char cbuf[128];
    std::snprintf(cbuf, sizeof(cbuf), "当前版本: %s (%s)", ESPDECK_BUILD_VERSION, running ? running->label : "?");
    lv_label_set_text(v->lbl_curr, cbuf);
    lv_obj_set_style_text_font(v->lbl_curr, UiFont(), 0);
    lv_obj_set_style_text_color(v->lbl_curr, lv_color_black(), 0);
    lv_obj_set_pos(v->lbl_curr, 0, 36);

    v->lbl_mode = lv_label_create(card1);
    lv_obj_set_style_text_font(v->lbl_mode, UiFont(), 0);
    lv_obj_set_style_text_color(v->lbl_mode, lv_color_black(), 0);
    lv_obj_set_pos(v->lbl_mode, 0, 74);

    v->lbl_url = lv_label_create(card1);
    lv_obj_set_width(v->lbl_url, 396);
    lv_label_set_long_mode(v->lbl_url, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(v->lbl_url, UiFont(), 0);
    lv_obj_set_style_text_color(v->lbl_url, lv_color_black(), 0);
    lv_obj_set_pos(v->lbl_url, 0, 110);

    // Source toggle button
    lv_obj_t* btn_mode = makeBtn(scr, "切换升级源 (局域网 / 公网)", 432, 56, [](lv_event_t*) {
        if (!g_ota || g_ota->busy.load()) return;
        const int next = (g_ota->mode.load() == static_cast<int>(SourceMode::Lan))
                             ? static_cast<int>(SourceMode::Remote)
                             : static_cast<int>(SourceMode::Lan);
        g_ota->mode.store(next);
        g_ota->has_release.store(false);
        setStatus(g_ota, next == 0 ? "已切至局域网自动发现模式" : "已切至公网 HTTPS 模式");
    });
    lv_obj_set_pos(btn_mode, 24, 286);

    // Release info card
    lv_obj_t* card2 = lv_obj_create(scr);
    lv_obj_set_size(card2, 432, 160);
    lv_obj_set_pos(card2, 24, 362);
    lv_obj_set_style_bg_color(card2, lv_color_white(), 0);
    lv_obj_set_style_border_color(card2, lv_color_black(), 0);
    lv_obj_set_style_border_width(card2, 2, 0);
    lv_obj_set_style_radius(card2, 14, 0);
    lv_obj_set_style_pad_all(card2, 16, 0);
    lv_obj_remove_flag(card2, LV_OBJ_FLAG_SCROLLABLE);

    v->lbl_info = lv_label_create(card2);
    lv_obj_set_width(v->lbl_info, 396);
    lv_obj_set_style_text_font(v->lbl_info, UiFont(), 0);
    lv_obj_set_style_text_color(v->lbl_info, lv_color_black(), 0);
    lv_obj_set_pos(v->lbl_info, 0, 0);

    // Progress bar + status
    v->bar = lv_bar_create(scr);
    lv_obj_set_size(v->bar, 360, 26);
    lv_obj_set_pos(v->bar, 24, 548);
    lv_bar_set_range(v->bar, 0, 100);
    lv_obj_set_style_border_color(v->bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(v->bar, 2, 0);
    lv_obj_set_style_bg_color(v->bar, lv_color_white(), 0);
    lv_obj_set_style_bg_color(v->bar, lv_color_black(), LV_PART_INDICATOR);

    v->lbl_pct = lv_label_create(scr);
    lv_label_set_text(v->lbl_pct, "0%");
    lv_obj_set_style_text_font(v->lbl_pct, UiFont(), 0);
    lv_obj_set_style_text_color(v->lbl_pct, lv_color_black(), 0);
    lv_obj_set_pos(v->lbl_pct, 396, 548);

    v->lbl_status = lv_label_create(scr);
    lv_obj_set_width(v->lbl_status, 432);
    lv_obj_set_style_text_font(v->lbl_status, UiFont(), 0);
    lv_obj_set_style_text_color(v->lbl_status, lv_color_black(), 0);
    lv_obj_set_pos(v->lbl_status, 24, 592);

    // Action buttons
    lv_obj_t* btn_check = makeBtn(scr, "检查更新", 206, 68, [](lv_event_t*) {
        if (!g_ota || g_ota->busy.load()) return;
        g_ota->pending_cmd.store(static_cast<int>(Cmd::Check));
    });
    lv_obj_set_pos(btn_check, 24, 688);

    v->btn_upgrade = makeBtn(scr, "立即升级", 206, 68, [](lv_event_t*) {
        if (!g_ota || g_ota->busy.load()) return;
        g_ota->pending_cmd.store(static_cast<int>(Cmd::Upgrade));
    });
    lv_obj_set_pos(v->btn_upgrade, 250, 688);

    xTaskCreate(workerTask, "espdeck_ota", 10240, v, 4, &v->worker);
    v->ui_timer = lv_timer_create(refreshUi, 400, v);
    refreshUi(v->ui_timer);

    VkKeyScreenDesc desc{};
    desc.factory = Create;
    VkKey_AttachScreen(scr, "espdeck_ota", desc);

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        auto* view = static_cast<OtaView*>(lv_event_get_user_data(e));
        view->alive.store(false);
        if (view->ui_timer) {
            lv_timer_delete(view->ui_timer);
        }
        // Wait briefly for worker socket to close
        for (int i = 0; i < 15 && view->worker != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        if (view->lock) {
            vSemaphoreDelete(view->lock);
            view->lock = nullptr;
        }
        if (g_ota == view) g_ota = nullptr;
        delete view;
        PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
        PowerPolicy::GetInstance().Release(PowerNeed::UiKeepNet);
    }, LV_EVENT_DELETE, v);

    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestNextFullRefresh();
    }
    return scr;
}
