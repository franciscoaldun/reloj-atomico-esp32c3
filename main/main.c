/* ============================================================
 *  reloj-atomico  —  ESP32-C3 SuperMini + OLED SSD1306 128x32
 * ------------------------------------------------------------
 *  Reloj "atomico" (NTP) con fuente futurista tipo HUD.
 *  3 pantallas que rotan solas (10 s / 5 s / 5 s):
 *    1) RELOJ   HH:MM grande + SS + centesimas + fecha + sync
 *    2) AHORA   temperatura de San Clemente y Talca
 *    3) MANANA  horas de lluvia (24h + AM/PM) y hora pico,
 *               o el max/min si esta despejado
 *
 *  Cableado I2C:  SDA = GPIO8   SCL = GPIO9   addr 0x3C
 *  Panel a 800 kHz para maximo refresco (si se ve con basura,
 *  bajar OLED_HZ a 400000).
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"

/* ------------------- CONFIGURA AQUI ----------------------- */
/* WIFI_SSID, WIFI_PASS y OWM_KEY viven en secrets.h (no se sube a git).
 * Copia secrets.h.example a secrets.h y rellena tus datos. */
#include "secrets.h"

#define SCL_LAT "-35.5372"
#define SCL_LON "-71.4832"          /* San Clemente, Maule */
#define TAL_LAT "-35.4264"
#define TAL_LON "-71.6554"          /* Talca */

#define TZ_CHILE "<-04>4<-03>,M9.1.6/24,M4.1.6/24"

/* ------------------- OLED / I2C --------------------------- */
#define I2C_SDA   8
#define I2C_SCL   9
#define OLED_ADDR 0x3C
#define OLED_HZ   800000            /* 800 kHz: max refresco (~170 fps) */
#define OLED_W    128
#define OLED_H    32

static const char *TAG = "reloj";
static uint8_t fb[OLED_W * OLED_H / 8];
static bool g_colon = true;

static inline void px(int x, int y)
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    fb[(y >> 3) * OLED_W + x] |= (uint8_t)(1u << (y & 7));
}
static void fillr(int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j);
}

/* ------------------- Fuente 5x7 (rotulos) ----------------- */
typedef struct { char c; uint8_t r[7]; } glyph_t;
static const glyph_t FONT[] = {
    {' ', {0,0,0,0,0,0,0}},
    {'0', {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110}},
    {'1', {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110}},
    {'2', {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111}},
    {'3', {0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110}},
    {'4', {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010}},
    {'5', {0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110}},
    {'6', {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110}},
    {'7', {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000}},
    {'8', {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110}},
    {'9', {0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100}},
    {'.', {0,0,0,0,0,0b01100,0b01100}},
    {'-', {0,0,0,0b11111,0,0,0}},
    {'/', {0b00001,0b00010,0b00100,0b00100,0b00100,0b01000,0b10000}},
    {':', {0,0b01100,0b01100,0,0b01100,0b01100,0}},
    {'%', {0b11001,0b11010,0b00100,0b01011,0b10011,0,0}},
    {0xB0,{0b01100,0b10010,0b10010,0b01100,0,0,0}},
    {'A', {0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}},
    {'B', {0b11110,0b10001,0b11110,0b10001,0b10001,0b10001,0b11110}},
    {'C', {0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110}},
    {'D', {0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110}},
    {'E', {0b11111,0b10000,0b11110,0b10000,0b10000,0b10000,0b11111}},
    {'F', {0b11111,0b10000,0b11110,0b10000,0b10000,0b10000,0b10000}},
    {'G', {0b01110,0b10001,0b10000,0b10111,0b10001,0b10001,0b01111}},
    {'H', {0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}},
    {'I', {0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110}},
    {'J', {0b00111,0b00010,0b00010,0b00010,0b00010,0b10010,0b01100}},
    {'K', {0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001}},
    {'L', {0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111}},
    {'M', {0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001}},
    {'N', {0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001}},
    {'O', {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}},
    {'P', {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000}},
    {'Q', {0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101}},
    {'R', {0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001}},
    {'S', {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110}},
    {'T', {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}},
    {'U', {0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}},
    {'V', {0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100}},
    {'W', {0b10001,0b10001,0b10001,0b10101,0b10101,0b11011,0b10001}},
    {'X', {0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001}},
    {'Y', {0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100}},
    {'Z', {0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111}},
};
#define NGLYPH (sizeof(FONT)/sizeof(FONT[0]))
#define DEG ((char)0xB0)

