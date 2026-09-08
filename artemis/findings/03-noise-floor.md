# Noise floor experiments

The gating question for the whole project: **what is the smallest
improvement this host can honestly resolve?** Anything below that number is
unreportable no matter what the p-value says.

Method: the same `mariadbd` binary benchmarked against itself (arms verified
bit-identical by sha256), separate datadirs and sockets, full server restart
between measurements, ABBA interleaved ordering. Any difference measured is
noise by construction.

---

## Experiment 1 — 2026-09-03, no thermal soak

`results/ab-noise_a-vs-noise_b-20260903T093344Z`
10 pairs / 20 measurements, sysbench `oltp_read_only`, 8x100k rows, 4 threads,
10s warm-up + 30s measured, server on CPUs 4-7, client on 8-11.

**Result: the host failed this experiment.**

| Analysis | 95% CI on a known-null effect |
|---|---|
| Unpaired (Mann-Whitney) | **-10.28% .. +11.41%** |

CV was 11.7% / 9.9% per arm. An interval of +/-11% on a null result means no
patch of the size the client is asking for could be distinguished from the machine.

### Cause: thermal decay, not random noise

Throughput in run order:

```
6412  5936  5640  5440  5284  5180  4925  4812  4862  4741
4770  4711  4633  4644  4574  4626  4528  4566  4521  4596
```

A monotonic **-28.3% decline over ~17 minutes**, asymptoting near 4570 tps.
Rolling 6-run CV tells the same story:

| Window | mean tps | CV |
|---|---|---|
| runs 1-6   | 5649 | 8.16% |
| runs 7-12  | 4804 | 1.66% |
| runs 15-20 | 4568 | **0.88%** |

The host is not inherently noisy. It was being measured while it cooled from
idle into thermal saturation. Once saturated it is stable to ~1%.

Note the single pre-experiment measurement recorded 6794 tps on a cold
machine, versus ~4570 tps at steady state. **A naive before/after using a cold
baseline and a warm candidate would have manufactured a 33% "regression" out
of nothing.** This is precisely the failure mode behind MariaDB's "signal or
noise?" objection.

### Fix 1 — paired analysis

Because ab.sh runs A and B adjacently, both arms of a pair sit within ~90s of
each other on the drift curve. Differencing within a pair cancels it.
Re-analysing the *same* data:

| Analysis | 95% CI on the null |
|---|---|
| Unpaired | -10.28% .. +11.41%  (width 21.7pp) |
| **Paired (Wilcoxon signed-rank)** | **-2.06% .. +1.59%  (width 3.65pp)** |

A 6x tightening from the analysis alone, no extra measurements.

Restricting further to the four thermally-settled pairs gives a within-pair
sd of 1.15pp, which indicates where the floor lands once the drift is gone.

### Fix 2 — thermal soak

Drive the workload for 600s and discard it, so measurement begins at steady
state rather than traversing the decay curve. Implemented in `ab.sh`;
CPU temperature and mean clock are now recorded per measurement, turning the
thermal explanation from an inference into logged evidence.

---

## Experiment 2 — 2026-09-03, 600s soak + paired analysis

`results/ab-noise_a-vs-noise_b-20260903T095139Z`
Same configuration as experiment 1, plus a 600s discarded soak, analysed with
the paired Wilcoxon test.

**Result: the host passes.**

| | 95% CI on a null result | within-pair sd | series drift | CV |
|---|---|---|---|---|
| exp1 unpaired, no soak | -10.28% .. +11.41% | - | -28.3% | 11.70% |
| exp1 paired, no soak | -2.06% .. +1.59% | 3.10 pp | -28.3% | 11.70% |
| **exp2 paired, 600s soak** | **-0.56% .. +0.53%** | **0.88 pp** | **-2.36%** | **1.27%** |

Combined, the soak and the paired analysis narrowed the null interval by a
factor of ~20.

### Adopted noise floor: 1.0%

The measured floor is ~0.6%; `MDB_NOISE_FLOOR` is set to **1.0%** in
`harness/00-env.sh` to round conservatively. A candidate is only called a WIN
if its entire 95% CI clears +1.0%. In practice this host can honestly resolve
effects of roughly 1.5% and up.

This must be re-measured after any change to the host, the workload, the
thread count or the pinning.

### Caveat that matters for target selection

Temperature sat at **98-100 C for the whole of experiment 2**, with sampled
clock between 1.0 and 2.2 GHz against a 4.0 GHz nominal for these E-cores.
The host is stable because it is *pinned against its thermal limit*, not
because it is running freely.

Two consequences:

1. Absolute throughput here (~4250 tps) badly understates MariaDB on real
   server hardware (~6800 tps was observed on this same box merely from
   starting cold). **Absolute numbers from this runner are not quotable.**
   Only the relative A/B comparison is.
2. At ~1.5 GHz effective clock the CPU-to-memory speed ratio is very different
   from a datacenter part holding 3+ GHz. That shifts the balance between
   compute-bound and memory-bound code, so a hotspot ranking derived here may
   not rank the same way on MariaDB's hardware, and a patch that wins here
   could be neutral there.

Point 2 is the strongest argument for getting a dedicated bare-metal runner
before the result is put in front of MariaDB engineering. It does not block
building the loop or selecting targets, but the final number should be
reproduced on hardware that is not thermally pinned.

---

## Still unavailable: fixed clock

`performance` governor and `no_turbo=1` both need root and are not yet applied.
Every number above was taken with `powersave` + turbo active, so they represent
an **upper bound** on this host's noise, not its best achievable.

---

## Experiment 3 — 2026-09-07, cross-build A/A (the Discovery-shaped null test)

`results/ab-baseline-vs-candidate-20260907T115307Z`
Unlike experiments 1-2 (one binary vs itself via symlinks), this compares two
INDEPENDENTLY BUILT binaries of identical source - the same shape as a real
Discovery comparison. Full pipeline: quiet-host waiter, 600s soak, 10 ABBA
pairs, per-pair contention gate (no pauses triggered), paired analysis.

| | Result |
|---|---|
| Verdict | **NO-EFFECT** (correct) |
| Median improvement | -0.56% |
| 95% CI | -3.13% .. +1.28% |
| Wilcoxon p | 0.49 |
| Within-pair sd | 2.65 pp |

### Why sd is 2.65pp here vs 0.88pp in experiment 2

Experiment 2's arms were sha256-identical files. Independent builds differ in
code layout (timestamps shift link order and alignment), which perturbs
i-cache and branch-predictor behaviour differently per binary - a real,
systematic component that identical-file A/A cannot see. Discovery compares
independent builds, so THIS is the honest null spread for real candidate
evaluation on this host.

### Consequence for the WIN threshold

A ~2-3pp cross-build spread means the 1.0% noise floor is optimistic for
cross-build comparisons at 10 pairs: expect to resolve effects of roughly
3%+ reliably, or run more pairs for smaller effects. Options if finer
resolution is needed: more pairs (CI shrinks ~1/sqrt(n)), or link-layout
normalisation. On dedicated hardware this should also tighten.

Earlier the same day, two contention incidents validated the guard stack:
a compile fleet inflated within-pair sd to 40pp (run discarded), which led to
the per-pair quiet gate in ab.sh and the stricter launch waiter.
