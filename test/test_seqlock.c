/*
 * test_seqlock.c - proves the torn read is real, and that the seqlock fixes it.
 *
 * Two cases, run back to back:
 *
 *   Case 1  unprotected split store  -> tearing MUST be observed.
 *   Case 2  seqlock (main/seqlock.h) -> tearing MUST NOT be observed.
 *
 * A test that only checked case 2 would be worthless: it would pass on a
 * broken implementation that happened never to collide. Case 1 is the control.
 * It establishes that this harness can detect the fault it is testing for.
 *
 * The detector: the writer publishes values whose two halves are equal,
 * v = (n << 32) | n. Any value a reader observes with hi != lo is therefore a
 * value that was never published - a torn read - and needs no timing analysis
 * to identify.
 *
 *     make          # build and run
 *     make clean
 */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../main/seqlock.h"

/* Enough iterations that a real race shows up every run, small enough that the
 * whole suite stays under a second in CI. */
#define ITERATIONS 3000000u

/* --------------------------------------------------------------------- *
 * Case 1: the bug. A 64-bit value stored as two halves with no protocol -
 * which is exactly what a 32-bit core does for you, whether you meant it or
 * not.
 * --------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t hi;
    volatile uint32_t lo;
} split_u64_t;

static split_u64_t g_unsafe = { 0, 0 };
static seqlock_u64_t g_safe = SEQLOCK_U64_INIT;
static volatile bool g_run = true;

static inline void split_store(split_u64_t *s, uint64_t v)
{
    s->hi = (uint32_t)(v >> 32);
    /* On the target, an interrupt can land precisely here. */
    s->lo = (uint32_t)(v & 0xFFFFFFFFu);
}

static inline uint64_t split_load(const split_u64_t *s)
{
    const uint32_t hi = s->hi;
    const uint32_t lo = s->lo;
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* --------------------------------------------------------------------- *
 * Threads. The writer stands in for the ISR; the reader for the main loop.
 * --------------------------------------------------------------------- */

static void *writer_unsafe(void *arg)
{
    (void)arg;
    for (uint32_t n = 1; g_run; n++) {
        split_store(&g_unsafe, ((uint64_t)n << 32) | (uint64_t)n);
    }
    return NULL;
}

static void *writer_safe(void *arg)
{
    (void)arg;
    for (uint32_t n = 1; g_run; n++) {
        seqlock_u64_store(&g_safe, ((uint64_t)n << 32) | (uint64_t)n);
    }
    return NULL;
}

typedef struct {
    uint64_t reads;
    uint64_t tears;
    uint64_t worst_skew;   /* largest |hi - lo| seen, in raw units */
} result_t;

static void *reader_unsafe(void *arg)
{
    result_t *r = arg;
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        const uint64_t v = split_load(&g_unsafe);
        const uint32_t hi = (uint32_t)(v >> 32);
        const uint32_t lo = (uint32_t)(v & 0xFFFFFFFFu);
        r->reads++;
        if (hi != lo) {
            r->tears++;
            const uint32_t skew = (hi > lo) ? (hi - lo) : (lo - hi);
            if (skew > r->worst_skew) {
                r->worst_skew = skew;
            }
        }
    }
    return NULL;
}

static void *reader_safe(void *arg)
{
    result_t *r = arg;
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        const uint64_t v = seqlock_u64_load(&g_safe);
        const uint32_t hi = (uint32_t)(v >> 32);
        const uint32_t lo = (uint32_t)(v & 0xFFFFFFFFu);
        r->reads++;
        if (hi != lo) {
            r->tears++;
        }
    }
    return NULL;
}

static result_t run_case(void *(*writer)(void *), void *(*reader)(void *))
{
    result_t r = { 0, 0, 0 };
    pthread_t wt, rt;

    g_run = true;
    if (pthread_create(&wt, NULL, writer, NULL) != 0) {
        perror("pthread_create writer");
        exit(1);
    }
    if (pthread_create(&rt, NULL, reader, &r) != 0) {
        perror("pthread_create reader");
        exit(1);
    }
    pthread_join(rt, NULL);
    g_run = false;
    pthread_join(wt, NULL);

    return r;
}

/* --------------------------------------------------------------------- *
 * Single-threaded sanity: the protocol must be lossless in the trivial case.
 * --------------------------------------------------------------------- */
static bool test_roundtrip(void)
{
    static const uint64_t vectors[] = {
        0ULL,
        1ULL,
        0xFFFFFFFFULL,              /* all of lo, none of hi */
        0x100000000ULL,             /* the boundary */
        0xFFFFFFFF00000000ULL,      /* all of hi, none of lo */
        0xDEADBEEFCAFEBABEULL,
        UINT64_MAX,
    };

    seqlock_u64_t s = SEQLOCK_U64_INIT;
    for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
        seqlock_u64_store(&s, vectors[i]);
        const uint64_t got = seqlock_u64_load(&s);
        if (got != vectors[i]) {
            printf("FAIL roundtrip: stored %016" PRIx64 " read %016" PRIx64 "\n",
                   vectors[i], got);
            return false;
        }
    }
    /* The counter must be even - no write left in flight. */
    if (s.seq & 1u) {
        printf("FAIL roundtrip: seq left odd (%" PRIu32 ")\n", s.seq);
        return false;
    }
    printf("PASS roundtrip: 7 vectors, counter even\n");
    return true;
}

int main(void)
{
    bool ok = true;

    printf("== case 0: single-threaded round trip ==\n");
    ok = test_roundtrip() && ok;

    printf("\n== case 1: unprotected split store (the control) ==\n");
    printf("   this case MUST tear, or the harness cannot detect tearing\n");
    const result_t bad = run_case(writer_unsafe, reader_unsafe);
    printf("   reads=%" PRIu64 " tears=%" PRIu64 " (%.4f%%) worst skew=%" PRIu64 "\n",
           bad.reads, bad.tears,
           100.0 * (double)bad.tears / (double)bad.reads, bad.worst_skew);
    if (bad.tears == 0) {
        printf("FAIL case 1: no tearing observed. The control did not fire, so\n"
               "     case 2 proves nothing. Try more iterations, or a machine\n"
               "     with more than one core.\n");
        ok = false;
    } else {
        printf("PASS case 1: torn reads reproduced\n");
    }

    printf("\n== case 2: seqlock ==\n");
    const result_t good = run_case(writer_safe, reader_safe);
    printf("   reads=%" PRIu64 " tears=%" PRIu64 "\n", good.reads, good.tears);
    if (good.tears != 0) {
        printf("FAIL case 2: %" PRIu64 " torn reads through the seqlock\n",
               good.tears);
        ok = false;
    } else {
        printf("PASS case 2: zero torn reads in %" PRIu64 " reads\n", good.reads);
    }

    printf("\n%s\n", ok ? "ALL PASS" : "FAILURES ABOVE");
    return ok ? 0 : 1;
}
