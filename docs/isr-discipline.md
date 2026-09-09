# What an ISR may and may not do

Every rule below has a field failure attached. The rules without one are folklore.

## 1. It must be short, and "short" is a number

An interrupt handler runs with equal or lower-priority interrupts masked. Its execution time is
added directly to the worst-case latency of everything below it. If the ISR takes 200 µs and a
UART at 115200 baud gives you 86 µs per byte before overrun, you have designed a data-loss bug
into the system.

**The number to know for your own device:** worst-case ISR duration versus the tightest deadline
in the system. If you cannot state it, you do not know whether the firmware works — you know it
has not failed yet.

In this project the timer ISR does three stores and returns. The work — formatting, logging,
driving the LED — happens in task context.

## 2. No blocking, ever

No `vTaskDelay`, no mutex, no `malloc`, no file or flash I/O, no waiting on anything. A blocking
call in an ISR either asserts, or deadlocks with interrupts disabled, which on ESP32 means the
interrupt watchdog fires and the device reboots. The reboot is the good outcome; the bad one is a
priority inversion that only manifests under load.

`malloc` deserves its own line: it takes a lock, so it is out on those grounds alone, but it is
also non-deterministic in duration and can fail. Dynamic allocation in an ISR is never right.

## 3. No `printf`, and the reason generalises

`printf` takes a lock, allocates, and can block on the UART. In this project's host demo,
`experiments/volatile_demo.c`, the signal handler uses `write(2)` and `_exit(2)` — the only two
async-signal-safe calls in it — for exactly the same reason. The generalised rule: **an ISR may
only call functions that are documented as safe to call from an ISR.** On ESP-IDF that means the
`...FromISR` variants (`xQueueSendFromISR`, `xSemaphoreGiveFromISR`, `xTaskNotifyFromISR`), and
nothing that is not on the list. There is no partial credit; a function that takes a lock 1% of
the time fails 1% of the time.

For logging from interrupt context, use `ESP_EARLY_LOGx` (which bypasses the lock and blocks on
the UART, so it is a debugging tool, not a shipping one) or, better, put the event in a queue and
log it from a task.

## 4. `IRAM_ATTR`, and the crash that only happens during OTA

On ESP32, code lives in memory-mapped external flash and is reached through a cache. When the
cache is disabled, code in flash cannot execute. The cache is disabled during:

- an SPI flash write or erase — which means **during an OTA update**,
- `esp_flash` operations from the other core,
- some sleep transitions.

An ISR in flash that fires during any of those crashes the device. The symptom is a device that
works perfectly for months and bricks itself during a firmware update, i.e. the worst possible
time and on your whole fleet at once.

`IRAM_ATTR` places the function in internal RAM instead. This is why `on_timer_alarm` and
`on_button_edge` carry it — and why `seqlock_u64_store` is `static inline` in a header rather than
a function in a `.c` file: it inlines into the IRAM handler instead of becoming a call back into
flash. An `IRAM_ATTR` handler that calls a non-IRAM function has gained you nothing, and the
linker will not tell you.

`ESP_INTR_FLAG_IRAM`, passed to `gpio_install_isr_service()`, is your assertion that every handler
on that service is IRAM-safe. It is a promise the toolchain cannot verify. Also note: constant
strings referenced from an IRAM handler live in `.rodata` in flash unless you place them
explicitly. IRAM is a scarce resource — check `idf.py size` after adding to it.

## 5. Debounce by timestamp, not by delay

```c
const uint64_t now = (uint64_t)esp_timer_get_time();
if (now - s_last_button_us < DEBOUNCE_US) {
    return;
}
s_last_button_us = now;
```

A comparison is free; a delay is rule 2. Two things worth being explicit about:

- The trade: this also rejects a genuine second press inside 50 ms. That is a product decision,
  and it should be stated in the code rather than discovered by a user.
- `s_last_button_us` is touched only by the ISR. It is therefore **not** shared, and correctly
  **not** `volatile`. Marking everything `volatile` because some things need to be is how you end
  up with firmware that is slow for no reason and still wrong where it matters.

## 6. Hand off with intent, and know what you are handing

| Mechanism | Use it for | Cost |
|---|---|---|
| `volatile` flag (this project) | "Something happened, look when you can" | Nothing. Loses count if the task is late. |
| Counter | "N things happened" | Nothing. Loses ordering and timing. |
| Seqlock | "Here is the latest wide value" | Reader may retry. See [`torn-read.md`](torn-read.md). |
| `xQueueSendFromISR` | "Here is every event, in order" | Memory, a copy, and it can fail when full — **check the return value.** |
| `xTaskNotifyFromISR` | "Wake this specific task now" | Cheapest real wakeup. Use it instead of a queue when there is one waiter and no payload. |

The flag in this project is the weakest of these on purpose: if the main loop is late, the second
tick overwrites the first and a beat is silently lost. For a heartbeat that is correct behaviour —
you want the current state, not the backlog. For sensor samples it would be data loss, which is
what portfolio project 02 is about.

## 7. The return value means something

```c
static bool IRAM_ATTR on_timer_alarm(...)
{
    ...
    return false;   /* no higher-priority task woken -> no yield needed */
}
```

Returning `true` requests a context switch on the way out of the ISR. Return it when you have
woken a task that should preempt the current one — otherwise your low-latency wakeup waits for the
next tick, which is up to 10 ms on a default 100 Hz FreeRTOS. In the raw FreeRTOS spelling this is
the `pxHigherPriorityTaskWoken` out-parameter and `portYIELD_FROM_ISR()`. Getting this wrong
produces firmware that works but has mysterious latency, which is much harder to find than
firmware that crashes.

## 8. On a dual-core ESP32, an ISR is pinned to a core

An interrupt is allocated on the core that registered it, and it fires only on that core.
`portENTER_CRITICAL` on ESP-IDF takes a real spinlock rather than merely disabling interrupts,
because disabling interrupts on core 0 does nothing to a task running on core 1. Any reasoning of
the form "interrupts are off, so nothing else can touch this" is single-core reasoning and is
wrong here. This is one of the places where the ESP-IDF abstraction differs from the bare-metal
Cortex-M model an STM32 interview will assume, and it is worth being able to name the difference
in both directions.
