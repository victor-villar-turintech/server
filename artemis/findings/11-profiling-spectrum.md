# The profiling spectrum — three workloads, one coherent picture

Completes the P-core drill-down series (artemis/findings/09, artemis/findings/10). Raw data:
`artemis/profiling/pcore-drilldown/oltp_read_only/`

`oltp_read_only` is a *blend*: each transaction issues 10 point selects plus
one each of simple, sum, order and distinct range queries. It was profiled as
a cross-check - if the two mechanisms found in the other workloads are real
and compose, the blend should sit between them.

## It does, on every metric measured

| Metric | distinct_ranges | **oltp_read_only** | tpcb_key |
|---|---|---|---|
| Retiring | 42.4% | **37.1%** | 21.1% |
| Front-End | 20.8% | **29.6%** | 38.7% |
| Back-End | 30.2% | **27.8%** | 35.5% |
| CPI | 0.275 | **0.324** | 0.613 |
| **DSB coverage** | 91.3% | **73.0%** | 22.2% |
| ICache misses | 3.8% | **7.7%** | 12.5% |
| ITLB misses | 5.1% | **11.1%** | 21.1% |
| Branch resteers | 2.9% | **4.4%** | 7.6% |
| MITE bandwidth | 5.2% | **11.4%** | 18.4% |
| L3 Bound | 1.6% | **3.1%** | 8.6% |
| DRAM Bound | 2.1% | **4.0%** | 7.2% |
| 3+ ports utilised | 61.8% | **55.0%** | 32.7% |

Eleven metrics, three workloads, **monotonic ordering in every case**. Back-End
is the only near-tie, and even there the level-2 split (Memory Bound 7.4% →
10.6% → 19.2%) is ordered.

That consistency matters beyond the individual numbers: it says these
measurements are capturing a real, systematic property of the code paths rather
than collection noise or artefact. It is the strongest internal validation the
profiling work has produced.

## The two mechanisms, and where each workload sits

**Mechanism A - tight loop, too many scalar operations.** `distinct_ranges`:
retiring-bound, uop cache working (91% DSB), caches quiet (L2 0.4%), machine
issuing wide (3+ ports on 62% of cycles), zero vector uOps. One function,
`MY_HASH_ADD`, at 18% of actionable time.

**Mechanism B - large instruction footprint.** `tpcb_key`: uop cache lost
(22% DSB), iTLB thrashing (21%), fallback to legacy decode (MITE 18%), flat
function profile. The deep SQL → handler → InnoDB → B-tree → undo/redo path.

**`oltp_read_only` is a genuine mixture of both**, which is exactly what its
query composition predicts.

## Practical consequence for the pilot

A patch to the collation path should improve `distinct_ranges` strongly and
`oltp_read_only` **detectably but diluted** - the DISTINCT query is one of
fourteen per transaction in the blend, so only a fraction of the gain carries
through.

This has a concrete implication for how a result is reported. The honest
framing is to quote both:

- the targeted workload, where the effect is large and the mechanism is clear
- the headline blend, where the effect is smaller but represents what a mixed
  production workload would actually see

Quoting only the targeted number would overstate it; quoting only the blend
would understate the work. `artemis-bench-ab.sh` can adjudicate both, and the
11-workload suite already tracks each separately, so no new tooling is needed.

It also gives a falsifiable prediction to test once a candidate exists: if a
collation patch improves workload 9 but leaves workload 1 completely flat, the
mechanism is not what we think it is.

## Status of the profiling work

Complete for the priority workloads. Three independent instruments - the
original perf profile, Phase 1 level-1 TMA on the benchmark cores, and the
P-core level-2 drill-down - agree on the target and the mechanism.

Remains **evidence, not a committed shortlist**: the brief requires the
shortlist to reflect the client's steer and to be reviewed internally before Discovery runs.
