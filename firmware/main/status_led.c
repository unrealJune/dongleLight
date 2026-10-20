// Bit-banged APA102 (the T-Dongle-S3's onboard RGB LED). Pins are Kconfig
// options because board revisions differ; check your board's pinout.
#include "status_led.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#if CONFIG_KSA_LED_ENABLE
#include "driver/gpio.h"

#include "freertos/semphr.h"

static led_state_t s_steady = LED_BOOT;
static TimerHandle_t s_timer;
static SemaphoreHandle_t s_lock; // several tasks + the timer task drive the LED

static void shift_byte(uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(CONFIG_KSA_LED_DATA_GPIO, (v >> i) & 1);
        gpio_set_level(CONFIG_KSA_LED_CLK_GPIO, 1);
        gpio_set_level(CONFIG_KSA_LED_CLK_GPIO, 0);
    }
}

static void write_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 4; i++) shift_byte(0x00);      // start frame
    shift_byte(0xE0 | 4);                                // global brightness 4/31
    shift_byte(b);
    shift_byte(g);
    shift_byte(r);
    for (int i = 0; i < 4; i++) shift_byte(0xFF);      // end frame
}

static void show(led_state_t s)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    switch (s) {
    case LED_BOOT:      write_rgb(40, 40, 40); break;
    case LED_SCANNING:  write_rgb(0, 0, 255); break;
    case LED_CONNECTED: write_rgb(0, 255, 0); break;
    case LED_FRAME:     write_rgb(255, 255, 255); break;
    case LED_BAD_FRAME: write_rgb(255, 0, 255); break;
    case LED_ERROR:     write_rgb(255, 0, 0); break;
    }
    xSemaphoreGive(s_lock);
}

static void restore(TimerHandle_t t) { (void)t; show(s_steady); }

void status_led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_KSA_LED_DATA_GPIO) | (1ULL << CONFIG_KSA_LED_CLK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    s_lock = xSemaphoreCreateMutex();
    s_timer = xTimerCreate("led", pdMS_TO_TICKS(100), pdFALSE, NULL, restore);
    show(LED_BOOT);
}

void status_led_set(led_state_t s)
{
    s_steady = s;
    show(s);
}

void status_led_flash(led_state_t s, int ms)
{
    show(s);
    xTimerChangePeriod(s_timer, pdMS_TO_TICKS(ms), 0); // also starts it
}
#else
void status_led_init(void) {}
void status_led_set(led_state_t s) { (void)s; }
void status_led_flash(led_state_t s, int ms) { (void)s; (void)ms; }
#endif
