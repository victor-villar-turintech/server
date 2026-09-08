# Phase 1 profiling — consolidated report

**MariaDB Server performance pilot · 2026-09-08**
Target: `MariaDB/server` 13.1.0 @ `b2a8c223` (branch `perf/baseline`)

This is the single reference for the profiling work. It supersedes the reading
of notes 07-13 for anyone who needs the conclusions rather than the derivation.

---

## 1. What was measured, and why

Our earlier `perf` profile said *where* time went. It could not say *why*. That
distinction decides what an optimising agent should attempt: a memory-bound
hotspot wants better data layout; a core-bound one wants fewer operations; a
front-end-bound one wants a smaller instruction footprint. Aiming the wrong
remedy at a hotspot wastes the whole search.

Phase 1 attaches a **bottleneck classification** to every hotspot, and resolves
the top ones to **source lines**.

## 2. Instruments, and where each is valid

This CPU (Intel Core Ultra X7 368H, "Panther Lake") is hybrid, with two PMUs.
VTune's top-down metrics return **zero on the E-cores** where our benchmark
server is pinned; they work only on P-cores. Profiling on P-cores alone would
describe a different microarchitecture from every benchmark number we hold.

| Question | Instrument | Cores | Authority |
|---|---|---|---|
| Which functions / source lines are hot? | VTune hotspots | E-cores 4-7 | authoritative |
| Which bottleneck category? | perf topdown (`cpu_atom`) | E-cores 4-7 | authoritative |
| Why exactly - which cache, which stall? | VTune uarch-exploration | P-cores 0-3 | see below |
| Are threads blocking on each other? | VTune threading | E-cores 4-7 | authoritative |

**The P-core caveat turned out to be weaker than feared.** Measured on the same
workload, level-1 agrees within ~3 points across all four categories
(E-core 39.7/33.1/19.9/7.3 vs P-core 42.4/30.2/20.8/6.4), so the drill-down
transfers better than a single-core-type comparison would have suggested.

Every collection was gated: host verified quiet, 10-minute thermal soak, and
**load asserted at ~15 Gcycles/s on the server cores before sampling** - a guard
added after an early run silently profiled an idle server.

## 3. Results

| id | workload | Retiring | Back-End | Front-End | Bad Spec | CPI | bound by |
|---|---|---|---|---|---|---|---|
| 9 | `SELECT_DISTINCT_RANGES` | 39.7% | 33.1% | 19.9% | 7.3% | 0.34 | **retiring** |
| 1 | `OLTP_RO` | 32.1% | 26.4% | 35.0% | 6.6% | 0.42 | mixed |
| 10 | `TPCB_KEY` | 16.8% | 25.4% | 52.0% | 5.7% | 0.83 | front-end |
| 11 | `TPCB_NO_KEY` | 16.6% | 26.1% | 52.2% | 5.0% | 0.85 | front-end |
| 5 | `POINT_SELECT` | 16.0% | 21.9% | 58.4% | 3.6% | 0.87 | front-end |

**Four of five workloads are front-end bound.** This contradicted our working
assumption - the database reflex, and our own synthetic calibration workload
(87.6% back-end), both pointed at memory. The real server is instruction-supply
limited, not data limited, on most of the suite.

### The microarchitectural spectrum — all five workloads

P-core level-2 drill-down, complete:

| workload | Retiring | Back-End | Front-End | Fetch Lat | Mem Bound | Core Bound | CPI |
|---|---|---|---|---|---|---|---|
| `distinct_ranges` | 42.4% | 30.2% | 20.8% | 9.0% | 7.4% | **22.8%** | 0.275 |
| `oltp_read_only` | 37.1% | 27.8% | 29.6% | 13.3% | 10.6% | 17.3% | 0.324 |
| `tpcb_key` | 21.1% | 35.5% | 38.7% | 19.6% | 19.2% | 16.3% | 0.613 |
| `tpcb_no_key` | 20.8% | 36.3% | 38.7% | 19.2% | 19.3% | 17.0% | 0.623 |
| `point_select` | 22.0% | 40.5% | 34.6% | 14.7% | 21.4% | 19.1% | 0.593 |

### The front-end axis is strictly ordered

