*This project has been created as part of the 42 curriculum by <your_login>.*

# Codexion

## Description

`Codexion` is a multithreaded C simulation of coders sharing a co-working hub with a
limited number of USB dongles. Each coder repeatedly cycles through **compiling**
(which requires holding two dongles at once, one per hand), **debugging** and
**refactoring**. Dongles sit between neighbouring coders on a circular table, so
acquiring both hands' dongles at the same time is the central resource-allocation
problem the project explores — the same shape as the classic dining philosophers
problem, but with an added dongle cooldown and a choice of arbitration policy
(`fifo` or `edf`).

The goal is a scheduler that never deadlocks, never starves a coder under `edf`,
detects burnout within 10 ms of it happening, and never corrupts its own log output —
all without a single global variable.

## Instructions

Build:

```sh
make
```

This produces the `codexion` binary at the project root. Other rules: `make clean`
(removes object files), `make fclean` (also removes the binary), `make re`.

Run:

```sh
./codexion number_of_coders time_to_burnout time_to_compile time_to_debug \
           time_to_refactor number_of_compiles_required dongle_cooldown scheduler
```

- All eight arguments are mandatory.
- The first, sixth and last arguments aside, every value is a duration in
  milliseconds.
- `scheduler` must be exactly `fifo` or `edf`.
- Any negative number, non-integer, or unknown scheduler name is rejected with a
  usage message and a non-zero exit code.

Example:

```sh
./codexion 5 2000 200 100 100 3 50 edf
```

Each state change a coder goes through is printed as
`timestamp_in_ms coder_id message`, e.g. `1502 3 is compiling`.

## Blocking cases handled

- **Deadlock (hold-and-wait) prevention** — a coder never holds one dongle while
  waiting for the other. Both dongles a coder needs are requested as a single
  ticket in [src/scheduler.c](src/scheduler.c); the scheduler only ever grants a
  ticket once *both* dongles are simultaneously free (`dongles_try_take_pair` in
  [src/dongle.c](src/dongle.c)). Since a coder can never be seen holding a partial
  pair, the classic "grab left fork, wait forever for right fork" cycle cannot
  occur, regardless of how many coders are running.
- **Starvation prevention** — every pending request lives in one priority queue
  ([src/queue.c](src/queue.c)) ordered by arrival order (`fifo`) or by
  `last_compile_start + time_to_burnout` (`edf`), with the arrival sequence number
  used as a deterministic tie-breaker. The monitor only ever grants dongles to the
  head of the queue; it never lets a later request jump ahead of one still waiting,
  so every coder is guaranteed to reach the front within a bounded number of
  compile/debug/refactor cycles.
- **Dongle cooldown** — releasing a dongle stamps it with `free_at = now +
  dongle_cooldown` ([src/dongle.c](src/dongle.c)); a dongle is only considered free
  again once that timestamp has passed, checked atomically together with its
  `taken` flag under the dongle's own mutex.
- **Precise burnout detection** — a dedicated monitor thread
  ([src/monitor.c](src/monitor.c)) re-checks every coder's deadline every 500 µs
  (`POLL_INTERVAL_US`), well under the 10 ms tolerance the subject requires.
- **Log serialization** — every printed line goes through `log_event`
  ([src/log.c](src/log.c)), which holds a single dedicated mutex for the duration
  of the `printf`, so two lines can never interleave.

## Thread synchronization mechanisms

