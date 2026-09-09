/*
 * seqlock.h - handing a 64-bit value from an ISR to a task without tearing.
 *
 * The problem
 * -----------
 * The ESP32's Xtensa LX6 is a 32-bit core. A 64-bit store is therefore not one
 * instruction, it is two. If an interrupt fires between them, a reader in task
 * context sees the new half beside the old half: a value nobody ever wrote.
 * That is a torn read. In the field it looks like a timestamp that jumps
 * 4.295 seconds (2^32 microseconds) forward or back, on one device, once every
 * few hours, and it is unreproducible on the bench. `volatile` does not help:
 * it guarantees the compiler emits the two stores, and says nothing about what
 * happens between them.
 *
 * Why a seqlock rather than a critical section
 * -------------------------------------------
 * portENTER_CRITICAL() in the reader also works and is often the right answer.
 * It costs an interrupt-disable window in task context, which is a bad thing
 * to spend on a device that also has to service a radio. A seqlock makes the
 * writer unconditionally wait-free - it never blocks, never disables anything -
 * and pushes the cost onto the reader, which retries only on an actual
 * collision. For one ISR publishing and one task sampling, that is the right
 * trade. For a value the reader must have at a hard deadline, it is not.
 *
 * Why the value is stored as two explicit uint32_t halves
 * ------------------------------------------------------
 * Because it makes the non-atomicity visible in the source instead of leaving
 * it to the target's word size, and because it lets test/test_seqlock.c
 * exercise this exact code on a 64-bit host. If the value were a plain
 * uint64_t, x86-64 would store it in one atomic instruction, the host test
 * would pass for the wrong reason, and it would prove nothing about the ESP32.
 *
 * Constraints - read these before reusing this
 * -------------------------------------------
 * - EXACTLY ONE writer. Two writers need a lock between them; this provides
 *   none, and interleaved writers can leave the counter odd forever.
 * - The writer must always run to completion. An ISR does. A preemptible task
 *   at lower priority than a spinning reader does not, and would livelock it.
 * - Readers may spin. Bounded in practice by the length of the ISR, which is
 *   an argument for keeping the ISR short, not for tuning the reader.
 *
 * Note on strict correctness: `volatile` plus explicit fences is the idiom you
 * will meet in most firmware and in most interviews, and is what is written
 * here. The formally correct C11 spelling uses __atomic_load_n /
 * __atomic_store_n with acquire/release ordering on non-volatile fields;
 * seqlocks are, by the letter of the memory model, a data race. See
 * docs/torn-read.md.
 */
#ifndef SEQLOCK_H
#define SEQLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t seq;   /* even: stable.  odd: a write is in progress. */
    volatile uint32_t hi;    /* bits 63..32 */
    volatile uint32_t lo;    /* bits 31..0  */
} seqlock_u64_t;

#define SEQLOCK_U64_INIT { .seq = 0, .hi = 0, .lo = 0 }

/*
 * Publish a new value. ISR context, single writer only. Wait-free.
 *
 * The odd/even discipline is the entire mechanism. A reader can tell that a
 * write straddled its read, because the counter changed; and that a write was
 * in flight when it started, because the counter was odd.
 */
static inline void seqlock_u64_store(seqlock_u64_t *s, uint64_t v)
{
    const uint32_t seq = s->seq + 1u;              /* -> odd */
    s->seq = seq;
    __atomic_thread_fence(__ATOMIC_RELEASE);       /* seq before value */

    s->hi = (uint32_t)(v >> 32);
    s->lo = (uint32_t)(v & 0xFFFFFFFFu);

    __atomic_thread_fence(__ATOMIC_RELEASE);       /* value before seq */
    s->seq = seq + 1u;                             /* -> even */
}

/*
 * Read the value, retrying until a read completes with no concurrent write.
 * Task context. Safe for any number of readers.
 */
static inline uint64_t seqlock_u64_load(const seqlock_u64_t *s)
{
    for (;;) {
        const uint32_t before = s->seq;
        if (before & 1u) {
            continue;                              /* writer mid-flight */
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        const uint32_t hi = s->hi;
        const uint32_t lo = s->lo;

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (s->seq == before) {
            return ((uint64_t)hi << 32) | (uint64_t)lo;
        }
        /* A write landed on top of us. Nothing is wrong; try again. */
    }
}

#endif /* SEQLOCK_H */
