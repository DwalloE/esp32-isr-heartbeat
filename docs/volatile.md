# `volatile`: the loop that never exits

Not "it tells the compiler not to optimise." That sentence is true and useless. Here is the
specific optimisation, the specific instruction it emits, and the specific field failure.

## The code

`experiments/volatile_demo.c`, compiled twice from one source:

```c
static SHARED int tick_pending = 0;      /* SHARED = volatile, or nothing */

long wait_for_tick(void)
{
    long spins = 0;
    while (tick_pending == 0) {
        spins++;
    }
    tick_pending = 0;
    return spins;
}
```

A POSIX interval timer sets `tick_pending` from a signal handler. A signal handler is a fair model
of an ISR for this purpose: it runs asynchronously, so the compiler has exactly the same
information deficit about it that it has about a hardware interrupt — namely, none.

## What the compiler emits

Reproduce with `make -C experiments`. Output below is `gcc 13.3.0`, `-O2`, x86-64.

**Without `volatile`:**

```
0000000000001410 <wait_for_tick>:
    1410:	endbr64
    1414:	mov    0x2c06(%rip),%eax        # 4020 <tick_pending>
    141a:	test   %eax,%eax
    141c:	jne    1420 <wait_for_tick+0x10>
    141e:	jmp    141e <wait_for_tick+0xe>      <-- jumps to itself, forever
    1420:	movl   $0x0,0x2bf6(%rip)        # 4020 <tick_pending>
    142a:	xor    %eax,%eax
    142c:	ret
```

Read line `141e`. The loop is one instruction and that instruction is `jmp` to its own address.
There is no load. There is no test. The compiler read `tick_pending` **once**, at `1414`, decided
that nothing in the loop body could change it, and replaced the entire loop with an
unconditional branch to itself. No interrupt, no signal, no other core can affect a `jmp` to self.
The function will never return.

Note also that `spins++` has vanished entirely — its result is unobservable once the loop cannot
exit, so it was deleted. The compiler is not being perverse. It is being correct: under the C
abstract machine, with no `volatile` and no synchronisation, nothing in this program can change
`tick_pending` during the loop, so this transformation preserves the program's meaning exactly.

**With `volatile`:**

```
0000000000001420 <wait_for_tick>:
    1420:	endbr64
    1424:	mov    0x2bf6(%rip),%eax        # 4020 <tick_pending>
    142a:	test   %eax,%eax
    142c:	mov    $0x0,%eax
    1431:	jne    1446 <wait_for_tick+0x26>
    1433:	nopl   0x0(%rax,%rax,1)
    1438:	mov    0x2be2(%rip),%edx        # 4020 <tick_pending>   <-- reload
    143e:	add    $0x1,%rax
    1442:	test   %edx,%edx
    1444:	je     1438 <wait_for_tick+0x18>
    1446:	movl   $0x0,0x2bd0(%rip)        # 4020 <tick_pending>
    1450:	ret
```

Line `1438` is a fresh load from memory on every iteration, and `1444` branches back to it.
`spins++` survives at `143e`. That is the whole difference: `volatile` means *every access in the
source is an access in the object code*.

## What it does at runtime

```
== expecting the volatile-less build to hang and be killed ==
build: NOT volatile
STUCK: 20 ticks delivered, wait_for_tick() never returned
PASS: missing volatile reproduced (exit 2)
== expecting the volatile build to run to completion ==
build: volatile
beat=1 spins=32211239
beat=2 spins=32033942
...
PASS: volatile build completed 5 beats
```

Twenty timer ticks were delivered to the volatile-less build. It noticed none of them.

## The three cases that need it

1. **A variable shared with an interrupt handler.** This one. The compiler cannot see the ISR.
2. **A memory-mapped hardware register.** Reading it twice is not redundant — the second read may
   return a different value, and for many status registers the read itself has a side effect
   (clearing a flag). Without `volatile` the compiler will happily delete one of the reads, or
   reorder a write to a data register ahead of the write to the control register that arms it.
3. **A variable modified across a `setjmp`/`longjmp`** boundary.

Shared-with-another-*thread* is deliberately not on that list. See below.

## What it does not do

`volatile` is not atomicity, and it is not a memory barrier.

- **Not atomicity.** `count++` on a `volatile` is still load, add, store. Two writers still lose
  updates. And a `volatile uint64_t` on a 32-bit core is still two stores, which is the subject
  of [`torn-read.md`](torn-read.md).
- **Not a barrier.** It orders accesses *to that object* as written, and says nothing about the
  ordering of other objects around it, and nothing about what the CPU's store buffer or a second
  core's cache does. On a dual-core ESP32 that distinction is real.
- **Therefore not a substitute for a lock or an atomic in multithreaded code.** `volatile` is for
  "this memory can change beneath me"; C11 atomics and `__atomic_*` builtins are for "these
  accesses must be ordered and indivisible with respect to another agent." Using `volatile` where
  you needed an atomic is one of the most common bugs in embedded C, and it survives testing
  because it usually works.

## Why this ships

The bug is hard to reproduce on purpose and easy to introduce by accident, because almost anything
in the loop body masks it. A function call the compiler cannot see through is a barrier, so it
reloads the flag anyway:

```c
while (!tick_pending) {
    vTaskDelay(1);          /* opaque call -> compiler must reload -> bug hidden */
}
```

This is why `main/main.c` — which has a `vTaskDelay()` in its poll loop, as any sane FreeRTOS
application does — would *appear* to work with the `volatile` removed, on that build, at that
optimisation level. Then someone inlines the delay, or bumps `-O2` to `-O3`, or the LTO pass gets
better in the next toolchain release, and a device in the field stops responding. The variable was
wrong the whole time.

## Being asked about this

The question is usually "what does `volatile` do?" The answer that lands is the disassembly:
*the compiler hoists the load out of the loop and the loop becomes `jmp` to itself.* Then the
three cases. Then, unprompted, what it does **not** give you — atomicity or ordering — because
that is the follow-up, and getting there first is the difference between having read about
`volatile` and having debugged it.