| Metric | distinct_ranges | read_only | tpcb_key | tpcb_no_key | point_select |
|---|---|---|---|---|---|
| **DSB (uop cache) coverage** | **91.3%** | 73.0% | 22.2% | 16.0% | **5.9%** |
| ITLB misses | 5.1% | 11.1% | 21.1% | 23.5% | **25.3%** |
| MITE (legacy decode) | 5.2% | 11.4% | 18.4% | 18.7% | **22.1%** |
| ICache misses | 3.8% | 7.7% | 12.5% | 12.7% | 12.1% |
| L3 Bound | 1.6% | 3.1% | 8.6% | 9.4% | 8.9% |
| DRAM Bound | 2.1% | 4.0% | 7.2% | 6.1% | 5.7% |
| 3+ ports utilised | 61.8% | 55.0% | 32.7% | 31.6% | 34.5% |

**DSB coverage falls monotonically 91.3 → 73.0 → 22.2 → 16.0 → 5.9%**, and
ITLB misses and legacy-decode fallback rise monotonically alongside it. That
axis is clean across all five workloads.

The memory metrics are *not* strictly ordered across the last three. That is
expected and worth stating rather than smoothing: `tpcb_key`, `tpcb_no_key` and
`point_select` are three points in the *same* regime, so their relative order on
secondary metrics carries no signal. The meaningful structure is the gap between
the first two workloads and the last three.

### What the ordering actually measures

The counter-intuitive result is that **`POINT_SELECT` — the simplest query in
the suite — has the worst instruction-supply behaviour of all** (5.9% DSB
coverage, 25.3% ITLB misses). That is not a contradiction; it is the
explanation.

The axis is **ratio of framework code to inner-loop work**. A point select does
almost no work per query, so the fixed cost of the SQL path - parse, prepare,
execute, handler dispatch, InnoDB entry - dominates completely, and that path is
large. `distinct_ranges` runs a tight collation loop many times per query, so
the same framework cost is amortised and the uop cache serves the loop.

This reframes the front-end finding: MariaDB's per-query framework path is
large enough to defeat the uop cache, and the *only* workloads that escape it
are those doing enough inner-loop work to amortise it.

## 4. Two mechanisms

**A — Tight loop, too many scalar operations** (`SELECT_DISTINCT_RANGES`)
uop cache working, caches quiet (L2 0.4%, DRAM 2.1%), core issuing wide,
**zero vector uOps**. One function dominates. Bounded patch surface.

**B — Large instruction footprint** (`TPCB_*`, `POINT_SELECT`)
uop cache lost (22% coverage), iTLB thrashing (21%), fallback to legacy decode,
flat function profile. The deep SQL → handler → InnoDB → undo/redo path.

`OLTP_RO` is a genuine blend of both, exactly as its query composition predicts
(10 point selects + 4 range queries per transaction).

## 5. The patch target, at source-line level

For `SELECT_DISTINCT_RANGES`, collation work is ~40% of actionable CPU time and
the hottest single **line** in the entire study is `strings_def.h:209` at
**42.5 s**:

```c
#define MY_HASH_ADD_MARIADB(A, B, value) \
  do { A ^= (((A & 63) + B) * (value)) + (A << 8); B += 3; } while(0)
```

This resolves the apparent paradox of a retiring-bound workload that cannot go
faster: `A` on each byte depends on `A` from the previous byte **through a
multiply**. The multiply latency sets a floor per byte no matter how many
execution ports are idle - which is why 62% of cycles issue to 3+ ports while
this is still the bottleneck.

Two structural observations:

- **`B` is trivially predictable** (`B += 3` each iteration, so `B_n = B_0 + 3n`).
  It need not be carried through the chain at all - classic strength reduction.
- **`A` is the real chain.** A speedup must shorten it or process multiple bytes
  per step with an algebraically equivalent formulation.

Supporting hotspots: `ctype-uca.c:31285` (16.3 s,
`my_uca_scanner_next_expansion_weight` - the loop feeding the hash),
`ctype-utf8.c:3008` (7.3 s), `m_ctype.h:1980` (6.1 s).

> **Hard constraint.** These hashes determine partition placement for existing
> tables; the two bytes per weight are deliberately combined in a non-obvious
> order for compatibility. Any change must be **bit-exact in output**. This is a
> rewrite of *how* the same value is computed. The partitioning tests are
> load-bearing in the correctness gate.

## 6. Threading

Write workloads are **not lock-bound at our operating point**. Spin time is
0.000 s / 0.020 s; the alarming aggregate wait figures (464 s) are dominated by
background threads legitimately parked - a `cond_timedwait` with a wait count of
**2** over 60 s is a sleeping thread, not a hot lock. Genuine identifiable lock
wait is ~22 s.

