# Discovery loop budget — measured, not estimated

All figures measured on this runner 2026-09-03.

## Component costs

| Step | Cost | Note |
|---|---|---|
| Clone (blobless, main) | 27.6 s | one-off |
| Cold build | 189 s | one-off |
| **Rebuild, single .cc** | **4.2 s** | the normal candidate case |
| Rebuild, hot header (`sql/item.h`, 486 targets) | 16.4 s | worst realistic case |
| One benchmark measurement | 43 s | 10s warm-up + 30s measured + server start/stop |
| A/B at 10 pairs (20 measurements) | 14.2 min | |
| Thermal soak | 10 min | once per session, not per candidate |
| Correctness gate (`mtr --suite=main,innodb`, 1848 tests) | 5.9 min | all passing on baseline |

## The key finding: building is free, measuring is not

The brief anticipated long build times dominating the loop. They do not.
A candidate patch costs **4 seconds to compile and 14 minutes to evaluate** —
a ratio of about 200:1. Every hour spent making the build faster is wasted;
the only lever that matters is reducing measurement time or measuring fewer
candidates.

## Recommended funnel

Cheapest rejection first. Costs per candidate:

| Stage | Cost | Purpose |
|---|---|---|
| 1. Rebuild | 4 s | |
| 2. Perf screen, 4 pairs | 5.7 min | reject candidates that are obviously not winning |
| 3. Correctness gate (mtr) | 5.9 min | reject fast-but-wrong before paying for the full A/B |
| 4. Full A/B, 10 pairs | 14.2 min | the reportable number |

- Rejected at screen: **5.7 min**
- Survivor through all four stages: **25.8 min**

mtr sits *before* the full A/B deliberately: it is cheaper (5.9 vs 14.2 min),
and a candidate can look fast precisely because it skips required work. This
does not weaken the correctness rule from artemis/findings/02-method.md — **nothing is
reported as a win without mtr green**; the funnel only changes the order in
which the search spends its time, not what qualifies as a result.

## Throughput

| Candidates | All-full | With funnel (25% survive screen) |
|---|---|---|
| 10 | 3.5 h | 1.8 h |
| 20 | 6.9 h | 3.8 h |
| 40 | 13.6 h | 7.4 h |

A 20-candidate Discovery run is comfortably an overnight job. This is the
number to quote when asked what multi-candidate search actually costs.

## Ways to buy more candidates, if needed

1. **Shorten the measured window.** 30s is conservative. Re-measuring the noise
   floor at 15s would show whether the floor widens; if it holds, that nearly
   halves A/B cost. Must be re-validated by A/A, never assumed.
2. **Narrow the correctness gate during search.** Run a targeted suite for
   screening and the full suite once before handover. Requires care: for the
   UCA hash hypothesis (H1) the partitioning tests are load-bearing and must
   stay in the screening set.
3. **Do not parallelise across cores on this host.** Only CPUs 4-11 are usable
   as a matched pair of core sets; running two benchmarks at once would
   reintroduce exactly the contention the pinning removes.
