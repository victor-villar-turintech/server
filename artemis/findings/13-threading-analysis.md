# Threading / lock-contention analysis — write workloads, 2026-09-08

Raw data: `artemis/profiling/threading/`
Both TPC-B workloads, E-cores (the benchmark cores), 4 client threads, 60 s each,
shared 10-min soak, load verified at ~15 Gcycles/s.

**Question this answers.** Top-down analysis shows where *cycles* go, not where
*threads wait*. The write workloads spent 42-44 s per collection in libc/syscall
time (artemis/findings/08), which could be network round-trips or genuine lock contention -
materially different conclusions. This separates them.

## Headline numbers

| | tpcb_key | tpcb_no_key |
|---|---|---|
| Effective CPU utilisation | 17.6% of 16 CPUs (**2.81 of 4 pinned cores**) | 19.2% (3.08 of 4) |
| Total threads | 46 | 46 |
| **Spin / overhead time** | **0.000 s** | **0.020 s** |
| Top sync object | Mutex, 157.5 s / 159,432 waits | Mutex, 124.4 s / 534,767 waits |
| Futex | 65.3 s / 1,033,658 waits | 65.7 s / 1,444,500 waits |

## The wait time is mostly idle threads, not contention

The aggregate wait figures look alarming (464 s and 395 s of "wait with poor CPU
utilisation") but the per-function breakdown shows most of it is **background
threads legitimately parked**, not workers blocked on each other:

| Wait s (self) | Wait count | function | reading |
|---|---|---|---|
| 242.0 | 1,113,009 | `[No call stack information]` | unattributed |
| 60.0 | **191** | `inline_mysql_cond_timedwait` | idle thread parked |
| 55.2 | **60** | `buf_flush_page_cleaner` | page cleaner sleeping between rounds |
| 52.7 | **2** | `inline_mysql_cond_timedwait` | parked for the entire run |
| 7.6 | 7,563 | `inline_mysql_mutex_lock` | **real contention** |
| 6.4 | 27,739 | `poll` | network wait |
| 4.1 / 3.0 / 2.8 / 1.1 | 3,916 / 2,976 / 2,799 / 1,039 | `inline_mysql_mutex_lock` | **real contention** |
| 1.9 | 5,443 | `srw_mutex_impl::wait` | InnoDB rw-lock |
| 1.2 | 6,662 | `ssux_lock_impl::wait` | InnoDB rw-lock |

A wait count of 2 or 60 over 60 seconds is a sleeping thread, not a hot lock.
Only 46 threads exist for 4 concurrent clients; MariaDB's background workers
(purge, page cleaner, checkpointer) account for the largest single wait blocks.

**Identifiable genuine lock wait totals roughly 22 s** across 60 s elapsed -
present and worth noting, but not dominant.

## Conclusions

1. **No spinlock burn.** Spin time is 0.000 s and 0.020 s. Threads sleep rather
   than spin, so there is no wasted-cycle problem to reclaim.
2. **Not lock-bound at this operating point.** The syscall time seen in artemis/findings/08
   is substantially idle-thread parking and network round-trips, not workers
   blocking each other. The lock-contention hypothesis for tpcb is **not
   supported at 4 threads**.
3. **The ~30% idle capacity on the pinned cores is structural.** With 4
   synchronous client threads driving 4 server cores, each worker has a gap
   waiting for its next request - consistent with `poll` at 27,739 waits. It is
   client-side concurrency, not a server bottleneck.

## The caveat that matters most

**Lock contention typically emerges at high thread counts, and we measured at
four.** TAF's defaults sweep 8 to 1024 threads. At 4 threads a mutex that
serialises badly at 256 threads will look almost free. So this result should be
read as *"no contention problem visible at our operating point"*, **not** as
*"MariaDB has no contention problem"*.

That is a limitation of the hardware, not the method: the runner has 4 usable
pinned server cores, so higher thread counts would measure scheduler thrash
rather than server behaviour. It reinforces two asks already recorded as open questions for MariaDB engineering -
confirm the meaningful thread count, and provide a representative machine.

## Interpretation difference worth recording

The sync-object view attributes 157.5 s to a single mutex over 159,432 waits
(~1 ms each), while the function view accounts for only ~18.6 s in
`inline_mysql_mutex_lock`. The gap sits in the 242 s unattributed block where
VTune could not unwind stacks. We can say contention is not dominant; we cannot
name the specific mutex from this data. Naming it would need stack-enabled
collection or `perf lock`, and is only worth doing if a higher thread count
makes contention material.
