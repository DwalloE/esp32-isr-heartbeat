/*
 * Browser demo of esp32-isr-heartbeat - an Arduino-core PORT, not the firmware.
 *
 * Wokwi's browser editor compiles Arduino, not ESP-IDF, so this file exists
 * only to give the shared Wokwi project a play button that actually beats.
 * The canonical firmware is main/main.c (ESP-IDF v5.x), which CI builds and
 * runs on a simulated ESP32 on every commit:
 *
 *     https://github.com/DwalloE/esp32-isr-heartbeat
 *
 * The port keeps the part that matters - the shared-variable discipline:
 *
 *   - s_tick_pending  : volatile bool. One aligned byte; volatile is
 *                       necessary AND sufficient. See docs/volatile.md.
 *   - s_last_tick_us  : 64 bits on a 32-bit core = two stores. volatile is
 *                       necessary and NOT sufficient; it crosses via the same
 *                       seqlock as main/seqlock.h. See docs/torn-read.md.
 *   - s_button_events : volatile uint32_t. One aligned word; cannot tear.
 *                       The ++ is safe only because the ISR is the sole writer.
 */
#include <Arduino.h>
#include "esp_timer.h"

#define LED_GPIO     2
#define BUTTON_GPIO  4
#define DEBOUNCE_US  50000u

/* ---- seqlock, verbatim from main/seqlock.h ------------------------------ */
typedef struct {
  volatile uint32_t seq;   /* even: stable.  odd: a write is in progress. */
  volatile uint32_t hi;
  volatile uint32_t lo;
} seqlock_u64_t;

static inline void seqlock_u64_store(seqlock_u64_t *s, uint64_t v) {
  const uint32_t seq = s->seq + 1u;              /* -> odd */
  s->seq = seq;
  __atomic_thread_fence(__ATOMIC_RELEASE);       /* seq before value */
  s->hi = (uint32_t)(v >> 32);
  s->lo = (uint32_t)(v & 0xFFFFFFFFu);
  __atomic_thread_fence(__ATOMIC_RELEASE);       /* value before seq */
  s->seq = seq + 1u;                             /* -> even */
}

static inline uint64_t seqlock_u64_load(const seqlock_u64_t *s) {
  for (;;) {
    const uint32_t before = s->seq;
    if (before & 1u) continue;                   /* writer mid-flight */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint32_t hi = s->hi;
    const uint32_t lo = s->lo;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (s->seq == before) return ((uint64_t)hi << 32) | (uint64_t)lo;
  }
}

/* ---- shared state, same justifications as main/main.c ------------------- */
static volatile bool s_tick_pending = false;
static seqlock_u64_t s_last_tick_us = { 0, 0, 0 };
static volatile uint32_t s_button_events = 0;
static uint64_t s_last_button_us = 0;            /* ISR-only, not shared */

/* ---- interrupt handlers -------------------------------------------------- */
void IRAM_ATTR onTimerAlarm() {
  seqlock_u64_store(&s_last_tick_us, (uint64_t)esp_timer_get_time());
  s_tick_pending = true;
}

void IRAM_ATTR onButtonEdge() {
  const uint64_t now = (uint64_t)esp_timer_get_time();
  if (now - s_last_button_us < DEBOUNCE_US) return;   /* debounce by timestamp */
  s_last_button_us = now;
  s_button_events++;
}

/* ---- setup / loop -------------------------------------------------------- */
hw_timer_t *s_timer = nullptr;

void setup() {
  Serial.begin(115200);
  pinMode(LED_GPIO, OUTPUT);
  pinMode(BUTTON_GPIO, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_GPIO), onButtonEdge, FALLING);

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  s_timer = timerBegin(1000000);                 /* 1 MHz: 1 tick = 1 us */
  timerAttachInterrupt(s_timer, &onTimerAlarm);
  timerAlarm(s_timer, 1000000, true, 0);         /* 1 Hz, auto-reload */
#else
  s_timer = timerBegin(0, 80, true);             /* 80 MHz APB / 80 = 1 MHz */
  timerAttachInterrupt(s_timer, &onTimerAlarm, true);
  timerAlarmWrite(s_timer, 1000000, true);       /* 1 Hz, auto-reload */
  timerAlarmEnable(s_timer);
#endif

  Serial.printf("I (%lu) heartbeat: up: led=gpio%d button=gpio%d tick=1Hz (browser port)\n",
                millis(), LED_GPIO, BUTTON_GPIO);
}

void loop() {
  static uint32_t beats = 0;
  static uint32_t buttons_reported = 0;

  if (s_tick_pending) {
    s_tick_pending = false;

    const uint64_t tick_us = seqlock_u64_load(&s_last_tick_us);
    const uint32_t buttons = s_button_events;

    beats++;
    digitalWrite(LED_GPIO, beats & 1u);

    Serial.printf("I (%lu) heartbeat: beat=%u t=%lluus buttons=%u%s\n",
                  (unsigned long)millis(), beats, (unsigned long long)tick_us,
                  buttons, (buttons != buttons_reported) ? "  <- press" : "");
    buttons_reported = buttons;
  }

  delay(10);   /* yield - and see main.c on why the DURATION matters */
}
