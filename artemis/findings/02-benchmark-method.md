# Benchmark method — the protocol every result must pass

The project brief makes one requirement non-negotiable: MariaDB has already
challenged benchmark credibility ("is this signal or noise?"). This file is the
answer, written down before any number exists, so that no result can be accused
of having had its statistics chosen after the fact.

## The two gates

A candidate patch is only reportable if it passes BOTH, in this order:

**Gate 1 — correctness.** MariaDB's own test suite green, query results
identical. Run by `harness/mtr.sh`. A failing candidate is discarded and no
performance number is computed for it, however fast it looked. Fast-and-wrong
is not a partial success.

**Gate 2 — significance.** The improvement survives repeated, interleaved
measurement and clears the measured noise floor. Run by `harness/ab.sh`, gated
by `harness/stats.py`.

## Controlling the machine

The runner is a mobile Intel Core Ultra X7 368H with three core classes
(4.9-5.0 / 4.0 / 3.6 GHz), powersave governor and turbo enabled. Uncontrolled,
scheduling luck alone spans ~39% of clock. Controls applied:

| Threat | Control |
|---|---|
| Cross-class core migration | `taskset` server to CPUs 4-7, client to 8-11 — same class, disjoint sets |
| Server/client contention | Only one server runs at a time; client cores never overlap server cores |
| Cold caches, buffer pool fill | Fixed warm-up run, discarded before measurement |
| Disk I/O polluting a CPU measurement | Buffer pool sized above the dataset; `flush_log_at_trx_commit=2`; binlog off; doublewrite off |
| Thermal drift over a run series | **Interleaved ABBA ordering**, never blocked |
| Frequency variance | `performance` governor + `no_turbo=1` — **pending root, see 01-open-questions.md** |

## Why ABBA ordering

Thermal drift on a mobile part is monotonic across a long run series. A blocked
design — all baseline runs, then all candidate runs — charges the entire drift
to whichever arm ran second. Alternating within-pair order (A B / B A / A B …)
places both arms at the same average point in the drift, cancelling it to first
order. This is a design control, not a statistical correction, which makes it
much harder to argue with.

## Why these statistics

Implemented in `harness/stats.py`, stdlib only, validated against five
synthetic cases (identical distributions, true +5%, true +0.5% inside the noise
floor, true -6%, and a lower-is-better latency metric).

- **Median, not mean.** Interference on a live machine is right-skewed; the
  mean chases those tails and the median does not.
- **Mann-Whitney U, not Student's t.** We are unwilling to assert normality of
  throughput samples, and at N in the tens that assumption would be doing real
  work we cannot check.
- **Bootstrap 95% CI on the relative median difference, reported as the
  headline.** A p-value says "these differ"; an engineer deciding whether to
  spend review time needs "by how much, and how sure". The CI answers that.
- **A measured noise floor is a required input.** Statistical and practical
  significance are different claims: with enough runs a 0.3% effect becomes
  "significant" while remaining indistinguishable from the machine. A result is
  called a WIN only if the entire confidence interval clears the noise floor.

That last rule is deliberately conservative — it will throw away some real
improvements. That is the correct trade for this project: one defensible
number is worth more than three arguable ones.

## Noise floor is the gating experiment

`harness/noise-floor.sh` runs the same binary against itself — verified
bit-identical by sha256 — through the entire harness, including separate
datadirs and full server restarts. Whatever spread it reports is noise by
construction.

That number is a go/no-go on the host itself. If the noise floor is wide
relative to the effect sizes we expect from small patches, this laptop cannot
be the hardware target and we should say so early rather than defend a laptop
measurement in front of MariaDB's server team.

## Provenance

Every `ab.sh` run writes a `manifest.txt` capturing git SHA, workload and its
parameters, thread count, warm-up and measurement duration, core pinning,
buffer pool size, governor, turbo state, kernel and compiler version — plus
`raw.csv` with every individual measurement in run order. A result that cannot
be reproduced from its manifest is not a result.