static const glyph_t *find_glyph(char c)
{
    for (unsigned i = 0; i < NGLYPH; i++) if (FONT[i].c == c) return &FONT[i];
    return NULL;
}
static void draw_char(int x0, int y0, char c)
{
    const glyph_t *g = find_glyph(c);
    if (!g) return;
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g->r[row] & (1u << (4 - col))) px(x0 + col, y0 + row);
}
static void draw_str(int x0, int y0, const char *s)
{
    int x = x0;
    while (*s) { draw_char(x, y0, *s++); x += 6; }
}
static int str_w(const char *s) { return (int)strlen(s) * 6 - 1; }
static void draw_str_c(int cx, int y0, const char *s) { draw_str(cx - str_w(s) / 2, y0, s); }

/* ------------------- Digitos hexagonales 7-seg ------------ */
static const uint8_t SEG[10][7] = {
    {1,1,1,1,1,1,0},{0,1,1,0,0,0,0},{1,1,0,1,1,0,1},{1,1,1,1,0,0,1},
    {0,1,1,0,0,1,1},{1,0,1,1,0,1,1},{1,0,1,1,1,1,1},{1,1,1,0,0,0,0},
    {1,1,1,1,1,1,1},{1,1,1,1,0,1,1},
};
static inline int seg_inset(int k, int t) { return (abs(2 * k - (t - 1)) + 1) / 2; }
static void hseg(int x, int y, int len, int t)
{
    for (int r = 0; r < t; r++) { int ins = seg_inset(r, t); for (int i = ins; i < len - ins; i++) px(x + i, y + r); }
}
static void vseg(int x, int y, int len, int t)
{
    for (int c = 0; c < t; c++) { int ins = seg_inset(c, t); for (int i = ins; i < len - ins; i++) px(x + c, y + i); }
}
static void hexdigit(int x, int y, int d, int cw, int ch, int t)
{
    if (d < 0 || d > 9) return;
    const uint8_t *s = SEG[d];
    int mid = y + (ch - t) / 2;
    if (s[0]) hseg(x, y, cw, t);
    if (s[3]) hseg(x, y + ch - t, cw, t);
    if (s[6]) hseg(x, mid, cw, t);
    if (s[5]) vseg(x, y, (mid + t) - y, t);
    if (s[1]) vseg(x + cw - t, y, (mid + t) - y, t);
    if (s[4]) vseg(x, mid, (y + ch) - mid, t);
    if (s[2]) vseg(x + cw - t, mid, (y + ch) - mid, t);
}
static int seg_width(const char *s, int cw, int gap)
{
    int w = 0;
    for (; *s; s++) { int cwx = (*s == '-') ? (cw * 7) / 10 : (*s == ':') ? cw / 2 : cw; w += cwx + gap; }
    return w - gap;
}
static int seg_str(int x, int y, const char *s, int cw, int ch, int t, int gap)
{
    int cx = x;
    for (; *s; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') { hexdigit(cx, y, c - '0', cw, ch, t); cx += cw + gap; }
        else if (c == '-') { int w = (cw * 7) / 10; hseg(cx, y + (ch - t) / 2, w, t); cx += w + gap; }
        else if (c == ':') {
            int w = cw / 2;
            if (g_colon) { int dx = cx + (w - t) / 2; fillr(dx, y + (ch * 28) / 100, t, t); fillr(dx, y + (ch * 62) / 100, t, t); }
            cx += w + gap;
        } else { cx += cw + gap; }
    }
    return cx - gap;
}
static void deg_ring(int x, int y)
{
    px(x+1,y); px(x+2,y); px(x,y+1); px(x+3,y+1);
    px(x,y+2); px(x+3,y+2); px(x+1,y+3); px(x+2,y+3);
}

