# The torn read: when `volatile` is necessary but not sufficient

`s_tick_pending` needed `volatile` and nothing more. `s_last_tick_us` needs `volatile` and a
protocol. The difference is width, and on a 32-bit core width is a correctness property.

## The failure

The ESP32's Xtensa LX6 is a 32-bit machine. A 64-bit store is two instructions:

```
    store high word
    <-- an interrupt can land here
    store low word
```

`volatile` guarantees both stores are emitted. It guarantees nothing about what happens between
them. If the ISR publishes a new 64-bit timestamp while a task is halfway through reading the old
one, the task assembles a value out of one new half and one old half — a number that was never
written by anybody.

The signature is unmistakable once you know it: a microsecond timestamp that jumps
**2³² µs = 4.295 seconds** forward or backward, exactly. Downstream, that is a negative interval,
a division by a negative duration, a rate of −0.24 Hz, and a data point your carbon-credit verifier
rejects. Once every few hours. On one device. Never on the bench.

## Reproducing it on a 64-bit laptop

An x86-64 store of an aligned `uint64_t` is a single atomic instruction, so a naive test on your
laptop passes and proves nothing about the target. `test/test_seqlock.c` therefore models the
target honestly: the value is stored as two explicit `uint32_t` halves — which is precisely what
the 32-bit core does for you whether you asked or not.

```c
typedef struct { volatile uint32_t hi; volatile uint32_t lo; } split_u64_t;

static inline void split_store(split_u64_t *s, uint64_t v)
{
    s->hi = (uint32_t)(v >> 32);
    /* on the target, an interrupt can land precisely here */
    s->lo = (uint32_t)(v & 0xFFFFFFFFu);
}
```

The detector needs no timing analysis: the writer publishes only values whose halves are equal,
`v = (n << 32) | n`. Any observed value with `hi != lo` was therefore never published.

## Measured, on a 2-core host

```
== case 0: single-threaded round trip ==
PASS roundtrip: 7 vectors, counter even

== case 1: unprotected split store (the control) ==
   this case MUST tear, or the harness cannot detect tearing
   reads=3000000 tears=120864 (4.0288%) worst skew=26144
PASS case 1: torn reads reproduced

== case 2: seqlock ==
   reads=3000000 tears=0
PASS case 2: zero torn reads in 3000000 reads

ALL PASS
```

**Four percent** of reads were garbage. Case 1 exists because a test suite that only checked case 2
would be worthless — it would pass on a broken seqlock that simply never collided. The control
proves the harness can see the fault it is testing for. Any test of a concurrency fix needs one.

## The fix, and why this one

`main/seqlock.h`. The writer brackets its update with an odd/even counter; the reader retries if
the counter moved or was odd.

```c
static inline void seqlock_u64_store(seqlock_u64_t *s, uint64_t v)
{
    const uint32_t seq = s->seq + 1u;              /* -> odd */
    s->seq = seq;
    __atomic_thread_fence(__ATOMIC_RELEASE);       /* seq lands before value */

    s->hi = (uint32_t)(v >> 32);
    s->lo = (uint32_t)(v & 0xFFFFFFFFu);

    __atomic_thread_fence(__ATOMIC_RELEASE);       /* value lands before seq */
    s->seq = seq + 1u;                             /* -> even */
}
```

The two fences are not decoration. Without the first, the compiler or the store buffer may make the
value visible before the odd counter, and a reader sees a half-written value with an even counter —
the exact bug, now with extra code. Without the second, the counter may go even before the value
lands. `volatile` will not order these for you; that is what [`volatile.md`](volatile.md) means by
"not a barrier."

The reader:

```c
for (;;) {
    const uint32_t before = s->seq;
    if (before & 1u) continue;                /* writer mid-flight */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint32_t hi = s->hi;
    const uint32_t lo = s->lo;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (s->seq == before) return ((uint64_t)hi << 32) | lo;
}
```

### The four alternatives, and when each is right

| Approach | Cost | Use it when |
|---|---|---|
| `portENTER_CRITICAL` in the reader | Disables interrupts in task context for the read | The reader has a hard deadline and must not retry. The honest default; often correct. |
| **Seqlock** (this) | Writer wait-free; reader may retry | An ISR publishes and a task samples. The ISR must never be delayed and the reader can tolerate a retry. |
| `_Atomic uint64_t` / `__atomic_load_n` | GCC emits a lock or a libatomic call for 64-bit on a 32-bit target | You want correctness over predictability and have measured the cost. Check the disassembly — a hidden lock inside an ISR is a deadlock. |
| Ring buffer / queue | Memory, and a copy | You need the *history*, not the latest value. Which is usually what you actually need — see portfolio project 02. |

### What this seqlock will not survive

- **Two writers.** It has no mutual exclusion between writers. Interleaved writers can leave the
  counter odd permanently and hang every reader.
- **A writer that can be preempted indefinitely.** An ISR always completes. A low-priority task
  writing while a high-priority task spins in the reader is a livelock on a single core.
- **Being read from an ISR while a task writes.** The roles are not symmetric. Writer is the ISR.

## Strict correctness, since an interviewer may push

By the letter of the C11 memory model, this is a data race: `volatile` accesses are not atomic
accesses, and a race is undefined behaviour regardless of what the generated code happens to do.
The formally clean version drops `volatile` and uses `__atomic_load_n`/`__atomic_store_n` with
acquire/release ordering on the counter and relaxed ordering on the halves. Real firmware is full
of the `volatile`-plus-fence spelling — it is what the Linux kernel's own seqlock grew out of, and
it is what you will be shown in an interview — so both are worth being able to write. If asked
which is correct: the atomics version. If asked why the other is everywhere: it predates C11 and
it works on every compiler anyone ships.

Related: `s_button_events` in `main.c` is a `volatile uint32_t` with **no** seqlock, and that is
correct — one naturally-aligned 32-bit word on a 32-bit core cannot tear. The `++` is safe only
because the ISR is the sole writer. Being able to say why two shared variables in the same file
get different treatment is the point of the whole exercise.

## Postscript: the bug the simulator caught in the same variable

The first simulated CI run printed this:

```
I (1296) heartbeat: beat=1 t=3us buttons=0
I (2296) heartbeat: beat=2 t=2us buttons=0
```

The seqlock delivered every one of those values perfectly intact — and every one of them was
wrong. The alarm is configured with `auto_reload_on_alarm`, so the hardware sets the counter back
to `reload_count` (0) *before* the callback runs. `edata->count_value` at that point is not "the
time of this tick"; it is the dispatch latency since the reload, a handful of microseconds. The
published "timestamp" was pinned near zero forever, monotonic in nothing, and no concurrency
mechanism in the world was going to fix it, because it was the correct handoff of an incorrect
value.

The fix (in `on_timer_alarm`) accumulates the nominal period per alarm and adds the measured
latency back on top. The reason it is worth a postscript in this document: a torn read and a
wrong-by-construction value produce similar-looking downstream garbage, and the reflex after
writing a seqlock is to suspect the handoff. Check what the value *is* before reasoning about how
it travels — and assert the actual output in CI, which is what caught this one on the first run
the firmware ever made outside a compiler.
