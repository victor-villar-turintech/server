# Profiling capability on this host — VTune Phase 0 findings

Established 2026-09-07. Purpose: determine what microarchitectural attribution
we can actually produce for the Artemis agents, before building the collection
pipeline on assumptions.

## VTune is already installed

`/opt/intel/oneapi/vtune/2026.3/bin64/vtune` — **VTune Profiler 2026.3.0**,
system-wide. No install needed; the planned user-level install is moot.
All four analyses we wanted are present: `hotspots`, `uarch-exploration`,
`memory-access`, `threading`.

Driverless collection works **without root** (`perf_event_paranoid=0`), and
`uarch-exploration` reports `Collector Type: Event-based sampling driver`, so
the full PMU is reachable.

## The constraint: TMA is P-core only in VTune

CPU is **Intel Core Ultra X7 368H, "Pantherlake-P"** with three core classes
and two PMUs: `cpu_core` (type 4, P-cores 0-3) and `cpu_atom` (type 10,
E-cores 4-11 and LPE-cores 12-15).

| Collection | E-cores (4-11) | P-cores (0-3) |
|---|---|---|
| VTune hotspots | works | works |
| VTune uarch-exploration | **`TOPDOWN.SLOTS` = 0, no TMA** | **full TMA hierarchy** |

On P-cores VTune produces the complete breakdown - Retiring / Front-End Bound /
Back-End Bound / Bad Speculation, with sub-levels down to ICache misses, ITLB
misses, branch resteers, DSB switches, FP vs integer uOp mix. On E-cores the
topdown events return zero: VTune 2026.3 appears not to carry metric
definitions for this new atom variant.

**This matters because our benchmark server is pinned to CPUs 4-7 - E-cores.**
Profiling on P-cores would describe a different microarchitecture than the one
every benchmark number comes from.

## The resolution: perf gives level-1 TMA on E-cores

The kernel *does* expose topdown events on `cpu_atom`, and perf reads them:

```
taskset -c 4 perf stat -e cpu_atom/topdown-retiring/,cpu_atom/topdown-be-bound/,\
cpu_atom/topdown-fe-bound/,cpu_atom/topdown-bad-spec/ <workload>
```

Measured on a synthetic memory-bound + compute workload:

| Category | E-core (perf) |
|---|---|
| Retiring | 12.14% |
| Back-End Bound | 87.61% |
| Front-End Bound | 0.19% |
| Bad Speculation | 0.06% |

So the hardware and kernel support level-1 TMA on the cores we benchmark on;
only VTune's metric layer is missing for them.

### Encouraging cross-check

VTune on a P-core reported **Retiring 12.2%** for the same workload; perf on an
E-core gives **12.1%**. That is one workload, not proof - but it suggests the
level-1 split is not wildly uarch-dependent, which supports using P-core
deep-dives to inform E-core work.

## Adjusted three-instrument plan

| Question | Instrument | Cores |
|---|---|---|
| Which functions are hot? | VTune hotspots (or existing perf) | **E-cores 4-7**, matching the benchmark |
| Which TMA category? | **perf topdown** | **E-cores 4-7**, matching the benchmark |
| Why, in detail (which cache, which stall)? | VTune uarch-exploration | P-cores 0-3, **different uarch - directional only** |

Each tool is used only where it is valid. The first two run on exactly the
cores every benchmark number comes from; the third is a drill-down whose
proportions must be treated as indicative rather than authoritative.

## Standing caveats

- **Never profile concurrently with a benchmark or the Artemis runner.**
  Collection is itself load; the same quiet-host rule applies
  (`harness/check-quiet.sh`).
- **Profile the pinned checkout's build** (`$MDB_ROOT/build/*`), not runner
  builds: `artemis-build.sh` sets `CCACHE_NOHASHDIR` for cross-clone cache
  hits, which can leave debug paths pointing at deleted temp directories and
  corrupt source-line attribution.
- The thermal ceiling (98-100 C, ~1.5 GHz effective) compresses the
  memory-vs-core balance relative to an unthrottled server, so category
  proportions are directional. Function *ranking* is more robust than the
  category split.
- Each VTune collection is roughly 1 GB; `results/` already gitignores large
  artefacts, and only distilled reports get committed.