- `pthread_mutex_t mutex` — one per dongle ([src/codexion.h](src/codexion.h)),
  guarding only that dongle's `taken` flag and `free_at` cooldown timestamp. When a
  coder needs its left *and* right dongle, both mutexes are locked together by
  `dongles_lock_pair` in a fixed order (comparing the two dongles' addresses) so
  that two coders can never lock them in opposite order and deadlock on the mutexes
  themselves.
- `pthread_mutex_t sched_mutex` (one per simulation) — guards the shared priority
  queue, the arrival counter, the stop flag, and the handful of coder fields the
  monitor needs to inspect (`last_compile_start`, `compiles_done`, `ready`). Every
  read of those fields from another thread goes through this lock, so there is no
  data race between a coder announcing "I just finished compiling" and the monitor
  reading that same timestamp to check for burnout.
- `pthread_cond_t cond` — one per coder, always used together with `sched_mutex`.
  A coder waiting for dongles (`request_dongles`) or sleeping through a compile,
  debug or refactor phase (`wait_ms`) blocks on its own condition variable instead
  of busy-polling; the monitor thread wakes a coder with `pthread_cond_signal` the
  instant it grants that coder's dongles, and with `pthread_cond_broadcast` to wake
  everyone at once when the simulation stops (burnout or completion).
- `pthread_mutex_t log_mutex` — kept separate from `sched_mutex` on purpose: a
  coder printing "is debugging" does not need to hold the scheduling lock at all,
  which keeps the monitor from ever waiting on I/O.

Race-prevention in practice: `try_grant` (called by the monitor while holding
`sched_mutex`) peeks the queue head and calls `dongles_try_take_pair`, which locks
both dongles, re-checks `taken`/`free_at` and flips `taken` to `1` in the same
critical section — there is no window between "checked free" and "marked taken"
for another thread to slip through. A coder becomes aware it was granted its
dongles only after it wakes up under `sched_mutex` and observes `ready == 1`
itself set by the monitor under that same lock, so the hand-off is race-free by
construction rather than by timing luck.

## Project structure

- `src/main.c` — argument parsing, simulation bootstrap, join and teardown.
- `src/args.c` — strict validation of the eight command-line arguments.
- `src/lab_init.c` / `src/lab_cleanup.c` — allocation, mutex/condvar setup, thread
  creation, and their exact mirror-image teardown.
- `src/queue.c` — the fixed-capacity binary heap used as the FIFO/EDF priority
  queue (no standard-library priority queue is used).
- `src/dongle.c` — per-dongle locking, atomic pair acquisition, cooldown release.
- `src/scheduler.c` — ties the queue and the dongles together: enqueue a request,
  grant the head of the queue when possible, wake everyone on stop.
- `src/coder.c` — a coder's compile → debug → refactor life cycle.
- `src/monitor.c` — burnout detection, completion detection, dongle granting.
- `src/clock_utils.c` — millisecond clock and interruptible timed sleep.
- `src/log.c` — serialized, timestamped event printing.

## Resources

- POSIX Threads Programming (a classic, thorough tutorial):
  https://hpc-tutorials.llnl.gov/posix/
- `pthread_cond_timedwait`, `pthread_mutex_t` and related man pages:
  https://man7.org/linux/man-pages/man7/pthreads.7.html
- Dining philosophers problem and Coffman's deadlock conditions (background
  reading on the classic problem this project is modeled after):
  https://en.wikipedia.org/wiki/Dining_philosophers_problem
- Earliest Deadline First scheduling:
  https://en.wikipedia.org/wiki/Earliest_deadline_first_scheduling
- Binary heap / priority queue implementation:
  https://en.wikipedia.org/wiki/Binary_heap

### AI usage disclosure

An AI assistant (Claude, in Claude Code) was used while building this project to:
read and summarize the subject PDF, review two peers' existing implementations of
the same subject for design ideas (in particular the idea of representing pending
dongle requests as entries in a single priority queue, and using condition
variables instead of busy-polling), and to draft the initial C source files and
this README from that understanding. Every synchronization decision described
above — the atomic two-dongle acquisition to eliminate hold-and-wait, the split
between per-dongle and global locking, the choice of condition variables paired
with `sched_mutex` — was reviewed, tested (including with `valgrind --leak-check`
and `valgrind --tool=helgrind` for data races) and is understood well enough to be
explained and modified during a live defence.