/* ------------------- Driver SSD1306 ----------------------- */
static i2c_master_dev_handle_t dev;
static void oled_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[40]; buf[0] = 0x00; memcpy(buf + 1, cmds, n);
    i2c_master_transmit(dev, buf, n + 1, 200);
}
static void oled_flush(void)
{
    static const uint8_t win[] = { 0x21,0x00,0x7F, 0x22,0x00,0x03 };
    oled_cmds(win, sizeof(win));
    static uint8_t out[sizeof(fb) + 1];
    out[0] = 0x40; memcpy(out + 1, fb, sizeof(fb));
    i2c_master_transmit(dev, out, sizeof(out), 200);
}
static void oled_init(void)
{
    static const uint8_t seq[] = {
        0xAE, 0xD5,0xF0, 0xA8,0x1F, 0xD3,0x00, 0x40, 0x8D,0x14,
        0x20,0x00, 0xA1, 0xC8, 0xDA,0x02, 0x81,0x8F, 0xD9,0xF1,
        0xDB,0x40, 0xA4, 0xA6, 0x2E, 0xAF,
    };
    oled_cmds(seq, sizeof(seq));
}
static void oled_i2c_start(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = -1,
        .scl_io_num = I2C_SCL, .sda_io_num = I2C_SDA,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDR, .scl_speed_hz = OLED_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &dev));
    oled_init();
}
static void screen_status(const char *l1, const char *l2)
{
    memset(fb, 0, sizeof(fb));
    if (l1) draw_str_c(64, 6, l1);
    if (l2) draw_str_c(64, 18, l2);
    oled_flush();
}

/* ------------------- WiFi --------------------------------- */
static EventGroupHandle_t wifi_eg;
#define WIFI_CONNECTED_BIT BIT0
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_eg, WIFI_CONNECTED_BIT); esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(wifi_eg, WIFI_CONNECTED_BIT);
}
static void wifi_start(void)
{
    wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&c));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL));
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ------------------- NTP ---------------------------------- */
static volatile bool time_synced = false;
static volatile int64_t g_last_sync_us = 0;
static void time_sync_cb(struct timeval *tv)
{
    time_synced = true; g_last_sync_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Hora NTP OK");
}
static void sntp_start(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = time_sync_cb;
    esp_netif_sntp_init(&cfg);
    sntp_set_sync_interval(300000);   /* re-sincroniza cada 5 min */
    esp_sntp_restart();
    setenv("TZ", TZ_CHILE, 1); tzset();
}

/* ------------------- HTTP + OpenWeather ------------------- */
static char http_buf[12288];
static int  http_len = 0;
static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        if (http_len + e->data_len < (int)sizeof(http_buf) - 1) {
            memcpy(http_buf + http_len, e->data, e->data_len);
            http_len += e->data_len;
        }
    }
    return ESP_OK;
}
static bool http_get(const char *url)
{
    http_len = 0;
    esp_http_client_config_t cfg = { .url = url, .event_handler = http_evt, .timeout_ms = 9000 };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(cl);
    int status = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);
    http_buf[http_len] = 0;
    if (err == ESP_OK && status == 200) return true;
    ESP_LOGW(TAG, "HTTP fallo err=%d status=%d", (int)err, status);
    return false;
}

static float g_scl = 0, g_tal = 0;
static bool  g_now_ok = false;
static bool  g_tmr_ok = false, g_tmr_rain = false;
static int   g_tmr_from = 0, g_tmr_to = 0, g_tmr_pop = 0, g_tmr_peak = 0, g_tmr_max = 0, g_tmr_min = 0;

