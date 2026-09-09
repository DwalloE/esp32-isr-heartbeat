/*
 * volatile_demo.c - the loop that never exits.
 *
 * Reproduces, on your laptop, the exact failure that a missing `volatile`
 * causes on a microcontroller. Compile the same source twice:
 *
 *     -O2                  -> the loop never exits.       exit code 2
 *     -O2 -DUSE_VOLATILE   -> the loop exits every tick.  exit code 0
 *
 * A POSIX interval timer stands in for the hardware timer interrupt. It is a
 * faithful model for the purpose at hand: the signal handler runs
 * asynchronously, so the compiler cannot prove when - or whether - the flag
 * changes, which is the same information deficit it has about an ISR.
 *
 * The point is not that `volatile` "stops optimisation". The point is the
 * specific optimisation: the compiler proves the loop body cannot write the
 * flag, hoists the load out of the loop, and tests a register that nothing on
 * the machine can ever change.
 *
 *     make            # build both variants and dump the disassembly
 *     make verify     # assert the bug in one and its absence in the other
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(USE_VOLATILE)
#define SHARED volatile
#else
#define SHARED /* nothing. this is the bug. */
#endif

/* The flag an interrupt sets and a main loop polls. */
static SHARED int tick_pending = 0;

/* Escape hatch so a hung build fails the test instead of hanging CI. */
static volatile sig_atomic_t deliveries = 0;

static void on_tick(int sig)
{
    (void)sig;

    tick_pending = 1;

    if (++deliveries >= 20) {
        /* Twenty ticks delivered and the main loop has not noticed one of
         * them. Only write(2) and _exit(2) are async-signal-safe; printf is
         * not, which is itself an ISR-discipline lesson. */
        static const char msg[] =
            "STUCK: 20 ticks delivered, wait_for_tick() never returned\n";
        ssize_t n = write(STDERR_FILENO, msg, sizeof msg - 1);
        (void)n;
        _exit(2);
    }
}

/*
 * Kept in its own function with external linkage so that `objdump -d` shows it
 * as a single labelled block. This function is the artifact.
 */
long wait_for_tick(void)
{
    long spins = 0;

    while (tick_pending == 0) {
        spins++;
    }
    tick_pending = 0;

    return spins;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_tick;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGALRM, &sa, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    /* 10 ms period: 20 deliveries is a 200 ms verdict. */
    const struct itimerval it = {
        .it_interval = { .tv_sec = 0, .tv_usec = 10000 },
        .it_value    = { .tv_sec = 0, .tv_usec = 10000 },
    };
    if (setitimer(ITIMER_REAL, &it, NULL) != 0) {
        perror("setitimer");
        return 1;
    }

#if defined(USE_VOLATILE)
    printf("build: volatile\n");
#else
    printf("build: NOT volatile\n");
#endif
    fflush(stdout);

    for (int beat = 1; beat <= 5; beat++) {
        long spins = wait_for_tick();
        printf("beat=%d spins=%ld\n", beat, spins);
        fflush(stdout);
    }

    printf("OK: 5 beats observed\n");
    return 0;
}
