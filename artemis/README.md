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
