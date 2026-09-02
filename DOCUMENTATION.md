# Codexion — Full Technical Walkthrough

This document explains **why** Codexion is built the way it is, and then walks
through **every source file, function, and line** in the order the program
actually executes them — from the first instruction in `main()` to the last
`free()` before the process exits.

It is meant to be read once top to bottom before a defence, and then used as a
reference (`Ctrl+F` a function name) while re-reading the code side by side.

Table of contents:

1. [Theory: what problem is this, and why is it hard](#1-theory-what-problem-is-this-and-why-is-it-hard)
2. [The shared vocabulary: every struct, field by field](#2-the-shared-vocabulary-every-struct-field-by-field)
3. [Program start-up: `main.c`](#3-program-start-up-mainc)
4. [Turning `argv` into a `t_lab`: `args.c`](#4-turning-argv-into-a-t_lab-argsc)
5. [Building the world: `lab_init.c`](#5-building-the-world-lab_initc)
6. [Two kinds of thread run forever](#6-two-kinds-of-thread-run-forever)
   - 6.1 [`coder.c` — a coder's life cycle](#61-coderc--a-coders-life-cycle)
   - 6.2 [`monitor.c` — the referee](#62-monitorc--the-referee)
   - 6.3 [`scheduler.c` — the glue between coder and monitor](#63-schedulerc--the-glue-between-coder-and-monitor)
   - 6.4 [`dongle.c` — the only place a dongle's state changes](#64-donglec--the-only-place-a-dongles-state-changes)
   - 6.5 [`queue.c` — the priority queue (binary heap)](#65-queuec--the-priority-queue-binary-heap)
   - 6.6 [`clock_utils.c` and `log.c` — small utilities used everywhere](#66-clock_utilsc-and-logc--small-utilities-used-everywhere)
7. [Shutting down: `lab_cleanup.c`](#7-shutting-down-lab_cleanupc)
8. [A worked example: tracing one coder from birth to its second compile](#8-a-worked-example-tracing-one-coder-from-birth-to-its-second-compile)
9. [Why it cannot deadlock, cannot starve, and cannot race](#9-why-it-cannot-deadlock-cannot-starve-and-cannot-race)

---

## 1. Theory: what problem is this, and why is it hard

### 1.1 The scenario, mapped onto classic concurrency theory

The subject describes coders sitting in a circle around a shared table. There are
as many USB dongles as coders, one dongle placed between every pair of
neighbouring coders. To compile, a coder needs **both** the dongle on their left
and the dongle on their right at the same time. This is, field for field, the
**dining philosophers problem** (Dijkstra, 1965) with forks renamed to dongles and
eating renamed to compiling. Every well-known failure mode of that problem is
therefore a failure mode Codexion must avoid:

- **Deadlock** — every coder picks up their left dongle and then blocks forever
  waiting for their right dongle, which their neighbour is also holding while
  waiting on *their* neighbour, all the way around the circle.
- **Starvation** — a coder is technically able to get its dongles eventually, but
  in practice keeps losing the race to faster or luckier neighbours forever.
- **Burnout (the project's specific failure mode)** — a coder that starves for
  longer than `time_to_burnout` milliseconds since its last compile (or since the
  simulation began, if it never compiled) is defined to have "burned out", and the
  whole simulation must stop and say so within 10 ms of it happening.

Codexion adds two ingredients the classic problem does not have:

- A **cooldown**: a dongle cannot be re-taken for `dongle_cooldown` ms after being
  released, even if nobody else wants it. This exists purely to make the
  scheduling problem harder to get right — it means "the dongle is free" is not
  simply "nobody is holding it".
- A **choice of arbitration policy** (`fifo` or `edf`) for who gets a contested
  dongle next.

### 1.2 Coffman's four conditions and how each is broken here

A deadlock can only happen if **all four** of these hold at once (Coffman et al.,
1971). Codexion is built to make one of them structurally impossible, which is a
stronger guarantee than the usual "lock forks in a fixed order" trick (which only
prevents deadlock, and does nothing for fairness):

| Condition | Would look like | Broken how |
|---|---|---|
| Mutual exclusion | A dongle can only be held by one coder at a time | **Not broken** — this one is required by the subject (`pthread_mutex_t` per dongle) and is not the one we attack. |
| Hold and wait | A coder holds one dongle while waiting for the second | **Broken.** A coder never receives a single dongle on its own. `dongles_try_take_pair()` in `dongle.c` only ever succeeds when *both* of a coder's dongles are free in the same instant, under the same lock. There is no code path anywhere in this project that sets one dongle's `taken` flag without also setting its pair's. |
| No preemption | A dongle cannot be forcibly taken from a coder | Not broken, and does not need to be — with hold-and-wait impossible, preemption is never needed to recover from a stuck state. |
| Circular wait | Coder A waits on a resource held by B, which waits on one held by C, ... , which waits on one held by A | **Broken as a consequence of breaking hold-and-wait.** A "wait" in this project only ever means "sitting in the priority queue with zero dongles held", never "holding one resource while queued for another". A cycle of waiters who each hold something the next one needs cannot form if nobody in the cycle holds anything.

So Codexion does not use the traditional "always pick up the lower-numbered fork
first" deadlock-avoidance trick at the *coder* level at all. It uses it only at a
much smaller scale — ordering two `pthread_mutex_t` locks by address inside
`dongles_lock_pair()` — to stop two *dongles'* own mutexes from deadlocking each
other, which is a different and much simpler problem (see §6.4).

### 1.3 Fairness: FIFO vs EDF, and why a deadline is `last_compile_start + time_to_burnout`

Every coder that cannot immediately get both of its dongles is turned into one
**ticket** (`t_ticket`) sitting in exactly one priority queue shared by the whole
simulation (`t_lab.queue`). The two scheduler modes only change how that queue is
ordered:

- **`fifo`** — tickets are served in the order they arrived. Ties are impossible
  because the arrival number is a strictly increasing counter (`lab->next_arrival`).
- **`edf` (Earliest Deadline First)** — the ticket whose coder is closest to
  burning out is served first. "Closest to burning out" is computed once, at the
  moment the ticket is created, as `deadline = last_compile_start +
  time_to_burnout` (or `time_to_burnout` alone if the coder has never compiled
  yet, i.e. the reference point is simulation start). This is exactly the formula
  the subject specifies. Ties on `deadline` fall back to arrival order, which is
  unique, so ordering is always fully deterministic — never left to chance.

Either way, the queue is a **strict priority order**: the scheduler (`try_grant()`
in `scheduler.c`) will *only* grant dongles to whoever is currently at the front,
and stops looking the moment the front of the queue cannot be served, rather than
skipping ahead to serve someone further back who happens to be luckier. That
single rule is what makes both policies fair: nobody can be technically eligible
and still be repeatedly jumped by later arrivals.

### 1.4 Cooldown, burnout precision, and log serialization as separate concerns

These three requirements are handled independently, each by one small, focused
piece of code, rather than being tangled into the scheduling logic:

- **Cooldown** is just a timestamp (`t_dongle.free_at`) that release sets and
  acquisition checks — see §6.4.
- **Burnout precision** (must be detected and logged within 10 ms) is handled by
  giving the simulation one dedicated monitor thread that re-checks every coder
  roughly every half a millisecond (`POLL_INTERVAL_US`, `monitor.c`) — twenty
  times tighter than the required tolerance.
- **Log serialization** is one `printf` wrapped in one mutex (`log.c`), used by
  every thread, with the mutex held for the shortest possible time (just the
  `printf` call, not the surrounding logic).

---

## 2. The shared vocabulary: every struct, field by field

All of this lives in [`src/codexion.h`](src/codexion.h). There are no global
variables anywhere in the project (the subject forbids them); every function that
needs shared state receives a pointer to it as an argument, ultimately tracing
back to the one `t_lab` that `main()` keeps on its own stack.

### 2.1 `t_scheduler` (line 13-17)

```c
typedef enum e_scheduler
{
	ARBITRATION_FIFO,
	ARBITRATION_EDF
}	t_scheduler;
```

Just the two valid values of the `scheduler` command-line argument, decoded once
in `args.c`. The names are deliberately *not* `SCHED_FIFO` — that identifier is
already a macro defined by `<sched.h>` (a POSIX scheduling-policy constant
unrelated to this project), and `<pthread.h>` pulls `<sched.h>` in transitively.
Using it here caused a real compiler error the first time this file was written
(`expected identifier before numeric constant`); `ARBITRATION_FIFO` /
`ARBITRATION_EDF` sidesteps the clash entirely.

### 2.2 `t_dongle` (line 19-24)

```c
typedef struct s_dongle
{
	pthread_mutex_t	mutex;
	int				taken;
	long			free_at;
}	t_dongle;
```

- `mutex` — protects **only the other two fields of this same struct**. Every
  read or write of `taken` or `free_at`, from any thread, happens with this
  exact mutex held (via `dongles_lock_pair()` / `dongles_unlock_pair()`, §6.4).
  Nothing outside `dongle.c` ever touches these two fields directly.
- `taken` — `1` while some coder currently holds this dongle as part of a
  compile, `0` otherwise.
- `free_at` — the earliest timestamp (in the simulation's relative millisecond
  clock, see §6.6) at which this dongle is allowed to be granted again. Set to
  `now + dongle_cooldown` every time the dongle is released; a fresh dongle
  starts at `0`, which is always in the past, so it is immediately available.

### 2.3 `t_ticket` (line 39-44)

```c
typedef struct s_ticket
{
	t_coder	*coder;
	long	arrival;
	long	deadline;
}	t_ticket;
```

One entry in the priority queue. It exists only while a coder is waiting for
dongles; it is a plain value (not separately allocated — the whole array of
tickets is allocated once, see §2.4), copied by value when the heap moves entries
around.

- `coder` — which coder this ticket belongs to, so the scheduler knows whose
  dongles to check and whose condition variable to signal.
- `arrival` — the value of `lab->next_arrival` at the moment this ticket was
  created; used directly as the FIFO order and as the tie-breaker for EDF.
- `deadline` — this coder's burnout deadline at the moment the ticket was
  created, used as the EDF order.

### 2.4 `t_queue` (line 46-52)

```c
typedef struct s_queue
{
	t_ticket	*items;
	int			count;
	int			capacity;
	t_scheduler	mode;
}	t_queue;
```

A textbook array-backed **binary heap**, sized once and never resized.

- `items` — a single `malloc`'d array, `capacity` slots.
- `count` — how many of those slots currently hold a live ticket (`0` to
  `capacity`).
- `capacity` — fixed at `number_of_coders` when the queue is created. This bound
  is exact, not a guess: a coder can have **at most one** outstanding ticket at
  any time (it only calls `request_dongles()` again after its previous ticket was
  popped), so the queue can never need to hold more tickets than there are
  coders. No resizing logic is needed anywhere.
- `mode` — which of the two orderings `precedes()` (§6.5) should use. Stored here,
  not read from the wider `t_lab`, so `queue.c` has no dependency on `t_lab` at
  all and could be unit-tested or reused on its own.

### 2.5 `t_coder` (line 26-37)

```c
typedef struct s_coder
{
	int				id;
	pthread_t		thread;
	pthread_cond_t	cond;
	int				ready;
	t_dongle		*left;
	t_dongle		*right;
	long			last_compile_start;
	int				compiles_done;
	struct s_lab	*lab;
}	t_coder;
```

- `id` — the coder's number, `1..number_of_coders`, used only for log output.
- `thread` — the handle `pthread_create` fills in, joined once at shutdown.
- `cond` — this coder's *own* condition variable. It is always used together with
  `lab->sched_mutex` (never with any other lock), for two different waits that
  happen to share the exact same "wake me when something relevant changes"
  shape: waiting for dongles (`request_dongles()`) and sleeping through a
  compile/debug/refactor phase (`wait_ms()`). Giving every coder its own
  condition variable (instead of one shared one) lets the monitor wake up
  *exactly* the one coder it just granted dongles to, with `pthread_cond_signal`,
  instead of waking every coder to have them all re-check and mostly go back to
  sleep.
- `ready` — set to `0` the moment a coder starts waiting, and to `1` by the
  monitor the moment it grants that coder's dongles. This is the actual condition
  the coder is waiting for; `cond` is just the mechanism, `ready` is the truth.
- `left`, `right` — pointers into `lab->dongles`, fixed for the coder's entire
  lifetime once `init_coders()` sets them (§5).
- `last_compile_start` — the relative timestamp (ms since simulation start) at
  which this coder's *current* compile began. `-1` is a sentinel meaning "has
  never compiled yet"; every real timestamp produced by `now_ms()` is `>= 0`, so
  `-1` can never be confused with a real one. Read by the monitor for burnout
  checking and by `request_dongles()` for computing the next EDF deadline —
  always under `lab->sched_mutex`.
- `compiles_done` — how many compiles this coder has finished; compared against
  `lab->compiles_required` by the monitor. Also guarded by `lab->sched_mutex`.
- `lab` — back-pointer to the one shared `t_lab`, so every per-coder function
  only needs a `t_coder *` argument and can still reach shared state. This is
  the mechanism that lets the project have zero global variables: "global" state
  is reached by following a pointer that was handed to the thread when it was
  created, not by naming a variable at file scope.

### 2.6 `t_lab` (line 54-73)

```c
typedef struct s_lab
{
	int				coders_n;
	long			burnout_ms;
	long			compile_ms;
	long			debug_ms;
	long			refactor_ms;
	int				compiles_required;
	long			cooldown_ms;
	t_scheduler		scheduler;
	long			start_time;
	int				stopped;
	long			next_arrival;
	t_queue			queue;
	pthread_mutex_t	sched_mutex;
	pthread_mutex_t	log_mutex;
	t_dongle		*dongles;
	t_coder			*coders;
	pthread_t		monitor;
}	t_lab;
```

The whole simulation in one struct, created once on `main()`'s stack. Two groups
of fields:

**Configuration**, written once by `parse_args()` and never again:
`coders_n`, `burnout_ms`, `compile_ms`, `debug_ms`, `refactor_ms`,
`compiles_required`, `cooldown_ms`, `scheduler`.

**Live state**, read and written by multiple threads for the entire run:

- `start_time` — the absolute wall-clock millisecond timestamp the simulation
  began at (set once in `lab_start()`, *before* any thread is created, so it is
  safe for every thread to read it afterwards without a lock — see §6.6).
- `stopped` — the one flag that ends everything: set by the monitor on burnout or
  on completion, read by every coder thread and by `wait_ms()`. Always accessed
  under `sched_mutex`.
- `next_arrival` — the FIFO/tie-break counter, incremented every time a ticket is
  created. Always accessed under `sched_mutex`.
- `queue` — the one shared priority queue (§2.4).
- `sched_mutex` — the single lock guarding everything in this "live state" group
  *except* the dongles themselves and the log: `queue`, `stopped`,
  `next_arrival`, and the handful of per-coder fields the monitor needs to see
  (`ready`, `last_compile_start`, `compiles_done`).
- `log_mutex` — guards nothing but the `printf` in `log_event()`. Deliberately
  its own lock, separate from `sched_mutex`, so that printing a line never makes
  the monitor (which needs `sched_mutex` to keep scheduling) wait on I/O.
- `dongles`, `coders` — the two arrays allocated once in `lab_start()`,
  `coders_n` elements each.
- `monitor` — the monitor thread's handle.

### 2.7 The locking map, at a glance

| Lock | Protects |
|---|---|
| `t_dongle.mutex` (one per dongle) | that dongle's own `taken` and `free_at` — nothing else |
| `t_lab.sched_mutex` (one, shared) | `queue`, `stopped`, `next_arrival`, and coders' `ready` / `last_compile_start` / `compiles_done` |
| `t_lab.log_mutex` (one, shared) | the `printf` inside `log_event()` |

Two locks are ever held by the same thread at once, and only in one direction:
`sched_mutex` outer, a dongle's `mutex` inner (inside `dongles_try_take_pair()`,
called from `try_grant()`, called from the monitor while it holds `sched_mutex`).
That order is never reversed anywhere in the code, which is what makes the
locking scheme itself deadlock-free (see §9).

---

## 3. Program start-up: `main.c`

The entire file:

```c
1  #include "codexion.h"
2
3  int	main(int argc, char **argv)
4  {
5  	t_lab	lab;
6
7  	memset(&lab, 0, sizeof(lab));
8  	if (parse_args(argc, argv, &lab))
9  		return (1);
10 	if (lab_start(&lab))
11 	{
12 		fprintf(stderr, "codexion: failed to initialize the simulation\n");
13 		return (1);
14 	}
15 	lab_join(&lab);
16 	lab_destroy(&lab);
17 	return (0);
18 }
```

- **Line 5**: `lab` is a plain local variable — the *only* instance of `t_lab`
  that will ever exist. It lives on `main()`'s stack for the whole program.
  Nothing about this project needs `malloc` for the lab itself; everything
  inside it that does need heap memory (`dongles`, `coders`, the queue's
  `items`) is allocated later, in `lab_start()`.
- **Line 7**: zeroes every field defensively before anything reads it. Not
  strictly required (every field is in fact set explicitly by `parse_args()` or
  `lab_start()` before it is ever read), but it costs nothing and removes any
  risk of an uninitialised-read bug if a field is ever added later and someone
  forgets to initialise it in one of those two places.
- **Lines 8-9**: parse and validate the eight command-line arguments directly
  into `lab`. On any invalid input, `parse_args()` has already printed a usage
  message (see §4) — `main()` just needs to stop, with exit code `1`.
- **Lines 10-14**: allocate everything, initialise every mutex and condition
  variable, and start every thread (§5). If *any* of that fails partway through
  — a `malloc` returning `NULL`, or a `pthread_create` failing because the OS is
  out of resources — `lab_start()` guarantees it has already unwound anything it
  partially created, so it is safe for `main()` to simply report the error and
  exit; there is nothing left to clean up.
- **Line 15**: block until every coder thread and the monitor thread have
  returned. This is where `main()` spends effectively the entire runtime of the
  simulation — all of the actual work happens inside the coder and monitor
  threads (§6), not in `main()` itself.
- **Line 16**: once every thread is confirmed finished, it is safe to destroy
  every mutex/condition variable and free every array — nobody can possibly
  still be using them.
- **Line 17**: the simulation ran to a normal conclusion (either every coder hit
  `compiles_required`, or a burnout was detected and logged). Exit code `0`.

---

## 4. Turning `argv` into a `t_lab`: `args.c`

The subject requires **all eight** arguments, requires rejecting "negative
numbers, non-integers, or a scheduler other than fifo or edf", and forbids
crashing on bad input. Three small functions and one entry point handle this.

### `parse_nonneg` (line 3-21) — the one and only number parser

```c
3  static int	parse_nonneg(const char *str, long *out)
4  {
5  	long	value;
6
7  	if (!str || !*str)
8  		return (-1);
9  	value = 0;
10 	while (*str)
11 	{
12 		if (*str < '0' || *str > '9')
13 			return (-1);
14 		value = value * 10 + (*str - '0');
15 		if (value > 1000000000)
16 			return (-1);
17 		str++;
18 	}
19 	*out = value;
20 	return (0);
21 }
```

- **Line 7**: an empty string (`argv[i]` being `""`) is invalid — reject before
  touching `*str`.
- **Lines 10-18**: walk the string one character at a time.
  - **Line 12**: the moment a character is *not* a digit, the whole string is
    rejected. There is no branch anywhere in this function that accepts `+`,
    `-`, whitespace, or a decimal point — which is exactly how "reject negative
    numbers [and] non-integers" is enforced: a negative number necessarily
    contains a `-`, a non-integer necessarily contains something that is not a
    digit, so both are caught by this one check.
  - **Line 14**: standard left-to-right decimal accumulation:
    `value = value * 10 + digit`.
  - **Line 15**: a manual overflow guard. `1000000000` (one billion) is far
    larger than any duration or count a real test will use, and small enough
    that `value * 10` on the *next* iteration cannot overflow a `long` on any
    platform this runs on. This turns "someone passes a 40-digit number" from
    undefined behaviour into a clean rejection.
- **Line 19**: only written if every character was a valid digit and no overflow
  guard tripped — `*out` is never touched on a rejected input, so callers don't
  need to worry about a partially-written result.

### `parse_scheduler` (line 23-32)

```c
23 static int	parse_scheduler(const char *str, t_scheduler *out)
24 {
25 	if (!strcmp(str, "fifo"))
26 		*out = ARBITRATION_FIFO;
27 	else if (!strcmp(str, "edf"))
28 		*out = ARBITRATION_EDF;
29 	else
30 		return (-1);
31 	return (0);
32 }
```

A direct, case-sensitive match against exactly the two strings the subject
allows. Anything else — `"EDF"`, `"round-robin"`, `""` — falls into the `else`
and is rejected. This is what "The value must be exactly one of: fifo or edf"
means taken literally.

### `print_usage` (line 34-42)

Purely informational — printed to `stderr` (not `stdout`, so it never pollutes
the timestamped event log that the grader parses) whenever any argument is
rejected, listing the eight expected arguments and the two validation rules in
plain English.

### `parse_numbers` (line 44-63) — the six duration/count arguments

```c
44 static int	parse_numbers(char **argv, t_lab *lab)
45 {
46 	long	values[6];
47 	int		i;
48
49 	i = 0;
50 	while (i < 6)
51 	{
52 		if (parse_nonneg(argv[i + 2], &values[i]))
53 			return (-1);
54 		i++;
55 	}
56 	lab->burnout_ms = values[0];
57 	lab->compile_ms = values[1];
58 	lab->debug_ms = values[2];
59 	lab->refactor_ms = values[3];
60 	lab->compiles_required = (int)values[4];
61 	lab->cooldown_ms = values[5];
62 	return (0);
63 }
```

`argv[1]` (`number_of_coders`) is handled separately by `parse_args()` because it
needs an extra rule (`!= 0`) the other six don't. This function handles the
remaining six, which are `argv[2]` through `argv[7]`:

- **Lines 49-55**: parse all six into a temporary local array first, `argv[i+2]`
  for `i` in `0..5` (i.e. `argv[2]..argv[7]`). Crucially, **if any one of them is
  invalid, the function returns immediately without writing anything to `lab`
  at all** — `lab`'s fields are only touched after every value in `values[]` is
  known-good (lines 56-61). This avoids ever leaving `lab` in a half-updated
  state after a rejected input.
- **Lines 56-61**: copy the six validated values into their named fields, in the
  same order the subject lists them in the argument list. `compiles_required`
  is the only one of the six that is conceptually a count rather than a
  duration, hence the `(int)` cast — everything else stays a `long` millisecond
  value throughout the program.

### `parse_args` (line 65-86) — the public entry point

```c
65 int	parse_args(int argc, char **argv, t_lab *lab)
66 {
67 	long	coders;
68
69 	if (argc != 9)
70 	{
71 		print_usage(argv[0]);
72 		return (-1);
73 	}
74 	if (parse_nonneg(argv[1], &coders) || coders == 0)
75 	{
76 		print_usage(argv[0]);
77 		return (-1);
78 	}
79 	lab->coders_n = (int)coders;
80 	if (parse_numbers(argv, lab) || parse_scheduler(argv[8], &lab->scheduler))
81 	{
82 		print_usage(argv[0]);
83 		return (-1);
84 	}
85 	return (0);
86 }
```

- **Line 69**: the program name plus eight arguments is nine `argv` entries —
  anything else (missing or extra arguments) is rejected immediately.
- **Line 74**: `number_of_coders` gets its own check because it has an extra
  rule beyond "non-negative integer": it must not be `0` either (a simulation
  with zero coders is meaningless — there would be nothing to run and nothing to
  free).
- **Line 79**: cast to `int` only after the value is known to be a valid,
  positive, in-range number.
- **Line 80**: `parse_numbers()` and `parse_scheduler()` are evaluated with
  short-circuiting `||` — if the six numeric arguments are already invalid,
  the scheduler string is never even inspected, but that doesn't matter because
  either failure produces the exact same outcome (print usage, return `-1`).
- Every failure path in this whole file ends the same way: print usage to
  `stderr`, return a non-zero status, and — critically — **never call any
  `pthread_*` function and never touch `lab->dongles` / `lab->coders`** (they
  haven't been allocated yet at this point in `main()`). Bad input is rejected
  before any resource exists that would need to be freed.

---

## 5. Building the world: `lab_init.c`

This file turns a validated, but otherwise inert, `t_lab` into a running
simulation: two heap-allocated arrays, every mutex and condition variable
initialised, the priority queue ready, and every thread launched. It is written
so that **any failure at any point leaves nothing dangling** — every partial
success is unwound before returning `-1`.

### `init_dongles` (line 3-21)

```c
3  static int	init_dongles(t_lab *lab)
4  {
5  	int	i;
6
7  	i = 0;
8  	while (i < lab->coders_n)
9  	{
10 		if (pthread_mutex_init(&lab->dongles[i].mutex, NULL))
11 		{
12 			while (--i >= 0)
13 				pthread_mutex_destroy(&lab->dongles[i].mutex);
14 			return (-1);
15 		}
16 		lab->dongles[i].taken = 0;
17 		lab->dongles[i].free_at = 0;
18 		i++;
19 	}
20 	return (0);
21 }
```

- **Line 8**: `lab->dongles` was already `calloc`'d (§ below, `lab_start`) with
  exactly `coders_n` elements — this loop initialises each one's mutex and
  starting state.
- **Line 10**: `pthread_mutex_init` almost never fails on Linux with a `NULL`
  attribute, but the return value is still checked, because the subject
  explicitly forbids the program ever misbehaving on any code path, however
  unlikely.
- **Lines 12-13**: if dongle `i` fails to initialise, every dongle `0..i-1`
  that *did* succeed already had its mutex created and must be destroyed again
  before giving up — otherwise those would be leaked (technically: a
  `pthread_mutex_t` that was `init`'d but never `destroy`'d, which on some
  platforms holds kernel resources).
- **Lines 16-17**: `taken = 0` (available) and `free_at = 0` (available from
  time zero — no cooldown applies to a dongle that has never been used). Note
  this duplicates what `calloc` already guaranteed (zeroed memory) — it is
  written explicitly anyway so the initial state is documented at the point
  it matters, not left implicit in an allocation call three functions away.

### `init_coders` (line 23-46)

```c
23 static int	init_coders(t_lab *lab)
24 {
25 	int	i;
26
27 	i = 0;
28 	while (i < lab->coders_n)
29 	{
30 		if (pthread_cond_init(&lab->coders[i].cond, NULL))
31 		{
32 			while (--i >= 0)
33 				pthread_cond_destroy(&lab->coders[i].cond);
34 			return (-1);
35 		}
36 		lab->coders[i].id = i + 1;
37 		lab->coders[i].left = &lab->dongles[i];
38 		lab->coders[i].right = &lab->dongles[(i + 1) % lab->coders_n];
39 		lab->coders[i].last_compile_start = -1;
40 		lab->coders[i].compiles_done = 0;
41 		lab->coders[i].ready = 0;
42 		lab->coders[i].lab = lab;
43 		i++;
44 	}
45 	return (0);
46 }
```

Same defensive-unwind shape as `init_dongles`, but this is also where the whole
seating arrangement is decided:

- **Line 36**: coder numbers are `1`-based (`id = i + 1`), matching the subject's
  "Each coder has a number ranging from 1 to number_of_coders".
- **Line 37**: coder `i` (0-indexed internally) always takes dongle `i` as its
  left dongle.
- **Line 38**: and dongle `(i + 1) % coders_n` as its right dongle — the modulo
  is what closes the circle: the last coder's right dongle wraps back around to
  dongle `0`. **When `coders_n == 1`**, this expression is `(0 + 1) % 1 = 0`, so
  `left` and `right` both end up pointing at the *same* single dongle — exactly
  what the subject means by "if there is only one coder, there should be only
  one dongle on the table". No `if (coders_n == 1)` special case exists anywhere
  in this file; the modulo arithmetic already produces the right answer, and
  every later piece of code that handles a dongle pair (`dongles_lock_pair()`,
  `dongles_try_take_pair()`, `dongles_release_pair()`, §6.4) explicitly handles
  `left == right` as an ordinary case rather than an exception.
- **Line 39**: `-1` sentinel — "never compiled yet" (see §2.5).
- **Lines 40-42**: zeroed counters and the back-pointer to `lab`, so this coder
  can find shared state once its thread starts running.

### `init_sync` (line 48-70)

```c
48 static int	init_sync(t_lab *lab)
49 {
50 	if (pthread_mutex_init(&lab->sched_mutex, NULL))
51 		return (-1);
52 	if (pthread_mutex_init(&lab->log_mutex, NULL))
53 	{
54 		pthread_mutex_destroy(&lab->sched_mutex);
55 		return (-1);
56 	}
57 	if (init_dongles(lab))
58 	{
59 		pthread_mutex_destroy(&lab->sched_mutex);
60 		pthread_mutex_destroy(&lab->log_mutex);
61 		return (-1);
62 	}
63 	if (init_coders(lab))
64 	{
65 		pthread_mutex_destroy(&lab->sched_mutex);
66 		pthread_mutex_destroy(&lab->log_mutex);
67 		return (-1);
68 	}
69 	return (0);
70 }
```

A straight-line sequence of four steps (the two simulation-wide mutexes, then
the two arrays' own primitives), where each step's failure branch destroys
exactly the things that were successfully created *before* it and nothing more.
Note that if `init_dongles` or `init_coders` themselves fail, they have *already*
unwound their own partial work internally (see above) before returning `-1` here
— this function only needs to additionally destroy the two mutexes it created
directly.

### `spawn_threads` (line 72-98)

```c
72 static int	spawn_threads(t_lab *lab)
73 {
74 	int	i;
75 	int	j;
76
77 	if (pthread_create(&lab->monitor, NULL, monitor_main, lab))
78 		return (-1);
79 	i = 0;
80 	while (i < lab->coders_n)
81 	{
82 		if (pthread_create(&lab->coders[i].thread, NULL, coder_main,
83 				&lab->coders[i]))
84 		{
85 			pthread_mutex_lock(&lab->sched_mutex);
86 			lab->stopped = 1;
87 			wake_everyone(lab);
88 			pthread_mutex_unlock(&lab->sched_mutex);
89 			j = 0;
90 			while (j < i)
91 				pthread_join(lab->coders[j++].thread, NULL);
92 			pthread_join(lab->monitor, NULL);
93 			return (-1);
94 		}
95 		i++;
96 	}
97 	return (0);
98 }
```

- **Line 77**: the monitor thread is started **first**, before any coder exists.
  It immediately starts polling an empty queue (harmless — `try_grant()` on an
  empty queue does nothing, §6.3) and waiting for coders to appear.
- **Lines 80-96**: coder threads are then created one at a time, in order
  `1..coders_n`.
- **Lines 82-83**: each coder thread's argument is a pointer to *its own* array
  slot (`&lab->coders[i]`), already fully initialised by `init_coders()` — by
  the time any `coder_main()` starts running, its `id`, `left`, `right`, `lab`,
  and initial counters are all in their final state and never touched by this
  thread again except through the proper locks.
- **Lines 84-93**: if creating coder `i` fails (extremely unlikely — this means
  the OS refused to create *any more threads*, e.g. a resource limit), there are
  already `i` live coder threads and one live monitor thread running. This is
  the one place in the whole project where the simulation must be torn down
  *while it is conceptually "mid-flight"*:
  - **Lines 85-88**: set `stopped = 1` and broadcast every already-created
    coder's condition variable, under `sched_mutex` (the same lock every coder
    checks `stopped` under) — this is the exact same shutdown signal a normal
    burnout or completion uses (§6.2's `wake_everyone`), so the already-running
    coder threads unwind through their completely ordinary stop path and return
    on their own.
  - **Lines 89-91**: join every coder thread that *was* successfully started.
  - **Line 92**: then join the monitor, which also exits as soon as it observes
    `stopped == 1`.
  - Only after every thread has actually returned does this function return
    `-1` — by the time `lab_start()` calls `lab_destroy()` on this path, no
    thread can possibly still be touching any mutex, condition variable, or
    array it is about to destroy/free.

### `lab_start` (line 100-127) — the public entry point

```c
100 int	lab_start(t_lab *lab)
101 {
102 	lab->dongles = calloc((size_t)lab->coders_n, sizeof(t_dongle));
103 	lab->coders = calloc((size_t)lab->coders_n, sizeof(t_coder));
104 	if (!lab->dongles || !lab->coders)
105 	{
106 		free(lab->dongles);
107 		free(lab->coders);
108 		return (-1);
109 	}
110 	queue_init(&lab->queue, lab->coders_n, lab->scheduler);
111 	if (!lab->queue.items || init_sync(lab))
112 	{
113 		queue_destroy(&lab->queue);
114 		free(lab->dongles);
115 		free(lab->coders);
116 		return (-1);
117 	}
118 	lab->stopped = 0;
119 	lab->next_arrival = 0;
120 	lab->start_time = wall_ms();
121 	if (spawn_threads(lab))
122 	{
123 		lab_destroy(lab);
124 		return (-1);
125 	}
126 	return (0);
127 }
```

- **Lines 102-103**: the two big arrays, `calloc`'d (zero-initialised) rather
  than `malloc`'d, so that any field not explicitly set yet (there are none by
  the time threads start, but this is cheap insurance) starts at a predictable
  `0`/`NULL` rather than garbage.
- **Line 104**: `calloc` returning `NULL` for *either* array is treated as one
  failure. `free(NULL)` (line 106 or 107, whichever array didn't actually
  allocate) is well-defined and a no-op in C, so there is no need for an `if`
  around either `free` call.
- **Line 110**: `queue_init()` (§6.5) allocates the heap's backing array,
  `coders_n` tickets deep — exactly the bound reasoned about in §2.4.
- **Line 111**: checks the queue's allocation *and* runs every mutex/condvar
  setup in one condition. If either fails, everything allocated so far (queue,
  both arrays) is released and `lab_start` gives up — note `init_sync` has
  already internally unwound anything *it* partially created, so this cleanup
  only needs to handle the queue and the two top-level arrays.
- **Lines 118-120**: only once every piece of shared state exists and is fully
  initialised does the simulation's actual clock start: `start_time` is stamped
  right here, which means it is set *before any thread that will ever read it is
  created* (threads are spawned two lines later, line 121). This ordering is
  exactly why every later read of `lab->start_time` (inside `now_ms()`, called
  from every thread, constantly) never needs a lock: the value is written once,
  by this thread, strictly before any other thread that could read it exists,
  which is a standard "happens-before by construction" argument, not a race.
- **Line 121**: only now are the monitor and every coder thread actually started
  (§ `spawn_threads` above).
- **Lines 122-124**: if thread creation fails partway, `spawn_threads` has
  already joined every thread it started (see above) — so calling the full
  `lab_destroy()` here is safe and destroys/frees everything that was set up in
  this function, symmetrically.

---

## 6. Two kinds of thread run forever

Once `lab_start()` returns successfully, `main()` is just waiting in
`lab_join()`. All of the interesting behaviour happens concurrently, in
`coders_n` coder threads and one monitor thread, until one of them sets
`lab->stopped = 1`.

### 6.1 `coder.c` — a coder's life cycle

A coder's life, per the subject, is: try to compile → debug → refactor → try to
compile again → ... forever, unless the simulation stops or this coder itself
fails to get dongles because the simulation already stopped while it was
waiting.

#### `is_stopped` (line 3-11)

```c
3  static int	is_stopped(t_lab *lab)
4  {
5  	int	stopped;
6
7  	pthread_mutex_lock(&lab->sched_mutex);
8  	stopped = lab->stopped;
9  	pthread_mutex_unlock(&lab->sched_mutex);
10 	return (stopped);
11 }
```

The simplest possible correct read of a flag that another thread writes:
lock, copy, unlock, return the copy. Never returns a stale value from before the
lock was taken, and never holds the lock for longer than the single copy.

#### `start_compile` (line 13-30)

```c
13 static int	start_compile(t_coder *coder)
14 {
15 	t_lab	*lab;
16
17 	lab = coder->lab;
18 	request_dongles(coder);
19 	if (!coder->ready)
20 		return (-1);
21 	log_event(lab, coder->id, "has taken a dongle");
22 	log_event(lab, coder->id, "has taken a dongle");
23 	pthread_mutex_lock(&lab->sched_mutex);
24 	coder->last_compile_start = now_ms(lab);
25 	pthread_mutex_unlock(&lab->sched_mutex);
26 	log_event(lab, coder->id, "is compiling");
27 	wait_ms(coder, lab->compile_ms);
28 	release_dongles(coder);
29 	return (0);
30 }
```

- **Line 18**: `request_dongles()` (§6.3) blocks this thread until either it has
  been granted both its dongles, or the simulation stopped while it was waiting.
  Everything after this line only runs once that call has returned.
- **Line 19**: the way to tell *which* of those two outcomes happened:
  `coder->ready` is `1` only if the monitor actually granted this coder's
  dongles (see `try_grant()`, §6.3 — it is the *only* place that sets `ready` to
  `1`, and it only runs while `stopped` is still `0`). If the simulation stopped
  first, `request_dongles()` returns with `ready` still `0`, and this function
  bails out immediately without logging anything or touching a dongle — a coder
  that never got dongles has nothing to release.
- **Lines 21-22**: the subject's log format always shows two consecutive
  `"has taken a dongle"` lines before a `"is compiling"` line — one per hand.
  This is printed exactly twice unconditionally, whether the coder's two hands
  ended up on two different physical dongles or (in the `coders_n == 1` case)
  on the same one — from the log's point of view, and from the subject's own
  example output, it is always "two hands, two lines", regardless of how many
  distinct dongles exist underneath.
- **Lines 23-25**: recording *when* this compile started is the one piece of
  per-coder state the monitor needs for burnout checking (§6.2) and that
  `request_dongles()` needs for the *next* EDF deadline (§6.3) — so it is
  written under `sched_mutex`, the same lock both of those readers use.
- **Line 27**: `wait_ms()` (§6.6) sleeps for the compile duration, but is
  interruptible: if another coder burns out (or this run finishes) while this
  coder is mid-compile, this sleep returns early instead of running to
  completion.
- **Line 28**: dongles are always released after the compile — whether the sleep
  ran to completion or was cut short by a stop signal. Releasing on the stop
  path too is what a real coder would do (put the dongles back down), and it
  costs nothing since the simulation is ending anyway.

#### `debug_and_refactor` (line 32-46)

```c
32 static void	debug_and_refactor(t_coder *coder)
33 {
34 	t_lab	*lab;
35
36 	lab = coder->lab;
37 	pthread_mutex_lock(&lab->sched_mutex);
38 	coder->compiles_done++;
39 	pthread_mutex_unlock(&lab->sched_mutex);
40 	log_event(lab, coder->id, "is debugging");
41 	wait_ms(coder, lab->debug_ms);
42 	if (is_stopped(lab))
43 		return ;
44 	log_event(lab, coder->id, "is refactoring");
45 	wait_ms(coder, lab->refactor_ms);
46 }
```

- **Lines 37-39**: the compile that was just released in `start_compile()`
  counts as done the instant debugging starts — matching the subject's
  "number_of_compiles_required: If all coders have compiled at least this many
  times, the simulation stops", counted as soon as the compile phase itself is
  over, not once the whole debug+refactor cycle finishes. Under `sched_mutex`
  because the monitor's `check_all_compiled()` (§6.2) reads this exact field.
- **Line 41**: debug for `time_to_debug` ms, interruptibly.
- **Line 42**: if the simulation ended *during* debugging (for example, this
  increment on line 38 was the one that satisfied `compiles_required` for every
  coder, and the monitor noticed and stopped everything while this coder was
  still asleep on line 41), skip refactoring and the accompanying log line
  entirely — there is no reason to print "is refactoring" for a phase the coder
  is not actually going to spend meaningful time in, and the outer loop
  (`coder_main`, below) is about to exit anyway.
- **Lines 44-45**: otherwise, refactor for `time_to_refactor` ms, interruptibly,
  and then this function returns — control goes back to `coder_main`'s loop,
  which will try to compile again.

#### `coder_main` (line 48-64) — the thread entry point

```c
48 void	*coder_main(void *arg)
49 {
50 	t_coder	*coder;
51 	t_lab	*lab;
52
53 	coder = (t_coder *)arg;
54 	lab = coder->lab;
55 	while (!is_stopped(lab))
56 	{
57 		if (start_compile(coder))
58 			break ;
59 		if (is_stopped(lab))
60 			break ;
61 		debug_and_refactor(coder);
62 	}
63 	return (NULL);
64 }
```

This is the function `pthread_create` calls, `arg` being the `&lab->coders[i]`
pointer handed to it in `spawn_threads()`.

- **Lines 53-54**: recover this coder's own struct and its lab pointer — the
  only two pieces of context this thread ever needs, both reachable from the
  single argument it was given.
- **Line 55**: the outer loop condition — checked fresh at the top of every
  cycle, so a coder that just finished refactoring will not start a new compile
  attempt if the simulation ended in the meantime.
- **Line 57**: attempt one full compile. If `start_compile` returns `-1` (this
  coder waited for dongles but the simulation stopped before it got them),
  `break` out of the loop immediately — there is nothing more for this coder to
  do.
- **Lines 59-60**: even on the success path, re-check `stopped` before running
  debug/refactor — this covers the (rare but possible) case where the
  simulation stopped in the exact gap between `start_compile()` returning
  successfully and this check, without ever starting a debug phase that would
  immediately be cut short anyway.
- **Line 61**: run the debug/refactor phases, then loop back to line 55 and try
  to compile again.
- **Line 63**: `pthread_create` requires a `void *`-returning function; this
  thread never has anything to report back through its return value (all
  results are observed by other threads through the shared `t_lab`/`t_coder`
  fields instead), so it always returns `NULL`. `pthread_join` on this thread
  (in `lab_join()`, §7) simply discards it.

### 6.2 `monitor.c` — the referee

One thread, started once, that is the *only* thread allowed to end the
simulation and the *only* thread that ever grants dongles.

#### `check_burnout` (line 3-25)

```c
3  static int	check_burnout(t_lab *lab)
4  {
5  	int		i;
6  	long	now;
7  	long	reference;
8
9  	now = now_ms(lab);
10 	i = 0;
11 	while (i < lab->coders_n)
12 	{
13 		reference = 0;
14 		if (lab->coders[i].last_compile_start >= 0)
15 			reference = lab->coders[i].last_compile_start;
16 		if (now - reference >= lab->burnout_ms)
17 		{
18 			lab->stopped = 1;
19 			log_event(lab, lab->coders[i].id, "burned out");
20 			return (1);
21 		}
22 		i++;
23 	}
24 	return (0);
25 }
```

Called by `monitor_main` while it already holds `sched_mutex` (below), so every
field read here (`last_compile_start`) is safe to read without a separate lock.

- **Line 9**: one timestamp for this entire pass — every coder is compared
  against the *same* "now", so the pass itself takes effectively zero
  simulated time, which matters for the 10 ms precision requirement.
- **Lines 13-15**: the reference point for "how long has this coder gone
  without compiling" is either its last compile's start time, or (if it has
  never compiled — the `-1` sentinel) `0`, i.e. the moment the simulation
  itself began. This is exactly the subject's own definition: "If a coder did
  not start compiling within time_to_burnout milliseconds since the beginning
  of their last compile **or the beginning of the simulation**...".
- **Line 16**: `>=`, not `>` — a coder that has gone *exactly* `time_to_burnout`
  ms without compiling has burned out, per the subject's wording ("did not
  start compiling within time_to_burnout milliseconds").
- **Lines 18-20**: the moment one coder is found to have burned out, the flag is
  set and the required log line is printed **immediately**, before even
  finishing the loop over the remaining coders — every millisecond of delay here
  eats directly into the 10 ms budget the subject allows. The function returns
  `1` (found one) rather than continuing to look for more; the first burnout
  found is the one that gets reported, which is correct since the simulation is
  about to stop regardless of whether other coders were also close to burning
  out.
- **Line 24**: nobody burned out this pass.

#### `check_all_compiled` (line 27-40)

```c
27 static int	check_all_compiled(t_lab *lab)
28 {
29 	int	i;
30
31 	i = 0;
32 	while (i < lab->coders_n)
33 	{
34 		if (lab->coders[i].compiles_done < lab->compiles_required)
35 			return (0);
36 		i++;
37 	}
38 	lab->stopped = 1;
39 	return (1);
40 }
```

- **Lines 32-36**: the moment *any single* coder is found not to have compiled
  enough times yet, the whole check fails fast — there is no point scanning the
  rest, the simulation is not over.
- **Lines 37-39**: only reached if every coder in the array satisfied the
  condition — matching the subject precisely ("If **all** coders have compiled
  at least this many times, the simulation stops").

#### `monitor_main` (line 42-62) — the thread entry point

```c
42 void	*monitor_main(void *arg)
43 {
44 	t_lab	*lab;
45 	int		done;
46
47 	lab = (t_lab *)arg;
48 	done = 0;
49 	while (!done)
50 	{
51 		pthread_mutex_lock(&lab->sched_mutex);
52 		done = check_burnout(lab) || check_all_compiled(lab);
53 		if (!done)
54 			try_grant(lab);
55 		else
56 			wake_everyone(lab);
57 		pthread_mutex_unlock(&lab->sched_mutex);
58 		if (!done)
59 			usleep(POLL_INTERVAL_US);
60 	}
61 	return (NULL);
62 }
```

- **Line 51**: the *entire* body of one iteration — burnout check, completion
  check, and granting — runs inside a single critical section. This is
  deliberate: it means that, from any other thread's point of view, the state
  of the whole simulation only ever changes in atomic, indivisible steps (never
  observing "half a grant" or a burnout flag set without its log line having
  been printed yet).
- **Line 52**: short-circuiting `||` — if a burnout was just found (and
  `stopped` set, and the line logged), `check_all_compiled` is not even called;
  there is no need to also check completion once the simulation is already
  ending for a different reason.
- **Lines 53-54**: the normal case — nothing ended this pass, so try to advance
  the queue by granting dongles to whoever at the front of it can currently
  have them (§6.3).
- **Lines 55-56**: the simulation just ended this pass (either check returned
  `1`) — every coder that might currently be blocked, either waiting for
  dongles in `request_dongles()` or sleeping in `wait_ms()`, needs to be woken
  up so it can notice `stopped == 1` and return, rather than sleeping for its
  full remaining duration or waiting forever for a grant that will now never
  come.
- **Line 59**: `usleep(POLL_INTERVAL_US)` — `POLL_INTERVAL_US` is `500`
  (microseconds, defined in `codexion.h`), so this thread re-checks every coder
  roughly 2000 times per second. Only reached when nothing ended this pass;
  there is no point sleeping after the loop is about to exit anyway. This
  half-millisecond cadence is what turns "burnout must be logged within 10 ms"
  from a tight deadline into a comfortable twenty-times margin.

### 6.3 `scheduler.c` — the glue between coder and monitor

This file is the only place where a coder thread and the monitor thread
actually hand something to each other. It contains no data of its own — it
operates entirely on the queue, the dongles, and the coder fields defined
elsewhere.

#### `wake_everyone` (line 3-13)

```c
3  void	wake_everyone(t_lab *lab)
4  {
5  	int	i;
6
7  	i = 0;
8  	while (i < lab->coders_n)
9  	{
10 		pthread_cond_broadcast(&lab->coders[i].cond);
11 		i++;
12 	}
13 }
```

Broadcasts every single coder's condition variable, one at a time. Always called
with `sched_mutex` already held by the caller (`monitor_main`, or the error path
in `spawn_threads`) — broadcasting while holding the mutex that guards the
predicate (`stopped`) the waiters are checking is the standard, race-free way to
wake POSIX condition-variable waiters: it guarantees no waiter can miss the
wake-up by checking `stopped` a moment before it was set and only then going to
sleep.

#### `try_grant` (line 15-30) — the only place a ticket ever leaves the queue

```c
15 void	try_grant(t_lab *lab)
16 {
17 	t_coder	*coder;
18 	long	now;
19
20 	now = now_ms(lab);
21 	while (!queue_empty(&lab->queue))
22 	{
23 		coder = queue_peek(&lab->queue);
24 		if (!dongles_try_take_pair(coder->left, coder->right, now))
25 			break ;
26 		queue_pop(&lab->queue);
27 		coder->ready = 1;
28 		pthread_cond_signal(&coder->cond);
29 	}
30 }
```

Called by the monitor, once per loop iteration, while it holds `sched_mutex`.

- **Line 20**: one "now" for the whole pass, same reasoning as `check_burnout`.
- **Line 21**: keep looking as long as there is anyone waiting at all.
- **Line 23**: `queue_peek` never removes anything — it only looks at whoever is
  currently at the front (highest priority: earliest arrival for `fifo`,
  earliest deadline for `edf`, §6.5).
- **Line 24**: the one call that actually checks *and marks* a dongle pair as
  taken, atomically (§6.4). If it returns `0` (the front-of-queue coder's
  dongles are not both free right now — one of them is still `taken`, or still
  cooling down),
- **line 25** stops the whole loop immediately. This is the fairness rule from
  §1.3 made concrete: even if some *other* ticket further back in the queue
  could technically be served right now, it never is, as long as the ticket
  ahead of it is still waiting. Nobody is ever skipped over.
- **Lines 26-28**: only reached if line 24 *succeeded* — the dongles are now
  marked taken. Pop the ticket (it is served), flip `ready` to `1` (the actual
  fact the coder's thread is waiting to observe, see `request_dongles` below),
  and signal — not broadcast — that one coder's own condition variable. Only
  that one thread wakes up; every other coder still asleep in
  `pthread_cond_wait` on its own, different, condition variable is entirely
  unaffected.
- The `while` then loops back to line 21: after granting one coder, it is
  entirely possible the *new* front of the queue can also be granted right away
  (its dongles don't overlap with the one just served), so the monitor keeps
  granting as many tickets as it can in one pass, still strictly in priority
  order, until either the queue empties or the new front is blocked.

#### `request_dongles` (line 32-47) — how a coder joins the queue and waits

```c
32 void	request_dongles(t_coder *coder)
33 {
34 	t_lab	*lab;
35 	long	deadline;
36
37 	lab = coder->lab;
38 	pthread_mutex_lock(&lab->sched_mutex);
39 	coder->ready = 0;
40 	deadline = lab->burnout_ms;
41 	if (coder->last_compile_start >= 0)
42 		deadline += coder->last_compile_start;
43 	queue_push(&lab->queue, coder, lab->next_arrival++, deadline);
44 	while (!coder->ready && !lab->stopped)
45 		pthread_cond_wait(&coder->cond, &lab->sched_mutex);
46 	pthread_mutex_unlock(&lab->sched_mutex);
47 }
```

- **Line 39**: reset `ready` to `0` *before* this coder's ticket is even in the
  queue — nobody else could have set it since the last time this same coder
  released its dongles, but resetting it explicitly here makes the invariant
  "ready is only ever 1 while this coder currently has a live grant it hasn't
  acted on yet" self-evident from reading this one function, rather than
  relying on it having been left at `0` by whatever ran before.
- **Lines 40-42**: exactly the deadline formula from §1.3 —
  `time_to_burnout` alone if this coder has never compiled (`last_compile_start
  == -1`, so the `if` is skipped), or `last_compile_start + time_to_burnout`
  otherwise. Computed once, here, and baked into the ticket — it does not
  change while the ticket sits in the queue, even as real time passes and the
  coder gets objectively closer to burning out; that's fine, because the
  *relative order* between any two tickets' deadlines is already fixed the
  moment both exist, and only the relative order matters for EDF, not the
  absolute value.
- **Line 43**: `lab->next_arrival++` reads the counter's current value (used as
  this ticket's `arrival`) and increments it for the next caller, all under
  `sched_mutex` — so no two tickets can ever end up with the same arrival
  number, even if two coder threads call this function at what looks like the
  same instant. `queue_push` (§6.5) then places the new ticket into the heap.
- **Lines 44-45**: the actual wait. The condition being waited for is
  `coder->ready` (set only by `try_grant`, only while holding this same
  `sched_mutex`) or `lab->stopped` (set by the monitor, also under
  `sched_mutex`). `pthread_cond_wait` atomically releases `sched_mutex` and
  sleeps, so the monitor is always free to take the lock, grant dongles or
  detect a stop, and signal/broadcast, while any number of coders are parked
  here. When this thread wakes up (signalled, broadcast, or — extremely rarely
  — a spurious OS wake-up, which POSIX condition variables are always allowed
  to produce), the `while` re-checks both conditions before deciding whether to
  actually stop waiting, which is the standard, mandatory pattern for using
  condition variables correctly (never trust a single wake-up without
  re-checking the predicate).
- **Line 46**: whichever way the loop above ended — granted, or the simulation
  stopped — release the lock and return. The caller (`start_compile`, §6.1)
  distinguishes the two outcomes by checking `coder->ready` itself right after.

#### `release_dongles` (line 49-56)

```c
49 void	release_dongles(t_coder *coder)
50 {
51 	t_lab	*lab;
52
53 	lab = coder->lab;
54 	dongles_release_pair(coder->left, coder->right, lab->cooldown_ms,
55 		now_ms(lab));
56 }
```

A thin, one-purpose wrapper: fetch this coder's own two dongle pointers and its
lab's cooldown setting, and hand them to `dongles_release_pair()` (§6.4). Note
this function never touches `sched_mutex` at all — releasing a dongle only ever
needs that dongle's own mutex (taken internally by `dongles_release_pair`), so a
coder finishing a compile never has to contend with the monitor's scheduling
lock just to put its dongles back down.

### 6.4 `dongle.c` — the only place a dongle's state changes

Every read or write of `t_dongle.taken` or `t_dongle.free_at`, anywhere in the
project, happens inside one of the four functions in this file.

#### `dongles_lock_pair` / `dongles_unlock_pair` (line 3-27)

```c
3  void	dongles_lock_pair(t_dongle *a, t_dongle *b)
4  {
5  	if (a == b)
6  	{
7  		pthread_mutex_lock(&a->mutex);
8  		return ;
9  	}
10 	if (a > b)
11 	{
12 		pthread_mutex_lock(&b->mutex);
13 		pthread_mutex_lock(&a->mutex);
14 	}
15 	else
16 	{
17 		pthread_mutex_lock(&a->mutex);
18 		pthread_mutex_lock(&b->mutex);
19 	}
20 }
21
22 void	dongles_unlock_pair(t_dongle *a, t_dongle *b)
23 {
24 	pthread_mutex_unlock(&a->mutex);
25 	if (a != b)
26 		pthread_mutex_unlock(&b->mutex);
27 }
```

Every place in the code that needs to touch a coder's *pair* of dongles goes
through these two functions rather than locking each mutex by hand — this is
what guarantees the ordering rule below is never accidentally violated in some
fourth call site added later.

- **Lines 5-8**: the `coders_n == 1` case — both pointers are identical
  (§5, `init_coders` line 38). There is only one mutex to lock; locking it
  "twice" would be locking a non-recursive `pthread_mutex_t` a second time from
  the same thread, which is undefined behaviour (typically an instant
  self-deadlock) — so this is checked and handled first, before either branch
  below can run.
- **Lines 10-19**: for two genuinely different dongles, lock the one at the
  **lower memory address first**, always, regardless of which one is logically
  "left" or "right" for the coder currently asking. Two dongles both belong to
  `lab->dongles`, a single array, so `a > b` is a well-defined comparison (both
  pointers point inside the same array object). Because *every* call site uses
  this same function, and this function always resolves the ordering the same
  way for a given pair of addresses, two coders that both want the same two
  dongles (in opposite "left/right" order relative to each other, since they
  are neighbours) can never lock them in opposite order — which is exactly what
  would be needed to deadlock two `pthread_mutex_t`s against each other. This
  is the one place in the whole project the classic "order your locks
  consistently" rule is actually used — deliberately kept to this narrow,
  mechanical scope rather than being the project's main deadlock defence
  (compare §1.2: the *coder-level* deadlock, hold-and-wait, is prevented by a
  completely different, stronger argument).
- **Lines 24-26**: unlocking in the mirror-image order doesn't actually matter
  for correctness (unlike locking order), but is written to read naturally
  reverse; the important line is 25 — never unlock `b`'s mutex a second time
  when `a == b`.

#### `is_free` (line 29-32)

```c
29 static int	is_free(t_dongle *d, long now)
30 {
31 	return (!d->taken && now >= d->free_at);
32 }
33 
```

A dongle is available exactly when nobody currently holds it **and** its
cooldown (§1.4, §2.2) has elapsed. Both parts are necessary: right after a
release, `taken` immediately goes back to `0`, but `free_at` is in the future,
so this still correctly reports "not free" until the cooldown passes.

#### `dongles_try_take_pair` (line 34-47) — the atomic heart of the scheduler

```c
34 int	dongles_try_take_pair(t_dongle *a, t_dongle *b, long now)
35 {
36 	int	ok;
37
38 	dongles_lock_pair(a, b);
39 	ok = is_free(a, now) && (a == b || is_free(b, now));
40 	if (ok)
41 	{
42 		a->taken = 1;
43 		b->taken = 1;
44 	}
45 	dongles_unlock_pair(a, b);
46 	return (ok);
47 }
```

This is the single function that decides whether a coder can start compiling —
and it is written so that "check if both are free" and "mark both as taken" are
one indivisible operation:

- **Line 38**: lock both dongles (or the one dongle, if `a == b`) before looking
  at anything.
- **Line 39**: both must be free. `a == b ||` short-circuits the second check
  entirely in the single-coder case — there is only one dongle to check, and it
  was already checked as `a`.
- **Lines 40-44**: only if *both* checks passed does either dongle get marked
  `taken`. There is no window, ever, in which this function has observed both
  dongles as free but not yet marked them taken while the lock is released —
  the lock is held continuously from the check to the mark. Note also that when
  `a == b`, line 43 (`b->taken = 1`) writes to the exact same memory as line 42
  did a moment earlier — harmless (it just sets the same field to the same
  value again), and avoiding it with a special case would only add a branch for
  no behavioural benefit.
- **Line 45**: unlock, and return whether it succeeded.

Because the *only* function anywhere that ever sets `taken` from `0` to `1` is
this one, and it is only ever called from `try_grant()` (§6.3), which itself is
only ever called by the single monitor thread — a dongle being taken is
entirely decided by one thread, one function, one lock acquisition. There is no
way for two different coders to both believe they were granted the same dongle.

#### `dongles_release_pair` (line 49-57)

```c
49 void	dongles_release_pair(t_dongle *a, t_dongle *b, long cooldown, long now)
50 {
51 	dongles_lock_pair(a, b);
52 	a->taken = 0;
53 	a->free_at = now + cooldown;
54 	b->taken = 0;
55 	b->free_at = now + cooldown;
56 	dongles_unlock_pair(a, b);
57 }
```

The mirror image of taking: lock the pair (same ordering rule, so this can never
deadlock against a concurrent `dongles_try_take_pair` call for an overlapping
pair either), mark both free again, and stamp both with the same cooldown
expiry. As with taking, when `a == b` lines 52-53 and 54-55 write the same
values to the same fields twice — harmless, and simpler than special-casing it.
After this function returns, the next time the monitor's `try_grant()` calls
`dongles_try_take_pair()` on this dongle, `is_free()` will correctly report
`false` until `now + cooldown` has actually passed.

### 6.5 `queue.c` — the priority queue (binary heap)

A standard array-backed binary min-heap, where "minimum" means "highest
priority" (served next) — implemented from scratch, as the subject requires
("you must implement a priority queue (heap) ... no standard library priority
queue may be used").

#### `precedes` (line 3-8) — the one function that encodes "fifo" vs "edf"

```c
3  static int	precedes(t_queue *q, t_ticket *a, t_ticket *b)
4  {
5  	if (q->mode == ARBITRATION_EDF && a->deadline != b->deadline)
6  		return (a->deadline < b->deadline);
7  	return (a->arrival < b->arrival);
8  }
```

- **Line 5**: only in `edf` mode, and only when the two tickets' deadlines
  actually differ, is the deadline used to decide order.
- **Line 6**: earlier deadline = higher priority = "precedes" the other.
- **Line 7**: the fallback used in every other case — `fifo` mode always, *and*
  `edf` mode whenever two tickets happen to share the exact same deadline.
  Since `arrival` is a strictly increasing counter (§ `request_dongles`, line
  43), no two tickets ever share the same `arrival` value, so this line alone
  is always enough to produce a definite answer — this is what guarantees the
  ordering is fully deterministic even in the tie case the subject specifically
  calls out ("Due to timestamp precision, equal deadlines may rarely occur in
  practice. The tie-breaker rule is required...").

This whole file is entirely independent of `t_lab` — it only knows about
`t_queue` and `t_ticket`, and `t_queue.mode` (set once, at `queue_init`, §2.4)
is all it needs to know which policy to apply.

#### `swap_tickets` (line 10-17)

```c
10 static void	swap_tickets(t_ticket *a, t_ticket *b)
11 {
12 	t_ticket	tmp;
13
14 	tmp = *a;
15 	*a = *b;
16 	*b = tmp;
17 }
```

A plain struct-value swap — `t_ticket` is small (a pointer and two `long`s), so
copying it by value three times is cheap and simple; no need to swap
field-by-field.

#### `sift_up` (line 19-31) and `sift_down` (line 33-53) — restoring heap order

```c
19 static void	sift_up(t_queue *q, int i)
20 {
21 	int	parent;
22
23 	while (i > 0)
24 	{
25 		parent = (i - 1) / 2;
26 		if (!precedes(q, &q->items[i], &q->items[parent]))
27 			break ;
28 		swap_tickets(&q->items[i], &q->items[parent]);
29 		i = parent;
30 	}
31 }
```

Textbook heap "bubble up": as long as the ticket at index `i` has higher
priority than its parent (index `(i - 1) / 2`, standard binary-heap-in-an-array
indexing), swap them and continue from the parent's old position. Stops the
moment the parent no longer outranks the child (line 26), or `i` reaches the
root (`i == 0`, loop condition on line 23).

```c
33 static void	sift_down(t_queue *q, int i)
34 {
35 	int	left;
36 	int	right;
37 	int	best;
38
39 	while (1)
40 	{
41 		left = i * 2 + 1;
42 		right = i * 2 + 2;
43 		best = i;
44 		if (left < q->count && precedes(q, &q->items[left], &q->items[best]))
45 			best = left;
46 		if (right < q->count && precedes(q, &q->items[right], &q->items[best]))
47 			best = right;
48 		if (best == i)
49 			break ;
50 		swap_tickets(&q->items[i], &q->items[best]);
51 		i = best;
52 	}
53 }
```

The mirror operation: starting at index `i`, find whichever of it and its two
children (indices `i*2+1`, `i*2+2`) has the highest priority (lines 41-47,
checking each child only if it is actually within bounds, `< q->count`). If a
child outranks the current node, swap down into that child's position and keep
going (line 50-51); if the current node already outranks both children
(`best == i`), the heap property holds again and the loop stops (line 48-49).

#### `queue_init` / `queue_destroy` (line 55-67)

```c
55 void	queue_init(t_queue *q, int capacity, t_scheduler mode)
56 {
57 	q->items = malloc(sizeof(t_ticket) * (size_t)capacity);
58 	q->count = 0;
59 	q->capacity = capacity;
60 	q->mode = mode;
61 }
62
63 void	queue_destroy(t_queue *q)
64 {
65 	free(q->items);
66 	q->items = NULL;
67 }
```

One allocation, sized exactly `capacity` (which is always `coders_n`, see §2.4
for why that bound is exact rather than a guess), for the entire run — this
heap never grows or shrinks its backing array, only the logical `count` of live
entries within it changes. `queue_init`'s caller (`lab_start`, §5) is
responsible for checking `q->items` for `NULL` before proceeding — `queue_init`
itself has nothing sensible to return on failure since it doesn't return a
status at all, by design (it mirrors the simplicity of every other "just set up
these fields" init function in the project; the allocation-failure check lives
at the one call site instead).

#### `queue_empty` / `queue_peek` (line 69-72, 85-90)

```c
69 int	queue_empty(t_queue *q)
70 {
71 	return (q->count == 0);
72 }
```

```c
85 t_coder	*queue_peek(t_queue *q)
86 {
87 	if (q->count == 0)
88 		return (NULL);
89 	return (q->items[0].coder);
90 }
```

`items[0]` is always the highest-priority live ticket in a valid binary heap —
that invariant is exactly what `sift_up`/`sift_down` exist to maintain after
every insertion and removal. `queue_peek` never modifies the queue; it is safe
to call as many times as needed (`try_grant`'s loop calls it once per
iteration, §6.3) without disturbing anything.

#### `queue_push` (line 74-83)

```c
74 void	queue_push(t_queue *q, t_coder *coder, long arrival, long deadline)
75 {
76 	if (q->count == q->capacity)
77 		return ;
78 	q->items[q->count].coder = coder;
79 	q->items[q->count].arrival = arrival;
80 	q->items[q->count].deadline = deadline;
81 	sift_up(q, q->count);
82 	q->count++;
83 }
```

- **Line 76**: defensive bound check — in practice this can never actually
  trigger, because the queue's capacity equals `coders_n` and no coder ever has
  two outstanding tickets at once (§2.4), but it costs one comparison and turns
  a theoretical off-by-one anywhere else in the codebase into a silent no-op
  instead of writing past the end of the array.
- **Lines 78-80**: write the new ticket into the first free slot, at the current
  `count` (the end of the live region).
- **Line 81**: `sift_up(q, q->count)` — called **before** `count` is
  incremented on line 82. This is intentional, not an off-by-one: the ticket
  was just written at index `q->count` (line 78-80), so that is exactly the
  index `sift_up` needs to bubble upward from; `sift_up` itself never reads
  `q->count` at all (re-check its body above — it only compares indices against
  each other and against `0`), so calling it with an index that is technically
  still "one past the officially live region" for one line's duration is
  completely safe.
- **Line 82**: only now does the new ticket officially become part of the live
  region that `queue_peek`/`queue_pop`/future `sift_down` calls will consider.

#### `queue_pop` (line 92-99)

```c
92 void	queue_pop(t_queue *q)
93 {
94 	if (q->count == 0)
95 		return ;
96 	q->count--;
97 	q->items[0] = q->items[q->count];
98 	sift_down(q, 0);
99 }
```

The standard heap-removal trick: the root (`items[0]`, the ticket being served)
is about to be discarded, so instead of shifting every other element down by
one (expensive), the *last* live element is moved into the root's place (line
97, after `count` has already been decremented on line 96 so `q->items[q->count]`
correctly refers to what was the last live slot), and `sift_down` (line 98)
restores the heap property from the root downward in `O(log n)` instead of
`O(n)`.

### 6.6 `clock_utils.c` and `log.c` — small utilities used everywhere

#### `wall_ms` (line 3-9)

```c
3  long	wall_ms(void)
4  {
5  	struct timeval	tv;
6
7  	gettimeofday(&tv, NULL);
8  	return ((long)tv.tv_sec * 1000 + tv.tv_usec / 1000);
9  }
```

The absolute wall-clock time, in milliseconds, using `gettimeofday` — one of the
functions the subject explicitly allows, and the one it specifically recommends
("real-time measurements using gettimeofday() are acceptable and recommended for
simplicity"). `tv_sec` (whole seconds) times `1000` plus `tv_usec`
(microseconds within the current second) divided by `1000` (dropping
sub-millisecond precision, which nothing in this project needs).

#### `now_ms` (line 11-14)

```c
11 long	now_ms(t_lab *lab)
12 {
13 	return (wall_ms() - lab->start_time);
14 }
```

Every timestamp printed or compared anywhere else in the project is
**relative to simulation start**, not an absolute Unix timestamp — matching the
subject's example log output, which starts at `0`. `lab->start_time` is written
exactly once, in `lab_start()`, before any thread that could call `now_ms()`
exists (§5) — which is why this subtraction never needs a lock even though it
is called from every single thread, constantly.

#### `timespec_from_now` (line 16-25)

```c
16 static void	timespec_from_now(long delay_ms, struct timespec *ts)
17 {
18 	struct timeval	now;
19 	long			nsec;
20
21 	gettimeofday(&now, NULL);
22 	nsec = now.tv_usec * 1000 + (delay_ms % 1000) * 1000000;
23 	ts->tv_sec = now.tv_sec + delay_ms / 1000 + nsec / 1000000000;
24 	ts->tv_nsec = nsec % 1000000000;
25 }
```

`pthread_cond_timedwait` requires an **absolute** `struct timespec` deadline
(not "wait this long", but "wake up at this point in time"), so this converts a
relative "wait `delay_ms` milliseconds from right now" into that absolute form:

- **Line 21**: the current absolute time.
- **Line 22**: combine the current microseconds-within-the-second with the
  sub-second part of the requested delay (`delay_ms % 1000`, converted from
  milliseconds to nanoseconds), giving the total nanosecond offset that doesn't
  cleanly fit into whole seconds.
- **Line 23**: whole seconds now, plus whole seconds of delay, plus any whole
  second that spilled out of the combined nanosecond figure on line 22 (e.g. if
  `now.tv_usec` was `900000` and the delay added another `300` ms worth of
  nanoseconds, the total would exceed one billion nanoseconds and need to carry
  into `tv_sec`).
- **Line 24**: whatever is left in `nsec` after removing whole seconds is the
  final nanoseconds field, always kept in the `0..999999999` range
  `pthread_cond_timedwait` requires.

#### `wait_ms` (line 27-46) — interruptible sleep

```c
27 void	wait_ms(t_coder *coder, long duration)
28 {
29 	t_lab			*lab;
30 	long			deadline;
31 	long			remaining;
32 	struct timespec	ts;
33
34 	lab = coder->lab;
35 	deadline = now_ms(lab) + duration;
36 	pthread_mutex_lock(&lab->sched_mutex);
37 	while (!lab->stopped)
38 	{
39 		remaining = deadline - now_ms(lab);
40 		if (remaining <= 0)
41 			break ;
42 		timespec_from_now(remaining, &ts);
43 		pthread_cond_timedwait(&coder->cond, &lab->sched_mutex, &ts);
44 	}
45 	pthread_mutex_unlock(&lab->sched_mutex);
46 }
```

This is what lets a coder "sleep" through a compile/debug/refactor phase while
still reacting immediately if the simulation stops, instead of either
busy-polling (wasting CPU) or sleeping unconditionally for the full duration
(making shutdown sluggish when a burnout happens elsewhere).

- **Line 35**: the absolute point in (relative) simulated time this sleep should
  end at, computed once, up front — not recomputed as "duration minus elapsed so
  far" repeatedly, which would slowly drift under repeated wake-ups.
- **Line 36**: takes `sched_mutex` — the same lock `stopped` is always read and
  written under, and the same lock this coder's own `cond` is always paired
  with.
- **Line 37**: keep sleeping only as long as nobody has stopped the simulation.
- **Lines 39-41**: recompute how much time is actually left every time this
  loop runs (it can run more than once — see below) — if it's already used up,
  stop sleeping, function is done.
- **Line 42**: turn the remaining duration into the absolute deadline
  `pthread_cond_timedwait` needs.
- **Line 43**: sleep until either that deadline passes (return value
  `ETIMEDOUT`, not checked explicitly — the loop's own recomputation of
  `remaining` on the next pass through line 39-41 will notice), or this coder's
  `cond` is signalled/broadcast. The monitor only ever touches a coder's `cond`
  in two situations: `try_grant` signalling it (irrelevant here — this coder
  isn't waiting for dongles right now, so its `ready` isn't being watched, but
  the wake-up is harmless: the loop just re-checks `stopped`, finds it still
  `0`, recomputes `remaining`, and goes back to sleep for whatever time is
  left) and `wake_everyone` broadcasting on a stop (line 37's condition becomes
  false on the very next check, and the loop exits for real). Either way, this
  function reacts to a stop signal within, at most, one `pthread_cond_wait`
  wake-up latency — not the full remaining sleep duration.
- **Line 45**: release the lock before returning — by this point either the
  full duration has genuinely elapsed, or the simulation has stopped; the
  caller (`coder.c`) checks which one via `is_stopped()` immediately afterward
  where it matters.

#### `log_event` (line 3-11 of `log.c`)

```c
3  void	log_event(t_lab *lab, int coder_id, const char *msg)
4  {
5  	long	elapsed;
6
7  	elapsed = now_ms(lab);
8  	pthread_mutex_lock(&lab->log_mutex);
9  	printf("%ld %d %s\n", elapsed, coder_id, msg);
10 	pthread_mutex_unlock(&lab->log_mutex);
11 }
```

- **Line 7**: the timestamp is captured **before** the lock is taken — if
  several threads are logging at almost the same instant and have to queue up
  for `log_mutex`, each line still reports the moment its own event actually
  happened, not the (slightly later) moment it happened to get the lock. This
  is what keeps timestamps meaningful under contention rather than becoming
  "whenever the log mutex happened to be free".
- **Lines 8-10**: the lock is held for nothing but the `printf` itself — the
  shortest possible critical section — which is exactly what the subject asks
  for ("A displayed state message should not be mixed up with another
  message... use a mutex to protect output") without making logging a
  bottleneck for anything else in the program. `log_mutex` is a lock used
  *nowhere else* in the entire project, so calling this function can never
  contend with, or be blocked by, `sched_mutex` or any dongle's mutex.

---

## 7. Shutting down: `lab_cleanup.c`

Called exactly once, from `main()`, after `lab_join()` and then `lab_destroy()`
have both returned (§3).

#### `lab_join` (line 3-14)

```c
3  void	lab_join(t_lab *lab)
4  {
5  	int	i;
6
7  	pthread_join(lab->monitor, NULL);
8  	i = 0;
9  	while (i < lab->coders_n)
10 	{
11 		pthread_join(lab->coders[i].thread, NULL);
12 		i++;
13 	}
14 }
```

- **Line 7**: the monitor is joined **first**. By the time `pthread_join`
  returns here, the monitor has already set `stopped = 1` and already called
  `wake_everyone()` (its last two actions before returning, §6.2) — so every
  coder thread is guaranteed to already be on its way out (or already
  finished) by the time this function even starts waiting for them.
- **Lines 8-13**: join every coder thread in order. Since none of them can be
  stuck waiting on a condition variable that will never be signalled (the
  broadcast in line 7's aftermath already woke everyone), each `pthread_join`
  here returns as soon as that particular coder's current
  compile/debug/refactor step naturally unwinds — no thread lingers.

#### `lab_destroy` (line 16-37)

```c
16 void	lab_destroy(t_lab *lab)
17 {
18 	int	i;
19
20 	i = 0;
21 	while (i < lab->coders_n)
22 	{
23 		pthread_cond_destroy(&lab->coders[i].cond);
24 		i++;
25 	}
26 	i = 0;
27 	while (i < lab->coders_n)
28 	{
29 		pthread_mutex_destroy(&lab->dongles[i].mutex);
30 		i++;
31 	}
32 	pthread_mutex_destroy(&lab->sched_mutex);
33 	pthread_mutex_destroy(&lab->log_mutex);
34 	queue_destroy(&lab->queue);
35 	free(lab->coders);
36 	free(lab->dongles);
37 }
```

By the time this runs (either right after `lab_join()` on the normal path in
`main()`, or from inside `lab_start()`'s own failure path, §5, where
`spawn_threads` has already guaranteed every started thread was joined first),
no thread anywhere can still be touching any of these — so nothing here needs
any additional locking; simply destroying and freeing in the reverse order of
creation is enough.

- **Lines 20-25**: every coder's condition variable, destroyed.
- **Lines 26-31**: every dongle's mutex, destroyed.
- **Lines 32-33**: the two simulation-wide mutexes.
- **Line 34**: `queue_destroy` frees the heap's backing array (§6.5).
- **Lines 35-36**: the two top-level arrays themselves. After this line, every
  single byte this simulation ever allocated on the heap has been freed —
  which is exactly what the `valgrind --leak-check=full` run mentioned in the
  README confirms (`0 bytes in 0 blocks... All heap blocks were freed`).

---

## 8. A worked example: tracing one coder from birth to its second compile

Concretely tying every earlier section together, for `./codexion 3 1000 200 100
100 2 30 fifo` (3 coders, generous burnout, `fifo`), following coder `1`:

1. `main()` → `parse_args()` fills in `lab` → `lab_start()` allocates
   `dongles[0..2]` and `coders[0..2]`, wires `coders[0].left = &dongles[0]`,
   `coders[0].right = &dongles[1]` (§5), stamps `start_time`, starts the
   monitor, then starts coder threads `1, 2, 3`.
2. Coder 1's thread enters `coder_main` → `start_compile` → `request_dongles`.
   It takes `sched_mutex`, sets `ready = 0`, computes `deadline = 1000` (never
   compiled yet, `last_compile_start == -1`), pushes a ticket
   `{coder=coders[0], arrival=0, deadline=1000}` into the (currently empty)
   queue, and starts `pthread_cond_wait`.
3. The monitor's very next loop iteration (within ~500 µs) takes `sched_mutex`,
   finds no burnout, finds not everyone compiled, calls `try_grant`: peeks the
   queue, sees coder 1's ticket, calls `dongles_try_take_pair(dongles[0],
   dongles[1], now)`. Both dongles are fresh (`taken = 0`, `free_at = 0`), so it
   succeeds — both flip to `taken = 1`. `try_grant` pops the ticket, sets
   `coders[0].ready = 1`, signals `coders[0].cond`.
4. Coder 1's `pthread_cond_wait` returns (still holding `sched_mutex`, per
   POSIX semantics), the `while (!ready && !stopped)` re-check sees `ready ==
   1` and exits the loop, `request_dongles` releases `sched_mutex` and returns.
5. Back in `start_compile`: `coder->ready` is `1`, so it logs `"has taken a
   dongle"` twice, records `last_compile_start = now_ms(lab)` (say, `2`) under
   `sched_mutex`, logs `"is compiling"`, and calls `wait_ms(coder, 200)`.
6. 200 ms later (barring any earlier stop), `wait_ms` returns, `start_compile`
   calls `release_dongles` → `dongles_release_pair(dongles[0], dongles[1], 30,
   now)`: both dongles become `taken = 0`, `free_at = now + 30`.
7. Back in `coder_main`: not stopped, calls `debug_and_refactor`:
   `compiles_done` becomes `1` under `sched_mutex`, logs `"is debugging"`,
   sleeps 100 ms, checks not stopped, logs `"is refactoring"`, sleeps 100 ms.
8. Loop repeats: `coder_main` calls `start_compile` again → `request_dongles`
   again. This time `last_compile_start` is `2` (not `-1`), so the new
   ticket's `deadline` is `2 + 1000 = 1002`. If dongles `0` and `1` are both
   past their `free_at` cooldown and not `taken` by anyone else, the very next
   monitor pass grants it immediately; `compiles_done` becomes `2` once this
   second compile finishes.
9. The monitor's `check_all_compiled` now finds every coder (assuming coders 2
   and 3 followed the same shape) with `compiles_done >= 2` (the
   `number_of_compiles_required` in this example), sets `stopped = 1`, calls
   `wake_everyone`. Every coder thread currently asleep (in `wait_ms` or
   `request_dongles`) wakes, notices `stopped`, and unwinds back up through
   `coder_main`, which returns `NULL`.
10. `main()`'s `lab_join()` returns, `lab_destroy()` frees everything, exit
    code `0`.

---

## 9. Why it cannot deadlock, cannot starve, and cannot race

Three separate correctness arguments, each tied to the exact lines that make it
true — the kind of argument that should be reproducible on demand during a
defence.

**Cannot deadlock (hold-and-wait is structurally impossible).** The only two
functions that ever change `t_dongle.taken` are `dongles_try_take_pair` (dongle.c:34-47)
and `dongles_release_pair` (dongle.c:49-57). The former only ever sets `taken`
to `1` for *both* of a pair's dongles, inside one lock acquisition
(dongle.c:38-45), and only when both were already confirmed free in that same
acquisition (dongle.c:39). There is no function anywhere in the project that
takes one dongle without simultaneously taking its pair. Therefore, at every
instant, every dongle is in one of exactly two states with respect to any coder:
"this coder holds both of its dongles" or "this coder holds neither" — never
"one of two". A cycle of coders each waiting on a resource the next one holds
cannot form if no coder in the prospective cycle is holding anything while it
waits (§1.2).

**Cannot starve (under either policy, given feasible parameters).** All pending
requests live in exactly one queue (`t_lab.queue`), and the *only* function that
ever removes an entry is `try_grant` (scheduler.c:15-30), which always inspects
the queue strictly from the front (`queue_peek`, scheduler.c:23) and stops the
instant the front cannot be served (scheduler.c:24-25) — it never reaches past
a blocked head to serve someone further back. Combined with `precedes()`
(queue.c:3-8) producing a total, deterministic order (no unresolved ties, since
`arrival` is always unique), this means: for any coder's ticket to never reach
the front, infinitely many *other* tickets would have to keep being pushed
ahead of it, and none of them are — no coder can enqueue more than one ticket
at a time (§2.4), so at most `coders_n - 1` other tickets can ever be ahead of
any given one. Every ticket therefore reaches the front within a bounded number
of other coders' compile cycles, each of which is itself bounded
(`time_to_compile` for the compile it's blocking, plus the time until its own
two dongles' cooldowns expire) — a finite wait, not an unbounded one.

**Cannot race.** Every piece of state that more than one thread touches has
exactly one lock that is *always* held while touching it, and that lock is
documented once in §2.7 rather than re-derived at each call site: a dongle's
`taken`/`free_at` only ever inside `dongles_lock_pair`/`dongles_unlock_pair`
(dongle.c), everything else shared (`queue`, `stopped`, `next_arrival`, and the
coder fields the monitor inspects) only ever under `sched_mutex`, and the
`printf` in `log_event` only ever under `log_mutex`. The one field read without
any lock at all, `lab->start_time`, is safe specifically because it is written
exactly once (lab_init.c:120) strictly before any thread that could read it is
created (lab_init.c:121) — a `pthread_create` call is a synchronization point,
so every subsequent read in any thread it spawns is guaranteed to see that
write. This was independently checked, not just argued: both `valgrind
--leak-check=full` (0 leaks) and `valgrind --tool=helgrind` (0 data races
reported) were run against this exact code, across multiple parameter sets
including 1-coder and 50-coder simulations.
