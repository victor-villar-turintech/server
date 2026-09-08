# P-core microarchitecture drill-down — oltp_distinct_ranges, 2026-09-08

Raw data: `artemis/profiling/pcore-drilldown/oltp_distinct_ranges/`

**Directional, not authoritative.** VTune's TMA metrics are absent for this
CPU's E-cores, so this collection ran the server on **P-cores 0-3** - a
different microarchitecture from the E-cores every benchmark number comes from
(artemis/findings/07-profiling-capability.md). Level-1 perf TMA was captured on the same
P-cores so the shift between core types is measured, not assumed.

Conditions: 10-min soak, load verified at 14.8 Gcycles/s, 60 s per collection,
3.71 GHz/core.

## The two core types substantially agree

| Level-1 | E-core (perf, benchmark cores) | P-core (perf, this run) |
|---|---|---|
| Retiring | 39.7% | 42.4% |
| Back-End | 33.1% | 30.2% |
| Front-End | 19.9% | 20.8% |
| Bad Spec | 7.3% | 6.4% |

Within ~3 points on every category. That is a much better transfer than the
single synthetic data point in Phase 0 suggested, and it means the drill-down
below can be read with more confidence than "directional" implies - though it
remains one workload on one host.

CPI is lower on P-cores (0.275 vs 0.34), as expected from a wider core.

## Level-2: where the time actually goes

| Category | % of slots | share of parent |
|---|---|---|
| **Core Bound** (Back-End minus Memory) | **22.8%** | 76% of Back-End |
| Fetch Bandwidth (Front-End minus Latency) | 11.8% | 57% of Front-End |
| Fetch Latency | 9.0% | 43% of Front-End |
| Memory Bound | **7.4%** | 24% of Back-End |
| Branch Mispredict | 5.9% | 92% of Bad Spec |
| Heavy Operations | 2.0% | 5% of Retiring |

**Memory is not the problem.** Only 7.4% of slots, and VTune's breakdown puts
almost none of it in the cache hierarchy: L1 Bound 6.5% of clockticks, L2 Bound
**0.4%**, L3 Bound 1.6%, DRAM Bound 2.1%, Store Bound 0.9%. The working set fits
comfortably; there is nothing to prefetch and no layout problem to fix.

**Back-End pressure is execution, not data.** Core Bound is 22.8% of slots -
three times Memory Bound - with Port Utilization at 22.5% of clockticks and
**61.8% of cycles issuing to 3+ ports**. The machine is issuing wide and staying
busy. This is the microarchitectural signature of *too many operations*, not of
waiting.

**Front-End is bandwidth, not misses.** DSB coverage is 91.3% and ICache Misses
only 3.8% of clockticks, yet Front-End Bandwidth DSB is 17.0% of slots. The
uop cache is working; it simply cannot deliver uops fast enough to feed the
loop. ITLB Misses (5.1%) are the largest single fetch-latency contributor.

**Operation mix**: Memory Operations 15.2% of slots, Fused Instructions 4.9%,
Non-Fused Branches 3.3%, FP Arithmetic and vector operations **0.0%**. Entirely
scalar integer and load/store work - consistent with a byte-at-a-time hash loop.

## What this means for the optimisation angle

The Phase 1 level-1 verdict for this workload was "retiring-bound: only fewer
instructions help". The drill-down **confirms and sharpens** that:

- not memory-bound (7.4%, and L2/L3/DRAM all near zero) - so data layout,
  prefetching and cache-blocking are the wrong tools
- Core Bound at 22.8% with 3+ ports busy 61.8% of cycles - execution
  throughput limited by *volume of operations*
- zero vector uops in the hottest collation path - `MY_HASH_ADD` processes one
  byte at a time and the machine never sees a vector instruction

The productive direction is **fewer, wider operations on the same bytes**:
process the UCA weight stream in larger units rather than per byte, and reduce
the per-weight operation count. Vectorisation is the obvious candidate given
0.0% vector uOps today - but any change must remain **bit-exact in output**,
because the two bytes per weight are deliberately hashed in the "wrong" order
for compatibility with existing partitioned tables (artemis/findings/04-profile-findings.md).
The partitioning tests are load-bearing in the correctness gate.

Secondary, smaller: ITLB misses at 5.1% of clockticks suggest huge pages might
help, but that is a deployment setting rather than a reviewable code patch, so
it fits the client's criterion poorly.

## Caveats

- P-cores, not the benchmark's E-cores. The level-1 agreement above is
  reassuring but is one workload on one host.
- 3.71 GHz here versus ~1.5 GHz under thermal saturation in a full benchmark
  run. A lower clock relatively favours the memory subsystem, so Memory Bound
  would be somewhat higher at benchmark conditions - though from 7.4% with L2/L3
  near zero, it would have to move a very long way to change the conclusion.
- Still evidence, not a committed shortlist: the brief requires the shortlist to
  reflect the client's steer and to be reviewed internally.
