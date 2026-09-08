# P-core drill-down — tpcb_key, 2026-09-08

Raw data: `artemis/profiling/pcore-drilldown/tpcb_key/`
Same protocol as artemis/findings/09 (P-cores 0-3, 10-min soak, load verified at
14.5 Gcycles/s, 60 s per collection, 3.65 GHz/core). Directional: P-cores, not
the benchmark's E-cores.

## Level-1: front-end bound, and the E-core result reproduces

| Level-1 | E-core (benchmark cores) | P-core (this run) |
|---|---|---|
| Retiring | 16.8% | 21.1% |
| Back-End | 25.4% | 35.5% |
| Front-End | **52.0%** | **38.7%** |
| Bad Spec | 5.7% | 4.7% |

Front-end dominant on both, though less extreme on the wider core - the P-core's
larger caches and better prefetch absorb some of the pressure. CPI 0.613.

## The contrast with distinct_ranges is the finding

Both workloads, same host, same protocol:

| Metric | distinct_ranges | **tpcb_key** | |
|---|---|---|---|
| **DSB coverage** | 91.3% | **22.2%** | uop cache effectively lost |
| Front-End Bandwidth MITE | 5.2% | **18.4%** | falling back to legacy decode |
| Front-End Bandwidth DSB | 17.0% | 2.5% | |
| **ITLB misses** | 5.1% | **21.1%** | 4x - code spans far more pages |
| **ICache misses** | 3.8% | **12.5%** | 3.3x |
| Branch Resteers | 2.9% | 7.6% | |
| L3 Bound | 1.6% | **8.6%** | |
| DRAM Bound | 2.1% | **7.2%** | |
| 3+ ports utilised | 61.8% | **32.7%** | machine no longer issuing wide |

**This is a large-instruction-footprint profile, not a hot-loop profile.**

DSB coverage collapsing from 91% to 22% means the code path no longer fits the
uop cache and the front end falls back to legacy decode (MITE 18.4%). ITLB
misses at 21.1% of clockticks say the *instruction* footprint spans so many
pages that address translation thrashes. That is the expected shape of an OLTP
write path: a deep call chain through SQL layer, handler API, InnoDB, B-tree
descent, undo and redo logging - a great deal of code executed once per row
rather than one loop executed many times.

Memory pressure is also genuine here in a way it was not for distinct_ranges:
L3 8.6% and DRAM 7.2% (versus 1.6% and 2.1%), consistent with B-tree and undo
page traversal touching a working set that does not sit in cache.

## What this means for target selection

**tpcb_key is a poor fit for the brief, and the profile says why.**

The cost is spread across a large volume of code, not concentrated in a loop.
That matches the flat function profile from Phase 1 - `cmp_dtuple_rec_bytes` at
6.7% and then nothing above 1%. The remedies that address instruction footprint
are:

- profile-guided optimisation or BOLT-style layout - a *build* change, not a
  source patch
- huge pages for the text segment - a *deployment* setting
- broad inlining and cold-path restructuring across the write path - a large,
  invasive change touching many files

None of these is "a small, high-impact change a MariaDB engineer can review in
minutes". A patch that meaningfully moves this workload would be exactly the
kind of large diff the client's criterion excludes.

**This creates a real tension worth naming.** TPC-B is the workload MariaDB
Foundation says historically caught ~70% of regressions, so it is what they care
most about - and it is the worst fit for the "minimum expert review time"
constraint. Worth putting to MariaDB engineering directly rather than quietly optimising
something easier and hoping it lands.

By contrast distinct_ranges is retiring-bound with 91% DSB coverage, near-zero
cache pressure and a single function at 18% of actionable time: a bounded patch
surface where a small change can plausibly move the number.

## Recommendation on the evidence

Order of attack: **distinct_ranges first** (workload 9), on all three
instruments agreeing and a tractable patch surface. Treat tpcb as a
*measurement* target - the thing we must not regress - rather than an
optimisation target for a first patch.

Still evidence, not a committed shortlist: the brief requires the client's steer, and review internally before Discovery.