static float parse_temp_at(char *p)
{
    char *t = p ? strstr(p, "\"temp\":") : NULL;
    return t ? strtof(t + 7, NULL) : -999.0f;
}
static void fetch_now(void)
{
    char url[200]; bool a = false, b = false;
    snprintf(url, sizeof(url),
        "http://api.openweathermap.org/data/2.5/weather?lat=%s&lon=%s&units=metric&appid=%s",
        SCL_LAT, SCL_LON, OWM_KEY);
    if (http_get(url)) { float v = parse_temp_at(http_buf); if (v > -900) { g_scl = v; a = true; } }
    snprintf(url, sizeof(url),
        "http://api.openweathermap.org/data/2.5/weather?lat=%s&lon=%s&units=metric&appid=%s",
        TAL_LAT, TAL_LON, OWM_KEY);
    if (http_get(url)) { float v = parse_temp_at(http_buf); if (v > -900) { g_tal = v; b = true; } }
    g_now_ok = a && b;
    if (g_now_ok) ESP_LOGI(TAG, "Ahora: SCL %dC / TAL %dC", (int)g_scl, (int)g_tal);
}
static void fetch_forecast(void)
{
    char url[210];
    snprintf(url, sizeof(url),
        "http://api.openweathermap.org/data/2.5/forecast?lat=%s&lon=%s&units=metric&cnt=16&appid=%s",
        SCL_LAT, SCL_LON, OWM_KEY);
    if (!http_get(url)) return;

    time_t tmr = time(NULL) + 86400;
    struct tm tt; localtime_r(&tmr, &tt);
    int md = tt.tm_mday, mo = tt.tm_mon;

    bool rain = false; int from = 99, to = -1, pop = 0, peak = 0;
    float tmin = 1e9f, tmax = -1e9f, peakm = -1.0f;

    char *p = http_buf;
    while ((p = strstr(p, "\"dt\":")) != NULL) {
        long dt = strtol(p + 5, NULL, 10);
        char *nxt = strstr(p + 5, "\"dt\":");

        char *pt = strstr(p + 5, "\"temp\":");
        float temp = (pt && (!nxt || pt < nxt)) ? strtof(pt + 7, NULL) : -999.0f;

        char cond[16] = "";
        char *pm = strstr(p + 5, "\"main\":\"");
        if (pm && (!nxt || pm < nxt)) {
            pm += 8; int i = 0;
            while (pm[i] && pm[i] != '"' && i < 15) { cond[i] = pm[i]; i++; }
            cond[i] = 0;
        }
        char *pp = strstr(p + 5, "\"pop\":");
        float popf = (pp && (!nxt || pp < nxt)) ? strtof(pp + 6, NULL) : 0.0f;

        float vol = 0.0f;
        char *pr = strstr(p + 5, "\"rain\":");
        if (pr && (!nxt || pr < nxt)) {
            char *v3 = strstr(pr, "\"3h\":");
            if (v3 && (!nxt || v3 < nxt)) vol = strtof(v3 + 5, NULL);
        }

        struct tm em; time_t et = (time_t)dt; localtime_r(&et, &em);
        if (em.tm_mday == md && em.tm_mon == mo) {
            if (temp > -900) { if (temp < tmin) tmin = temp; if (temp > tmax) tmax = temp; }
            bool r = (!strcmp(cond, "Rain") || !strcmp(cond, "Drizzle") ||
                      !strcmp(cond, "Thunderstorm")) || popf >= 0.5f;
            if (r) {
                rain = true;
                if (em.tm_hour < from) from = em.tm_hour;
                if (em.tm_hour + 3 > to) to = em.tm_hour + 3;
                int pv = (int)(popf * 100 + 0.5f); if (pv > pop) pop = pv;
                float metric = vol > 0 ? vol : popf;      /* "cuando llueve mas" */
                if (metric > peakm) { peakm = metric; peak = em.tm_hour; }
            }
        }
        if (!nxt) break;
        p = nxt;
    }

    if (tmax > -1e8f) {
        g_tmr_rain = rain;
        g_tmr_from = from; g_tmr_to = to > 24 ? 24 : to; g_tmr_pop = pop; g_tmr_peak = peak;
        g_tmr_max = (int)(tmax >= 0 ? tmax + 0.5f : tmax - 0.5f);
        g_tmr_min = (int)(tmin >= 0 ? tmin + 0.5f : tmin - 0.5f);
        g_tmr_ok = true;
        ESP_LOGI(TAG, "Manana: rain=%d %d-%dh pico=%dh pop=%d max=%d min=%d",
                 rain, from, to, peak, pop, g_tmr_max, g_tmr_min);
    }
}

