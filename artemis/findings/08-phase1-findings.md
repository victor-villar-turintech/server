# Phase 1 profiling results — 2026-09-08

Raw data: `artemis/profiling/matrix/`
(`summary.md` human-readable, `hotspots.json` machine-readable for the agents)

Method: 10-min thermal soak, then per workload a verified load, 60 s of perf
level-1 TMA on the pinned E-cores (4-7), and 60 s of VTune hotspots against the
same server process. Load was asserted at ~15 Gcycles/s on the server cores
before every collection.

## Headline: four of five workloads are FRONT-END bound

| id | workload | Retiring | Back-End | Front-End | Bad Spec | CPI | bound by |
|---|---|---|---|---|---|---|---|
| 9 | oltp_distinct_ranges | 39.7% | 33.1% | 19.9% | 7.3% | 0.34 | **retiring** |
| 10 | tpcb_key | 16.8% | 25.4% | **52.0%** | 5.7% | 0.83 | front-end |
| 11 | tpcb_no_key | 16.6% | 26.1% | **52.2%** | 5.0% | 0.85 | front-end |
| 5 | oltp_point_select | 16.0% | 21.9% | **58.4%** | 3.6% | 0.87 | front-end |
| 1 | oltp_read_only | 32.1% | 26.4% | 35.0% | 6.6% | 0.42 | front-end |

This **contradicts the assumption we were carrying**. The expectation for a
database engine - and the result from the synthetic Phase 0 workload (87.6%
back-end) - was memory/back-end dominance. The real server is the opposite: the
CPU is starving for *instructions*, not data.

Consequence for the agents: on workloads 5, 10, 11 the productive angle is
**instruction supply** - shrink the hot-path instruction footprint, hoist cold
and error paths out of line, reduce duplicated call sites, avoid megamorphic
dispatch. Data-layout and prefetch work, the reflex answer for a database,
would be aimed at the smaller of the two problems.

`oltp_distinct_ranges` is the exception and the most tractable target: 39.7%
retiring, CPI 0.34, so it is doing real work rather than stalling. For a
retiring-bound workload only *fewer instructions* helps.

## Actionable hotspots

Two categories were excluded from the percentages, and both matter:

**VTune's own collector** (`libtpsstool.so`) appeared as `func@0x823e30`
consuming **15-19%** of attributed CPU time - pure measurement artefact. It
initially looked like the top hotspot for the write workloads. Resolved by
module attribution; now excluded from the denominator so real hotspots are not
understated. This is exactly the sort of artefact that would have sent an agent
chasing a function that does not exist in MariaDB.

**libc/kernel syscall time** is large (23-44 s per workload) and is reported
separately: not patchable, but its size is a signal about network round-trips
per transaction.

### Workload 9 - `oltp_distinct_ranges` (retiring-bound)

| CPU s | % | function | source |
|---|---|---|---|
| 39.24 | **18.0%** | `MY_HASH_ADD` | strings_def.h |
| 17.08 | 7.8% | `my_uca_scanner_next_expansion_weight` | ctype-uca.c |
| 9.87 | 4.5% | `my_charlen_utf8mb4` | ctype-utf8.c |
| 8.89 | 4.1% | `my_ismbchar` | m_ctype.h |
| 5.92 | 2.7% | `my_uca_level_booster_simple_prefix_cmp` | ctype-uca.c |
| 5.34 | 2.5% | `skip_trailing_space` | strings_def.h |

Collation work dominates: the top six functions are all UCA/charset, together
**~40%** of actionable time. `MY_HASH_ADD` alone is 18% and 2.3x the next
symbol - independent confirmation of the very first `perf` profile
(artemis/findings/04-profile-findings.md), now with a bottleneck classification attached.

Retiring-bound plus a byte-at-a-time hash loop is a coherent story: the loop is
executing efficiently, there is simply too much of it per row.

**The constraint recorded in artemis/findings/04 still binds**: the two bytes per weight
are deliberately hashed in the "wrong" order for compatibility with existing
partitioned tables. Any change must be **bit-exact in output**, so this is an
optimisation of *how* the same value is computed, and the partitioning tests are
load-bearing in the correctness gate.

### Workloads 10, 11, 5 - `tpcb_key`, `tpcb_no_key`, `point_select` (front-end bound)

All three share the same top MariaDB function:

| workload | top function | CPU s | % |
|---|---|---|---|
| tpcb_key | `cmp_dtuple_rec_bytes` (page0cur.cc) | 12.35 | 6.7% |
| tpcb_no_key | `cmp_dtuple_rec_bytes` (page0cur.cc) | 9.11 | 5.1% |
| point_select | `cmp_dtuple_rec_bytes` (page0cur.cc) | 6.44 | 4.5% |

Below that the profile is flat - `alloc_root`, `my_betoh64`,
`btr_cur_t::search_leaf`, `page_cur_dtuple_cmp` all under 1%. A flat profile
plus front-end boundedness is consistent: the cost is spread across a large
instruction footprint rather than concentrated in one loop, which is precisely
why no single function stands out.

That makes these workloads **harder targets for a small patch** than workload 9,
and worth saying plainly given the "low review effort" criterion.

## Caveats

- **Clock differs from benchmark conditions.** Profiling ran at 3.73-3.78 GHz;
  thermally-saturated benchmark runs sit near ~1.5 GHz. Collections are short
  enough not to reach the ceiling. Function *ranking* is robust; the
  front-end/back-end *split* would shift somewhat at the benchmark's operating
  point, since a lower core clock relatively favours the memory subsystem.
- **E-cores only.** Correct - it is where the benchmark runs - but VTune's deep
  uarch drill-down (which cache, which stall) is P-core only on this CPU, so the
  *why behind the why* is not yet available for these workloads.
- Level-1 TMA only. Sub-categories (i-cache vs iTLB vs branch resteers) would
  need either the P-core drill-down or explicit `cpu_atom` sub-events.

## Suggested next step

Workload 9 is the strongest candidate on this evidence: the largest single
hotspot in the set (18%), a clear mechanism, a bounded patch surface, and a
workload TAF measures in isolation (`SELECT_DISTINCT_RANGES`).

This remains **evidence, not a committed shortlist** - the brief requires the
shortlist to reflect the optimisation steer (to be chosen), and to be reviewed
internally before Discovery runs.
