/*
 * esp32-isr-heartbeat
 *
 * A 1 Hz LED heartbeat driven by a hardware timer ISR, with a button counted by
 * a GPIO ISR. Deliberately the most obvious program in embedded software, so
 * that the only interesting things left in the file are the three that
 * interviews are actually about:
 *
 *   1. Which shared variables need `volatile`, and what breaks when they do not
 *      have it.                                   -> docs/volatile.md
 *   2. Which shared variables need more than `volatile`, because they are wider
 *      than the machine word.                     -> docs/torn-read.md
 *   3. What an ISR is and is not allowed to do.    -> docs/isr-discipline.md
 *
 * Target: ESP32 (Xtensa LX6, dual core), ESP-IDF v5.x.
 * Runs in Wokwi: see README.md.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "seqlock.h"

static const char *TAG = "heartbeat";

#define LED_GPIO        GPIO_NUM_2
#define BUTTON_GPIO     GPIO_NUM_4
#define TICK_HZ         1
#define TIMER_RES_HZ    1000000u        /* 1 MHz -> 1 timer tick = 1 us */
#define DEBOUNCE_US     50000u          /* 50 ms; see note in on_button_edge */

/* ------------------------------------------------------------------------- *
 * State shared between interrupt context and task context.
 *
 * Every one of these needs a written justification, because "I made it
 * volatile" is not an argument and "I made it atomic" is usually wrong on a
 * 32-bit core.
 * ------------------------------------------------------------------------- */

/*
 * Set by the timer ISR, cleared by the main loop.
 *
 * NEEDS volatile. The main loop reads it in a condition whose body never
 * writes it, so a compiler that cannot see the ISR is entitled to load it once
 * into a register and test that register forever. That is not a theoretical
 * hazard - experiments/volatile_demo.c reproduces it and prints the
 * disassembly. One aligned 8-bit store, so no tearing: volatile is sufficient
 * here as well as necessary.
 */
static volatile bool s_tick_pending = false;

/*
 * The timer count at the moment of the last tick, in microseconds.
 *
 * NEEDS MORE THAN volatile. 64 bits on a 32-bit core is two stores. volatile
 * guarantees the compiler emits them; it guarantees nothing about an interrupt
 * landing between them. Hence the seqlock. docs/torn-read.md has the
 * reproduction.
 */
static seqlock_u64_t s_last_tick_us = SEQLOCK_U64_INIT;

/*
 * Button press count, incremented by the GPIO ISR, read by the main loop.
 *
 * NEEDS volatile, does NOT need a seqlock. It is a single naturally-aligned
 * 32-bit word on a 32-bit core, so the store cannot tear. Note the asymmetry
 * with s_last_tick_us: the difference is width, not importance. Note also that
 * `s_button_events++` in the ISR is only safe because the ISR is the sole
 * writer - a read-modify-write shared between two writers would need an
 * atomic or a lock.
 */
static volatile uint32_t s_button_events = 0;

/* Touched only by the GPIO ISR. Not shared, therefore not volatile. */
static uint64_t s_last_button_us = 0;

/* ------------------------------------------------------------------------- *
 * Interrupt handlers
 * ------------------------------------------------------------------------- */

/*
 * IRAM_ATTR places this function in internal RAM rather than in memory-mapped
 * flash. Without it, the handler lives in flash and cannot run while the flash
 * cache is disabled - which happens during an OTA write, during an SPI flash
 * erase, and on the other core's cache flush. The symptom is a crash that only
 * ever occurs during a firmware update. See docs/isr-discipline.md.
 *
 * seqlock_u64_store is `static inline` in a header precisely so that it inlines
 * into this IRAM function instead of becoming a call into flash.
 */
static bool IRAM_ATTR on_timer_alarm(gptimer_handle_t timer,
                                     const gptimer_alarm_event_data_t *edata,
                                     void *user_ctx)
{
    (void)timer;
    (void)user_ctx;

    seqlock_u64_store(&s_last_tick_us, (uint64_t)edata->count_value);
    s_tick_pending = true;

    /* false: no higher-priority task was woken, so no yield is required. */
    return false;
}

/*
 * Debounce in the ISR by timestamp rather than by delay. A delay in an ISR is
 * always wrong; a comparison is free. This rejects contact bounce but will also
 * reject a genuine second press inside 50 ms, which is the trade being made
 * and should be stated rather than discovered.
 */
static void IRAM_ATTR on_button_edge(void *arg)
{
    (void)arg;

    const uint64_t now = (uint64_t)esp_timer_get_time();
    if (now - s_last_button_us < DEBOUNCE_US) {
        return;
    }
    s_last_button_us = now;
    s_button_events++;
}

/* ------------------------------------------------------------------------- *
 * Setup
 * ------------------------------------------------------------------------- */

static void configure_led(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(LED_GPIO, 0));
}

static void configure_button(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,   /* button pulls the pin to GND */
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /* ESP_INTR_FLAG_IRAM asserts that every handler registered on this service
     * is IRAM-safe. It is a promise the linker cannot check for you. */
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, on_button_edge, NULL));
}

static gptimer_handle_t configure_timer(void)
{
    gptimer_handle_t timer = NULL;

    const gptimer_config_t timer_cfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = TIMER_RES_HZ,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_cfg, &timer));

    const gptimer_event_callbacks_t cbs = { .on_alarm = on_timer_alarm };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_enable(timer));

    const gptimer_alarm_config_t alarm_cfg = {
        .reload_count = 0,
        .alarm_count  = TIMER_RES_HZ / TICK_HZ,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(timer, &alarm_cfg));
    ESP_ERROR_CHECK(gptimer_start(timer));

    return timer;
}

/* ------------------------------------------------------------------------- *
 * Main loop
 * ------------------------------------------------------------------------- */

void app_main(void)
{
    configure_led();
    configure_button();
    (void)configure_timer();

    ESP_LOGI(TAG, "up: led=gpio%d button=gpio%d tick=%dHz",
             (int)LED_GPIO, (int)BUTTON_GPIO, TICK_HZ);

    uint32_t beats = 0;
    uint32_t buttons_reported = 0;

    for (;;) {
        if (s_tick_pending) {
            s_tick_pending = false;

            const uint64_t tick_us = seqlock_u64_load(&s_last_tick_us);
            const uint32_t buttons = s_button_events;

            beats++;
            (void)gpio_set_level(LED_GPIO, beats & 1u);

            ESP_LOGI(TAG, "beat=%" PRIu32 " t=%" PRIu64 "us buttons=%" PRIu32 "%s",
                     beats, tick_us, buttons,
                     (buttons != buttons_reported) ? "  <- press" : "");
            buttons_reported = buttons;
        }

        /*
         * Yield. Without this the idle task never runs, the task watchdog fires
         * after 5 s, and the device reboots in a loop - which is a far more
         * common field failure than the volatile bug this program is about.
         *
         * Note for the volatile discussion: vTaskDelay is an opaque call, and a
         * call is a compiler barrier. It therefore MASKS the missing-volatile
         * bug on s_tick_pending, which is exactly why that bug ships. The
         * uncontaminated reproduction lives in experiments/, not here.
         */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
