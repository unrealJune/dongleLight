// Screen (ST7735, 80x160 portrait or 160x80 landscape), APA102 LED and BOOT button.
// Panel setup follows LILYGO's T-Dongle-S3 lcd example: BGR, inverted colors,
// byte-swapped RGB565 over SPI. In landscape, swap_xy (MV=1) swaps axes so one
// mirror bit (MX!=MY) yields a proper 90/270-degree rotation with gap (1, 26).
// In portrait, swap_xy is off (MV=0), so MX must equal MY: (false, false) puts
// y=0 at the USB plug end without mirroring; (true, true) rotates 180 degrees.
// The 0.96" 80x160 glass sits at offset (26, 1) in the ST7735's 132x162 RAM.
#include <string.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "ksb_ui_internal.h"

#if CONFIG_KSA_LCD_ENABLE
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7735.h"
#endif

static const char *TAG = "ksb_ui";
#define FRAME_MS 66 // ~15 fps

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------- LCD ----------
#if CONFIG_KSA_LCD_ENABLE
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static uint16_t *s_fb, *s_tx;

static bool on_flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

static bool lcd_init(void)
{
    s_fb = heap_caps_malloc(KSUI_W * KSUI_H * 2, MALLOC_CAP_INTERNAL);
    s_tx = heap_caps_malloc(KSUI_W * KSUI_H * 2, MALLOC_CAP_DMA);
    s_flush_done = xSemaphoreCreateBinary();
    if (!s_fb || !s_tx || !s_flush_done) return false;
    xSemaphoreGive(s_flush_done);

    spi_bus_config_t bus = ST7735_PANEL_BUS_SPI_CONFIG(CONFIG_KSA_LCD_SCLK, CONFIG_KSA_LCD_MOSI, KSUI_W * KSUI_H * 2);
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = ST7735_PANEL_IO_SPI_CONFIG(CONFIG_KSA_LCD_CS, CONFIG_KSA_LCD_DC,
                                                                      on_flush_done, NULL);
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io) != ESP_OK) return false;
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = CONFIG_KSA_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7735(io, &dev, &s_panel) != ESP_OK) return false;
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
#if CONFIG_KSA_LCD_PORTRAIT
    esp_lcd_panel_set_gap(s_panel, 26, 1);
    esp_lcd_panel_swap_xy(s_panel, false);
#if CONFIG_KSA_LCD_FLIP
    esp_lcd_panel_mirror(s_panel, true, true);
#else
    esp_lcd_panel_mirror(s_panel, false, false);
#endif
#else
    esp_lcd_panel_set_gap(s_panel, 1, 26);
    esp_lcd_panel_swap_xy(s_panel, true);
#if CONFIG_KSA_LCD_FLIP
    esp_lcd_panel_mirror(s_panel, true, false);
#else
    esp_lcd_panel_mirror(s_panel, false, true);
#endif
#endif
    esp_lcd_panel_disp_on_off(s_panel, true);

    gpio_config_t bl = {.pin_bit_mask = 1ULL << CONFIG_KSA_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(CONFIG_KSA_LCD_BL, 0); // active low
    return true;
}

static void lcd_draw(const ksui_view_t *v, uint32_t now)
{
    ksui_render(s_fb, v, now);
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200)); // previous DMA finished with s_tx
    for (int i = 0; i < KSUI_W * KSUI_H; i++) s_tx[i] = (uint16_t)((s_fb[i] >> 8) | (s_fb[i] << 8));
#if CONFIG_KSA_LCD_PORTRAIT
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, KSUI_H, KSUI_W, s_tx);
#else
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, KSUI_W, KSUI_H, s_tx);
#endif
}
#endif

// ---------- APA102 ----------
#if CONFIG_KSA_LED_ENABLE
static void led_byte(uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(CONFIG_KSA_LED_DATA_GPIO, (v >> i) & 1);
        gpio_set_level(CONFIG_KSA_LED_CLK_GPIO, 1);
        gpio_set_level(CONFIG_KSA_LED_CLK_GPIO, 0);
    }
}

static void led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 4; i++) led_byte(0x00);
    led_byte(0xE0 | 3); // global brightness 3/31: it sits right next to your eyes
    led_byte(b);
    led_byte(g);
    led_byte(r);
    for (int i = 0; i < 4; i++) led_byte(0xFF);
}

static void led_update(const ksui_view_t *v, uint32_t now)
{
    int top = ksb_top_source(&v->model);
    if (top >= 0 && v->model.src[top].state == KSA_SIG_NEEDS_YOU) {
        if ((now / 400) % 2) led_rgb(255, 150, 0);
        else led_rgb(0, 0, 0);
        return;
    }
    uint8_t r, g, b;
    ksb_scene_rgb(&v->shown[0], &r, &g, &b);
    led_rgb(r, g, b);
}
#endif

// ---------- button ----------
// Tap: acknowledge. Hold 3 s: claim another lamp. Hold 10 s: forget everything.
static void button_poll(void)
{
#if CONFIG_KSA_BUTTON_GPIO >= 0
    static bool down;
    static uint32_t down_at;
    bool pressed = gpio_get_level(CONFIG_KSA_BUTTON_GPIO) == 0;
    uint32_t now = now_ms();
    if (pressed && !down) {
        down = true;
        down_at = now;
    } else if (pressed) {
        ksb_bridge_button(now - down_at, false);
    } else if (down) {
        down = false;
        if (now - down_at > 40) ksb_bridge_button(now - down_at, true);
        else ksb_bridge_button(0, false);
    }
#endif
}

static void ui_task(void *arg)
{
#if CONFIG_KSA_LCD_ENABLE
    bool lcd_ok = lcd_init();
    if (!lcd_ok) ESP_LOGE(TAG, "LCD init failed; continuing without screen");
#endif
    static ksui_view_t view;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        uint32_t now = now_ms();
        ksb_bridge_view(&view);
#if CONFIG_KSA_LCD_ENABLE && CONFIG_KSA_LCD_PORTRAIT
        view.portrait = 1;
#endif
#if CONFIG_KSA_LCD_ENABLE
        if (lcd_ok) lcd_draw(&view, now);
#endif
#if CONFIG_KSA_LED_ENABLE
        led_update(&view, now);
#endif
        button_poll();
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_MS));
    }
}

void ksb_ui_start(void)
{
#if CONFIG_KSA_LED_ENABLE
    gpio_config_t led = {
        .pin_bit_mask = (1ULL << CONFIG_KSA_LED_DATA_GPIO) | (1ULL << CONFIG_KSA_LED_CLK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led);
#endif
#if CONFIG_KSA_BUTTON_GPIO >= 0
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << CONFIG_KSA_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
#endif
    xTaskCreate(ui_task, "ksb_ui", 6144, NULL, 3, NULL);
}
