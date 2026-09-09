# esp32-isr-heartbeat

A 1 Hz LED heartbeat on an ESP32, driven by a hardware timer ISR — deliberately the most obvious
program in embedded software, written so that the only things left in it are the three that
interviews are actually about.

[![ci](https://github.com/DwalloE/esp32-isr-heartbeat/actions/workflows/ci.yml/badge.svg)](https://github.com/DwalloE/esp32-isr-heartbeat/actions/workflows/ci.yml)

> **TODO before pushing:** record the Wokwi simulation — LED blinking, serial log scrolling, a
> button press appearing in the count — save it as `docs/demo.gif`, and replace this block with
> `![demo](docs/demo.gif)`. Convert with
> `ffmpeg -i in.mp4 -vf "fps=12,scale=720:-1" -loop 0 docs/demo.gif`.

## Run it in your browser

No hardware needed.

**Simulator:** *(paste your Wokwi project link here after saving the project)*

**Locally:**

```bash
# Host tests - no toolchain beyond gcc and make. These are the interesting part.
make -C experiments verify     # asserts the missing-volatile bug, and its absence
make -C experiments disasm     # prints the poll loop as the compiler emitted it
make -C test                   # torn reads: control case must tear, seqlock must not

# Firmware - needs ESP-IDF v5.3+
idf.py set-target esp32
idf.py build && idf.py uf2     # -> build/uf2.bin, which Wokwi flashes
```

## What it does

A GPTimer alarm fires at 1 Hz. Its ISR publishes the timer count and raises a flag. The main loop
sees the flag, toggles the LED, and logs the beat with the microsecond timestamp and the button
count. A GPIO ISR on a pushbutton counts presses, debounced by timestamp.

```
I (296) heartbeat: up: led=gpio2 button=gpio4 tick=1Hz
I (1296) heartbeat: beat=1 t=1000002us buttons=0
I (2296) heartbeat: beat=2 t=2000013us buttons=0
I (3296) heartbeat: beat=3 t=3000016us buttons=1  <- press
```

## Wiring

| Signal | GPIO | Notes |
|---|---|---|
| LED | 2 | through 220 Ω to GND |
| Button | 4 | to GND, internal pull-up, falling-edge interrupt |
| Logic analyzer | D0 = GPIO2, D1 = GPIO4 | writes `heartbeat.vcd`, open in PulseView or GTKWave |

`diagram.json` is the Wokwi circuit; `wokwi.toml` points the simulator at the built image.

## What this demonstrates

- **`volatile`, as a consequence rather than a definition.** [`docs/volatile.md`](docs/volatile.md)
  contains the actual disassembly of the poll loop with and without it. Without: the loop compiles
  to `jmp` to its own address — one instruction, no load, no test, unreachable exit. Twenty timer
  ticks are delivered and none is observed. The repo asserts this in CI.
- **When `volatile` is necessary but not sufficient.** A 64-bit timestamp on a 32-bit core is two
  stores, and an interrupt can land between them. The measured tear rate on an unprotected split
  store is **4.03% of reads**; through the seqlock in [`main/seqlock.h`](main/seqlock.h) it is
  zero in three million. [`docs/torn-read.md`](docs/torn-read.md) has the reproduction, the fences
  and why each one is load-bearing, and the four alternatives with the case for each.
- **Why two shared variables in one file get different treatment.** `s_button_events` is a
  `volatile uint32_t` with no seqlock and that is correct; `s_last_tick_us` is 64-bit and needs
  one. The difference is width, and being able to say so is the point.
- **ISR discipline with a field failure attached to every rule.**
  [`docs/isr-discipline.md`](docs/isr-discipline.md) — including why `IRAM_ATTR` exists (an ISR in
  flash crashes when the flash cache is disabled, i.e. **during an OTA update**, which is a device
  that works for months and then bricks your whole fleet at once) and why `seqlock_u64_store` is
  `static inline` in a header so it inlines into IRAM instead of calling back into flash.
- **A test suite with a control case.** `test/test_seqlock.c` first proves the harness can *detect*
  tearing, then proves the seqlock prevents it. A concurrency test without a control passes on a
  broken implementation that happens never to collide.

## The bug gallery

| Bug | Symptom | Where |
|---|---|---|
| Shared flag not `volatile` | Loop compiles to `jmp` to self; ticks delivered and never seen | [`docs/volatile.md`](docs/volatile.md), reproduced in `experiments/` |
| 64-bit value shared without a protocol | Timestamp jumps exactly ±4.295 s (2³² µs); negative intervals downstream | [`docs/torn-read.md`](docs/torn-read.md), reproduced in `test/` |
| Missing release fence in the seqlock writer | Value visible before the odd counter; reader sees a half-written value with a valid counter | [`docs/torn-read.md`](docs/torn-read.md) |
| ISR not in IRAM | Crash only during OTA or flash erase | [`docs/isr-discipline.md`](docs/isr-discipline.md) |
| No yield in the main loop | Idle task starved, task watchdog at 5 s, silent reboot loop | comment in `main.c`, watchdog left enabled in `sdkconfig.defaults` |
| ISR returning `false` after waking a task | Works, but the wakeup waits up to one tick — mysterious latency, no crash | [`docs/isr-discipline.md`](docs/isr-discipline.md) §7 |
| Publishing `count_value` from an auto-reload alarm | Timestamp pinned near zero; the seqlock faithfully delivers a wrong value | [`docs/torn-read.md`](docs/torn-read.md) postscript — caught by the CI simulation's first run |

The last three are the interesting ones, because they do not crash.

## Why the reproduction lives in `experiments/` and not in the firmware

`main.c` has a `vTaskDelay()` in its poll loop, as any FreeRTOS application should. A call the
compiler cannot see through is a compiler barrier, so it reloads the flag anyway — which means the
firmware would **appear** to work with `volatile` removed, on that build, at that optimisation
level. Then someone inlines the delay, or the LTO pass improves in the next toolchain release, and
a device in the field stops responding. The variable was wrong the whole time.

That is why the demonstration is a standalone program with a deterministic pass/fail, and why it
runs on every commit. A bug that only appears under conditions you cannot reproduce is not
evidence; a bug asserted in CI is.

## How it is tested

| Test | Asserts | Runs |
|---|---|---|
| `make -C experiments verify` | Volatile-less build hangs (exit 2); volatile build completes 5 beats | CI, every commit |
| `make -C experiments disasm` | Publishes both poll loops as a build artifact | CI, every commit |
| `make -C test` | Round trip over 7 vectors; control tears; seqlock does not, in 3M reads | CI, every commit |
| `idf.py build && idf.py uf2` | Firmware compiles for ESP32 under ESP-IDF v5.3 | CI, every commit |
| Wokwi CI | Simulated ESP32 reaches `beat=3` within 20 s with no error-level log | CI, when `WOKWI_CLI_TOKEN` is set |

Host tests need only `gcc` and `make`. The simulation step needs a free
[Wokwi CI token](https://wokwi.com/dashboard/ci) and is skipped without one, so the suite still
runs on a fork.

## Honest limits

- A simulator has no analog behaviour. The 220 Ω resistor is decoration here; nothing models LED
  forward voltage, and nothing would notice if it were omitted.
- Nothing here measures current, so nothing here says anything about power. That is a separate
  project, and it needs datasheet arithmetic rather than a simulator.
- The torn read is reproduced on x86-64 by modelling the 32-bit core's split store explicitly.
  That is a faithful model of the mechanism, but it is a model — the numbers are the host's
  collision rate, not the ESP32's.
- Wokwi's virtual ESP32 does not reproduce cache-disable behaviour, so the `IRAM_ATTR` failure in
  `docs/isr-discipline.md` is documented from the hardware's behaviour and the ESP-IDF
  documentation, not demonstrated here. Removing `IRAM_ATTR` will not break the simulation. On
  hardware, during an OTA, it will.
- Contact bounce is simulated (`"bounce": "1"` in `diagram.json`), so the debounce logic is
  genuinely exercised — but simulated bounce is a clean model of a messy analog phenomenon.

## Why this project exists

First of eleven in a deliberately-sequenced embedded portfolio, moving from ESP-IDF (which I ship
professionally on 10,000+ fielded devices) into bare-metal STM32 register work, Zephyr on nRF52840,
and embedded Linux. Index: [`embedded-portfolio`](https://github.com/DwalloE/embedded-portfolio).

## Layout

```
main/main.c            the application. every shared variable carries its justification.
main/seqlock.h         64-bit ISR-to-task handoff. read the constraints before reusing it.
experiments/           the volatile bug, reproduced and disassembled. gcc only.
test/test_seqlock.c    torn-read control case and the seqlock under 3M concurrent reads.
docs/volatile.md       the disassembly, the three cases that need it, what it does not do.
docs/torn-read.md      the measurement, the fences, and the four alternatives.
docs/isr-discipline.md eight rules, each with the field failure that motivates it.
diagram.json           Wokwi circuit.
wokwi.toml             simulator config, including VCD capture.
```

MIT licensed.
