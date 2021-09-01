
#include "esp_log.h"
#include "driver/rmt.h"

#include <math.h>

#include "common.h"
#include "led_strip.h"

static const char *TAG = "led chase";

#define PASTER(x,y) x ## _ ## y
#define EVALUATOR(x,y)  PASTER(x,y)
#define _BUILD_GPIO_PIN(pin) EVALUATOR(GPIO_NUM, pin)

#define LED_RMT_CHANNEL  RMT_CHANNEL_0
#define LED_GPIO         _BUILD_GPIO_PIN(CONFIG_LED_PIN)
#define LED_STRIP_LENGTH CONFIG_LED_STRIP_LENGTH
#define LEDS_PER_ROUND   CONFIG_LEDS_PER_ROUND


#define LED_UPDATE_SPEED (20)

extern enum state_ state;

static void led_strip_hsv2rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    h %= 360;
    uint32_t rgb_max = v * 2.55f;
    uint32_t rgb_min = rgb_max * (100 - s) / 100.0f;

    uint32_t i = h / 60;
    uint32_t diff = h % 60;

    // RGB adjustment amount by hue
    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    switch (i) {
    case 0:
        *r = rgb_max;
        *g = rgb_min + rgb_adj;
        *b = rgb_min;
        break;
    case 1:
        *r = rgb_max - rgb_adj;
        *g = rgb_max;
        *b = rgb_min;
        break;
    case 2:
        *r = rgb_min;
        *g = rgb_max;
        *b = rgb_min + rgb_adj;
        break;
    case 3:
        *r = rgb_min;
        *g = rgb_max - rgb_adj;
        *b = rgb_max;
        break;
    case 4:
        *r = rgb_min + rgb_adj;
        *g = rgb_min;
        *b = rgb_max;
        break;
    default:
        *r = rgb_max;
        *g = rgb_min;
        *b = rgb_max - rgb_adj;
        break;
    }
}

static void blank(led_strip_t *strip)
{
    strip->clear(strip, 100);
}

static void white_pulse(led_strip_t *strip)
{
    static uint16_t phase = 0;

    static const uint8_t min_brightness = 50;
    static const uint8_t max_brightness = 200;
    static const uint8_t pulse_slowness = 200;

    const uint8_t brightness = min_brightness + (max_brightness-min_brightness)*sin((float)phase / (float)pulse_slowness);

    for (int j = 0; j < LED_STRIP_LENGTH; j += 1) {
        ESP_ERROR_CHECK(strip->set_pixel(strip, j, brightness, brightness, brightness));
    }
    phase++;
}

static void red_chaser(led_strip_t *strip)
{
    static uint8_t start_chaser = 0;

    uint16_t hue = 0;
    uint8_t lightness = 100;
    uint8_t red, green, blue;

    for (int j = 0; j < LED_STRIP_LENGTH; j += 1) {
        lightness = 60 + 30*sin((float)(2*3.1415*j / LEDS_PER_ROUND + start_chaser) / 10.0f);
        led_strip_hsv2rgb(hue, 100, lightness, &red, &green, &blue);
        ESP_ERROR_CHECK(strip->set_pixel(strip, j, red, green, blue));
    }
    start_chaser++;
}

static void red_blink(led_strip_t *strip)
{
    static uint16_t counter = 0;
    static bool on = 0;
    if(counter++ > 500 / LED_UPDATE_SPEED)
    {
        counter = 0;
        on = !on;
        for (int j = 0; j < LED_STRIP_LENGTH; j += 1) {
            ESP_ERROR_CHECK(strip->set_pixel(strip, j, 250*on, 0, 0));
        }
    }
}

static void yellow(led_strip_t *strip)
{
    uint16_t hue = 60;
    uint8_t lightness = 80;
    uint8_t red, green, blue;
    led_strip_hsv2rgb(hue, 100, lightness, &red, &green, &blue);

    for (int j = 0; j < LED_STRIP_LENGTH; j += 1) {
        ESP_ERROR_CHECK(strip->set_pixel(strip, j, red, green, blue));
    }
}

static void rainbow(led_strip_t *strip)
{
    static uint16_t start_rgb = 0;
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    uint16_t hue = 0;

    for (int j = 0; j < LED_STRIP_LENGTH; j += 1) {
        hue = j * 360 / LEDS_PER_ROUND + start_rgb;
        led_strip_hsv2rgb(hue, 100, 100, &red, &green, &blue);
        ESP_ERROR_CHECK(strip->set_pixel(strip, j, red, green, blue));
    }
    start_rgb += 10;
    start_rgb %= 360;
}


static void led_worker(void *bogus_param)
{

    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(LED_GPIO, LED_RMT_CHANNEL);
    // set counter clock to 40MHz
    config.clk_div = 2;

    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));

    // install ws2812 driver
    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(LED_STRIP_LENGTH, (led_strip_dev_t)config.channel);
    led_strip_t *strip = led_strip_new_rmt_ws2812(&strip_config);
    if (!strip) {
        state = STATE_ERROR;
        ESP_LOGE(TAG, "install WS2812 driver failed");
    }

    while (true) {
        switch(state) {
            case STATE_STARTING:
                white_pulse(strip);
                break;
            case STATE_CONNECTING:
                rainbow(strip);
                break;
            case STATE_DISCONNECTED:
                blank(strip);
                break;
            case STATE_IDLE:
                blank(strip);
                break;
            case STATE_OFF_AIR:
                yellow(strip);
                break;
            case STATE_ON_AIR:
                red_chaser(strip);
                break;
            case STATE_ERROR:
                red_blink(strip);
            default:
                break;
        }
        // Flush RGB values to LEDs
        ESP_ERROR_CHECK(strip->refresh(strip, 100));
        vTaskDelay(pdMS_TO_TICKS(LED_UPDATE_SPEED));
    }
}


#define STACK_SIZE 2000
static StaticTask_t xTaskBuffer;
static StackType_t xStack[ STACK_SIZE ];
static TaskHandle_t task_handle = NULL;

void led_task_start(void)
{
    task_handle = xTaskCreateStatic(
                  led_worker,
                  "LED worker",
                  STACK_SIZE,
                  ( void * ) 0,
                  tskIDLE_PRIORITY,
                  xStack,
                  &xTaskBuffer );
}

