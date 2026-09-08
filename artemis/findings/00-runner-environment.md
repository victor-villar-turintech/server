# Runner environment — baseline facts

Captured 2026-09-03. This file records the hardware/software the benchmark
numbers are produced on. Every result must cite this file.

## Host

| Property | Value |
|---|---|
| CPU | Intel Core Ultra X7 368H, 16 cores / 16 threads, 1 socket, 1 NUMA node |
| RAM | 61 GiB (8 GiB swap) |
| Disk | NVMe, 93 GiB free on / |
| OS | Ubuntu, Linux 7.0.0-28-generic |
| Compiler | gcc 15.2.0 |
| cmake / ninja | 3.26.0 / 1.13.2 |
| ccache | 4.12.3 |

## !! Benchmarking hazard: heterogeneous cores + active frequency scaling

`lscpu -e` shows THREE distinct core classes:

| CPUs | Max MHz | Class |
|---|---|---|
| 0-3   | 4900-5000 | P-cores |
| 4-11  | 4000 | E-cores |
| 12-15 | 3600 | LP-E cores |

Plus:
- scaling_governor = `powersave`
- intel_pstate/no_turbo = 0 (turbo ENABLED)
- mobile/laptop part -> thermal throttling under sustained load is expected

Consequences if not controlled:
1. An unpinned run can land on a 5.0 GHz P-core or a 3.6 GHz LP-E core.
   That is a ~39% clock spread from scheduling luck alone, which dwarfs the
   size of improvement we are hunting for.
2. Turbo + powersave governor make clock a function of temperature and of
   whatever else the machine was doing 30 seconds ago.
3. Sustained OLTP load on a mobile part will thermally drift downward over a
   run series, so run order correlates with result. A naive "all baseline
   runs, then all candidate runs" design would attribute that drift to the
   patch.

This is precisely the "is it signal or noise?" objection MariaDB has already
raised. It must be controlled before any number leaves this machine.

## Mitigations to implement in the harness

- Pin mysqld to a fixed core set and sysbench to a disjoint set (`taskset`).
  Use only same-class cores. Default: server on CPUs 4-7 (E-cores, uniform
  4.0 GHz, 8 of them available so the client gets its own), client on 8-11.
  Rationale for E-cores over P-cores: 8 uniform cores, further from the
  thermal/turbo ceiling, so more stable clock. To be validated by the
  noise-floor measurement.
- Set governor to `performance` and `no_turbo=1` (fixed frequency) for
  benchmark runs. Costs absolute throughput, buys reproducibility. Both
  require root; see 01-open-questions.md.
- INTERLEAVED A/B run order (B A A B / A B B A), never blocked, so thermal
  drift and background noise hit both arms equally.
- Fixed warm-up period discarded before measurement.
- Report median + interquartile range + a significance test, never a single
  before/after pair.

## Noise floor is the gating experiment

Before ANY optimisation result is trusted, run baseline-vs-baseline: the same
binary against itself, N repeats, through the full harness. The spread of that
result IS the noise floor. Any candidate improvement smaller than it is
unreportable.

If the noise floor on this host is too wide to resolve the effect sizes we
care about, this host cannot be the hardware target and we need a dedicated
bare-metal runner. That is a go/no-go finding, not a detail.