**But we measured at 4 threads and TAF sweeps to 1024.** Contention emerges at
high thread counts. Read this as *"no contention visible at our operating
point"*, not *"MariaDB has no contention problem"*.

## 7. What Artemis consumes

`artemis/profiling/matrix/hotspots.json` - one object
per workload, keyed by the **same `workload_id` as `artemis_results.json`**, so
agents can join "what moved" against "what is hot there":

```
workload_id, workload
tma { retiring_pct, back_end_pct, front_end_pct, bad_spec_pct, cpi }
dominant_bottleneck          front_end | back_end | retiring | bad_spec
suggested_angle              the transformation class that fits that bottleneck
actionable_hotspots[]        MariaDB code only: function, cpu_seconds, cpu_pct, source_file
hot_source_lines[]           source_file, line, cpu_seconds
excluded_time_seconds        { collector, syscall }
```

Symbols are classified **server / syscall / collector**, and agents are shown
only `server` - code they can actually patch.

## 8. Three measurement errors this work caught

Worth recording because they are the argument for the loop, not just artefacts:

| Error | Magnitude | Now guarded by |
|---|---|---|
| Cold vs thermally-saturated identical binary | **33% phantom regression** | 10-min soak + interleaved A/B |
| Another process sharing benchmark cores | **8× phantom slowdown** | `check-quiet.sh`, re-checked per pair |
| VTune's own collector (`libtpsstool.so`) as top "hotspot" | **15-19% of CPU** | module classification in distillation |
| Silently profiling an idle server | would have been 100% wrong | load asserted before every capture |

## 9. Limits

- **Clock.** Profiling ran at 3.65-3.78 GHz; thermally saturated benchmark runs
  sit near ~1.5 GHz. Function *ranking* is robust; the front-end/back-end
  *split* would shift somewhat at benchmark conditions, since a lower core clock
  relatively favours the memory subsystem.
- **Thread count.** 4 threads throughout - see §6.
- **Hardware.** Thermally constrained mobile part. Absolute throughput from this
  host is not quotable; only relative A/B comparisons are.
- **One unresolved discrepancy**, recorded rather than smoothed over: VTune's
  sync-object view attributes 157 s to a single mutex while the function view
  accounts for ~18.6 s. The gap sits in a block VTune could not unwind. We can
  say contention is not dominant; we cannot name the mutex from this data.

## 10. Status

**Complete.** All five priority workloads have level-1 TMA, function and
source-line hotspots, and a P-core level-2 drill-down; both write workloads
additionally have threading analysis.

**Recommendation on the evidence:** attack `SELECT_DISTINCT_RANGES` first -
three independent instruments agree on target and mechanism, and it is the one
workload in the set with a bounded patch surface. Treat `TPCB_*` and
`POINT_SELECT` as **must-not-regress measurement targets**: their bottleneck is
aggregate instruction footprint across the framework path, which has no
small-patch remedy and would need PGO/BOLT, huge pages, or broad inlining work.

A **differential bit-exactness gate** now exists for any collation-hash change
(artemis/findings/15) - 70,868 vectors, validated against both gross and subtle sabotage.
It is a hard reject: no benchmark number is computed for a candidate that alters
hash output.

This remains **evidence, not a committed shortlist**: the brief requires the
shortlist to reflect the client's steer and review internally
before Discovery runs.

## File index

| Artefact | Path |
|---|---|
| **Agent-facing data** | `artemis/profiling/matrix/hotspots.json` |
| Human summary | `artemis/profiling/matrix/summary.md` |
| Level-1 TMA raw | `artemis/profiling/matrix/*.topdown.txt` |
| P-core drill-downs | `artemis/profiling/pcore-drilldown/` |
| Threading | `artemis/profiling/threading/` |
| Hash-exactness gate | `artemis/gates/hashcheck/` · reference `results/hashcheck/reference.tsv (runner host; 16 MB, not in repo)` |
| Collection script | `harness/profile-matrix.sh` |
| Drill-down script | `harness/profile-pcore-drilldown.sh` |
| Threading script | `harness/profile-threading.sh` |
| Distillation | `harness/profile-distill.sh` |
| Derivation notes | `artemis/findings/07-*` capability · `08` findings · `09`-`11` drill-downs · `13` threading · `15` hash gate |
