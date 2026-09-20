# artemis/ — optimisation working folder

Everything an optimising agent needs for the MariaDB collation-hash target, in
one place. **Start with `OPTIMISATION-OBJECTIVES.md`.**

```
OPTIMISATION-OBJECTIVES.md   objective, frozen state, constraints, target, evidence, pipeline
findings/                    analysis notes (method, noise floor, profiling, threading, gates)
profiling/
  matrix/                    level-1 TMA + VTune hotspots for 5 workloads; hotspots.json is agent-facing
  pcore-drilldown/<w>/       level-2 TMA + full VTune uarch hierarchy, one folder per workload
  threading/                 lock/wait analysis for the write workloads
gates/hashcheck/             reference copy of the bit-exactness gate (runs from the runner host)
```

This folder contains **no server source changes**. It is documentation and
data only; candidates must not modify it.

## Full pilot record (private)

The complete engagement — build/benchmark harness, VTune profiling, the
multi-model Discovery sweep, the A/B validation, and the executive report — lives
in a private companion repository (access-controlled; not publicly viewable):

➜ https://github.com/victor-villar-turintech/mariadb-perf-pilot

### Executive report

The full evidence report — 31 pages, 20 figures, linked contents page; preparation,
benchmarks and metrics explained for non-MariaDB readers, VTune profiling, the
objective given to the AI, the Discovery sweep, every unmeasured version accounted
for by cause, and all 25 paired A/B validations with confidence intervals — is:

➜ https://github.com/victor-villar-turintech/mariadb-perf-pilot/blob/main/results/ab-campaign/AB-Executive-Report.pdf

A 21-slide walkthrough of the same material for a non-technical audience:

➜ https://github.com/victor-villar-turintech/mariadb-perf-pilot/blob/main/results/ab-campaign/AB-Executive-Deck.pdf

and the presenter edition of the same deck (each slide beside its speaker notes):

➜ https://github.com/victor-villar-turintech/mariadb-perf-pilot/blob/main/results/ab-campaign/AB-Executive-Deck-Presenter.pdf

Headline: 98 candidates from 9 models, 25 taken through paired ABBA A/B —
zero validated speed-ups on the collation hash path; best confirmed effect
+0.13 % (inside the 1 % noise floor). Regeneration scripts for every figure and
table are alongside the PDF in `harness/`.