/* ------------------- Tarea de clima (no congela el reloj) -- */
static void weather_task(void *arg)
{
    while (!time_synced) vTaskDelay(pdMS_TO_TICKS(500));
    fetch_now();
    fetch_forecast();
    int64_t last_now = esp_timer_get_time(), last_fc = esp_timer_get_time();
    for (;;) {
        int64_t t = esp_timer_get_time();
        if (t - last_now > (g_now_ok ? 300000000LL : 60000000LL)) { fetch_now();      last_now = esp_timer_get_time(); }
        if (t - last_fc  > (g_tmr_ok ? 1800000000LL : 120000000LL)) { fetch_forecast(); last_fc  = esp_timer_get_time(); }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ------------------- Pantallas ---------------------------- */
static const char *DOW[7] = { "DOM","LUN","MAR","MIE","JUE","VIE","SAB" };
static const char *MON[12] = { "ENE","FEB","MAR","ABR","MAY","JUN","JUL","AGO","SEP","OCT","NOV","DIC" };

static void fmt_ampm(char *out, int h)   /* 0..24 -> "6PM" / "12AM" */
{
    int hh = h % 24; const char *ap = hh < 12 ? "AM" : "PM";
    int h12 = hh % 12; if (h12 == 0) h12 = 12;
    snprintf(out, 8, "%d%s", h12, ap);
}

static void draw_clock(void)
{
    memset(fb, 0, sizeof(fb));
    struct timeval tv; gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec; struct tm tm; localtime_r(&now, &tm);
    long ms = tv.tv_usec / 1000;             /* milisegundos reales y exactos 0..999 */
    if (ms < 0) ms = 0;
    if (ms > 999) ms = 999;
    int hh = tm.tm_hour % 24, mm = tm.tm_min % 60, sec = tm.tm_sec % 60;

    g_colon = true;
    char hm[8]; snprintf(hm, sizeof(hm), "%02d:%02d", hh, mm);
    seg_str(2, 0, hm, 14, 18, 3, 2);        /* HH:MM grande */

    char ss[6]; snprintf(ss, sizeof(ss), "%02d", sec);
    seg_str(98, 0, ss, 11, 13, 2, 2);       /* SS mediano arriba-der */

    char cc[8]; snprintf(cc, sizeof(cc), ".%03ld", ms);
    draw_str(98, 15, cc);                    /* 3 decimales (ms): exacto al techo mundial */

    char ds[24]; snprintf(ds, sizeof(ds), "%s %02d %s", DOW[tm.tm_wday], tm.tm_mday, MON[tm.tm_mon]);
    draw_str(2, 25, ds);                     /* fecha abajo-izq */

    char sy[16];
    if (g_last_sync_us == 0) snprintf(sy, sizeof(sy), "NO SYNC");
    else { int m = (int)((esp_timer_get_time() - g_last_sync_us) / 60000000LL); snprintf(sy, sizeof(sy), "NTP %dM", m); }
    draw_str(128 - str_w(sy) - 1, 25, sy);   /* frescura NTP abajo-der */
}

static void temp_block(int cx, const char *label, float val, bool ok)
{
    draw_str_c(cx, 0, label);
    char s[8];
    if (ok) snprintf(s, sizeof(s), "%d", (int)(val >= 0 ? val + 0.5f : val - 0.5f));
    else    snprintf(s, sizeof(s), "--");
    int cw = 11, ch = 17, t = 3, gap = 3;
    int sw = seg_width(s, cw, gap);
    int x0 = cx - (sw + 6) / 2;
    seg_str(x0, 10, s, cw, ch, t, gap);
    deg_ring(x0 + sw + 2, 10);
}
static void draw_now(void)
{
    memset(fb, 0, sizeof(fb));
    temp_block(33, "S.CLEM", g_scl, g_now_ok);
    temp_block(95, "TALCA",  g_tal, g_now_ok);
    for (int y = 4; y < 30; y += 3) px(64, y);
}

static void draw_tomorrow(void)
{
    memset(fb, 0, sizeof(fb));
    if (!g_tmr_ok) { draw_str_c(64, 6, "MANANA"); draw_str_c(64, 18, "CARGANDO"); return; }

    if (g_tmr_rain) {
        draw_str_c(64, 0, "MANANA LLUVIA");
        char hrs[8]; snprintf(hrs, sizeof(hrs), "%d-%d", g_tmr_from, g_tmr_to);
        int cw = 12, ch = 15, t = 3, gap = 3;
        int sw = seg_width(hrs, cw, gap);
        int x0 = 64 - (sw + 7) / 2;
        seg_str(x0, 8, hrs, cw, ch, t, gap);
        draw_str(x0 + sw + 3, 10, "H");
        char a[8], b[8]; fmt_ampm(a, g_tmr_from); fmt_ampm(b, g_tmr_to);
        int peak = g_tmr_peak % 24;
        char bot[40]; snprintf(bot, sizeof(bot), "%s-%s PICO%dH", a, b, peak);
        draw_str_c(64, 25, bot);
    } else {
        draw_str_c(64, 0, "MANANA DESPEJADO");
        char s[6]; snprintf(s, sizeof(s), "%d", g_tmr_max);
        int cw = 13, ch = 15, t = 3, gap = 3;
        int sw = seg_width(s, cw, gap);
        int x0 = 64 - (sw + 6) / 2;
        seg_str(x0, 8, s, cw, ch, t, gap);
        deg_ring(x0 + sw + 2, 8);
        char mn[40]; snprintf(mn, sizeof(mn), "MIN %d%c  MAX %d%c", g_tmr_min, DEG, g_tmr_max, DEG);
        draw_str_c(64, 25, mn);
    }
}

/* ------------------- MAIN --------------------------------- */
void app_main(void)
{
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }

    oled_i2c_start();
    screen_status("RELOJ ATOMICO", "INICIANDO");

    wifi_start();
    int dots = 0;
    while (!(xEventGroupGetBits(wifi_eg) & WIFI_CONNECTED_BIT)) {
        char l2[16]; snprintf(l2, sizeof(l2), "WIFI%.*s", dots % 4, "...");
        screen_status("CONECTANDO", l2);
        dots++; vTaskDelay(pdMS_TO_TICKS(500));
    }

    sntp_start();
    screen_status("SINCRONIZANDO", "HORA MUNDIAL");
    for (int i = 0; i < 30 && !time_synced; i++) vTaskDelay(pdMS_TO_TICKS(500));

    xTaskCreate(weather_task, "wx", 6144, NULL, 4, NULL);

    int cur = 0;
    const int64_t period[3] = { 10000000, 5000000, 5000000 };  /* 10s / 5s / 5s */
    int64_t last_switch = esp_timer_get_time();
    ESP_LOGI(TAG, "Reloj en marcha.");

    for (;;) {
        int64_t us = esp_timer_get_time();
        if (us - last_switch > period[cur]) { cur = (cur + 1) % 3; last_switch = us; }
        if      (cur == 0) draw_clock();
        else if (cur == 1) draw_now();
        else               draw_tomorrow();
        oled_flush();
        vTaskDelay(pdMS_TO_TICKS(1));       /* loop rapido -> centesimas fluidas */
    }
}
