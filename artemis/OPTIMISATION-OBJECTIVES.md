# Optimisation objectives — MariaDB server, collation hash path

**Status:** frozen for Discovery · **Prepared:** 2026-09-08 · **Updated:** 2026-09-22
(primary target moved from workload 9 to the new collation-heavy workload 12;
see `findings/17-collation-heavy-workload.md`)
**Scope of this folder:** everything an optimising agent needs to attack one
well-characterised hotspot in MariaDB server, prove the change correct, and
prove the improvement is real. Nothing here modifies server source.

---

## 1. Frozen codebase state

| Item | Value |
|---|---|
| Repository | `<fork>/server` (fork of `MariaDB/server`) |
| Baseline branch | `perf/baseline` |
| **Seed commit** | `b2a8c2234dbc276fe3947633c9ffea454badf48f` |
| Server version | MariaDB **13.1.0** (`SERVER_MATURITY=alpha`, upstream `main` at freeze) |
| Submodules | only `libmariadb` (`5a6fe6016fc908518e7d2a8c128a5c108b511c17`); all large engine submodules disabled |
| This branch | `artemis/discovery-prep` = seed + this `artemis/` folder, **no source changes** |

Every benchmark number and every profile in this folder was produced from the
seed commit above. A Discovery run's `baselineVersionSha` must resolve to it.

### Build configuration (fixed - do not change)

`RelWithDebInfo`, Ninja, ccache, gcc 15.2.0, cmake 3.26.0, ninja 1.13.2.
Disabled: `COLUMNSTORE ROCKSDB MROONGA SPIDER CONNECT OQGRAPH SPHINX DUCKDB S3`,
`WITH_WSREP=OFF`, system SSL and zlib, unit tests on. Full flag list in the
runner's `build.sh`; candidates **must not** alter build flags, the build
system, or compiler options - the objective is a source-level change.

### Runtime under test

Linux 7.0.0-28, Intel Core Ultra X7 368H (hybrid; server pinned to E-cores
4-7, load generator on 8-11). InnoDB 2 GB buffer pool, dataset 8 tables x
100,000 rows (fits in memory - this is a CPU study, not an I/O study).
Default server collation **`utf8mb4_uca1400_ai_ci`**; sysbench's `CHAR(120)`
and `CHAR(60)` columns inherit it, which is why the collation hash is on the
hot path.

Since 2026-09-22 the dataset also holds `sbtext`: 500,000 rows of
deterministic multilingual text (accented Latin, Cyrillic, Greek, CJK, plain
ASCII) with `name VARCHAR(128)` (60-128 chars, ~35% repeats), `body
VARCHAR(1024)` (200-600 chars) and `tag VARCHAR(64)` (60 distinct labels),
same collation. It is the table behind workload 12.

---

## 2. Objective

**Primary metric (from 2026-09-22):** `target_tps` = `tps_collation_heavy`,
the `tps` of **workload_id 12** (`collation_heavy`) in `artemis_results.json`.
**Higher is better.** Workload 12 is a read-only mix over `sbtext`, each query
on a random window of 100 consecutive ids: 3x `SELECT DISTINCT name`, 2x
`GROUP BY name`, 1x `GROUP BY tag`, 1x `ORDER BY body LIMIT 20`, 1x self-join
on `name` equality. DISTINCT and GROUP BY build MEMORY temp tables whose hash
index calls the UCA `hash_sort` on every row; ORDER BY builds sort keys
(`strnxfrm`); the join compares (`strnncollsp`). About **43% of server CPU is
inside the four target files** on this workload (scanner 19%, hash_sort 12%,
compare 7%, sort keys 2%), against ~18% for the hash step on workload 9.

The original primary metric, workload 9 (`oltp_distinct_ranges`, TAF
`SELECT_DISTINCT_RANGES`), is retained as a guard so results stay comparable
with the 2026-09 pilot, whose 20 A/B-tested candidates all measured within
+0.13% on it.

**What counts as a win:** the improvement must survive an interleaved, repeated
A/B against the baseline with its **entire 95% confidence interval above the
noise floor**. Measured cross-build noise on this host is ~3% at 10 pairs, so
effects below ~3% are not provable here; do not report them as wins. Inside a
Discovery run every version's benchmark is repeated 10 times and the baseline
is re-measured after the run, which bounds drift but not build-layout noise.

**Guards - must not regress** (within noise, `errors = 0`):

| workload_id | workload | why it is a guard |
|---|---|---|
| 9 | `oltp_distinct_ranges` | the pilot's target; ASCII-only DISTINCT, ties the new result to the old |
| 1 | `oltp_read_only` | headline blend; a real collation win shows here *diluted* |
| 5 | `oltp_point_select` | pure framework path; shares no mechanism with the target |
| 10 | `tpcb_key` | MariaDB's most regression-sensitive workload class |

A candidate that improves workload 12 and regresses any guard beyond noise is a
**reject**, not a trade-off.

**Falsifiable prediction:** a genuine collation-path improvement moves workload
12 strongly, workload 9 in the same direction but less (its DISTINCT hashes 120
single-weight ASCII characters per row), and workload 1 detectably. If 12
improves and 9 and 1 are perfectly flat, the mechanism is not what this
document claims and the result should be doubted.

---

## 3. Hard constraints (each is a gate; failing any is a hard reject)

1. **Bit-exact hash output.** The collation hash determines **partition
   placement for existing tables**. Any change must produce identical
   `(nr1, nr2)` for every input. Enforced by the differential gate in
   `gates/hashcheck/` - 70,868 vectors across 7 collations, run **before**
   the test suite. The two bytes of each collation weight are deliberately
   combined in a non-obvious order for compatibility; that order is part of
   the contract. This is an optimisation of *how* the same value is computed,
   never *what* is computed.
2. **`mysql-test` green:** suites `main,innodb`, **1,848 tests**, zero
   failures. `main.partition_key_algorithm`, `main.ctype_uca_partitions` and
   `innodb.partition_locking` are load-bearing for this target.
3. **Small, reviewable patch.** The acceptance bar is a diff a MariaDB engineer
   can review in minutes. Changes confined to the target files in §4. No new
   abstractions, no API changes, no new dependencies.
4. **No changes to tests, benchmarks, build system, or this folder.** These
   define the judgement; they are not part of the search space.
5. **C/C++ only, portable.** No inline assembly, no platform-specific
   intrinsics without a portable fallback, no behaviour change on other
   architectures.

---

## 4. Target

### Files (the search space)

| File | What is there |
|---|---|
| `include/m_ctype.h` | `MY_HASH_ADD_MARIADB` macro (line 551) - the hash step |
| `strings/strings_def.h` | `MY_HASH_ADD` / `MY_HASH_ADD_STR` (lines 203-225) - per-byte wrapper with the hasher dispatch branch |
| `strings/ctype-uca.inl` | `hash_sort` template (line 649) - the loop that feeds weights into the hash, including the trailing-space run logic |
| `strings/ctype-uca.c` | `my_uca_scanner_next_expansion_weight` (line 31283) and the scanner that produces weights |

### The hot code

`strings_def.h:209` is the single hottest source line in the study (42.5 s of a
60 s collection). It expands to:

```c
#define MY_HASH_ADD_MARIADB(A, B, value) \
  do { A ^= (((A & 63) + B) * (value)) + (A << 8); B += 3; } while(0)
```

called twice per collation weight (`s_res >> 8`, then `s_res & 0xFF`) from the
`hash_sort` loop, through this wrapper:

```c
static inline void MY_HASH_ADD(my_hasher_st *hasher, uchar value)
{
  if (hasher->m_hash_byte)            /* NULL for the default hasher */
    hasher->m_hash_byte(hasher, value);
  else
    MY_HASH_ADD_MARIADB(hasher->m_nr1, hasher->m_nr2, value);
}
```

### Why it is slow - the mechanism (from profiling, see §6)

- **Retiring-bound, CPI 0.34, 62% of cycles issuing to 3+ ports, 0.0% vector
  uOps.** The core is executing efficiently; there is simply too much scalar
  work per byte. Memory is *not* the problem: L2 0.4%, L3 1.6%, DRAM 2.1%.
- **`A` is a loop-carried dependency chain through a multiply.** Each byte's
  `A` depends on the previous byte's `A`, and the multiply's latency sets a
  floor per byte regardless of free execution ports. That is why it is
  retiring-bound yet cannot go faster.
- **`B` is trivially predictable:** `B += 3` each step, so `B_n = B_0 + 3n`.
  It does not need to be carried through the chain.
- **The `m_hash_byte` branch is loop-invariant.** `hasher` does not change while
  one string is hashed, and the pointer is `NULL` for the default hasher, yet
  it is loaded and tested twice per weight inside the hot loop.
- The feeding loop, `my_uca_scanner_next_expansion_weight`, is the second
  hotspot (16.3 s) and produces one weight per call.

### Candidate directions, ranked by confidence

1. Hoist the `m_hash_byte` dispatch out of the per-byte path (two loop
   variants, decided once per call). Bit-exact by construction.
2. Strength-reduce `B` (compute from index instead of carrying it).
3. Fuse the two per-weight `MY_HASH_ADD` calls into one algebraically
   equivalent 16-bit step to shorten the critical path. Highest potential,
   highest risk; the gate decides.
4. Reduce per-weight overhead in the scanner/feeding loop. On workload 12
   `my_uca_scanner_next_utf8mb4` is the **largest** single hotspot (19% of
   server CPU, called once per weight from the hash, compare and sort-key
   loops): streamline the common single-weight case (no expansion, no
   contraction), keep scanner state in registers, and keep the contraction
   check off the fast path when the character has no contraction flag. What
   any input produces must not change, only how fast it is produced.

Anything in this list is a hypothesis, not an instruction. The gates and the
benchmark are the arbiters.

---

## 5. Out of scope

- **`tpcb_key`, `tpcb_no_key`, `point_select` as optimisation targets.**
  Profiling shows they are bound by aggregate *instruction footprint* across
  the SQL/handler/InnoDB path (uop-cache coverage 22% / 16% / 5.9%, iTLB misses
  21-25%). The remedies are PGO/BOLT, huge pages or broad inlining changes -
  none is a small reviewable patch. They are **guards**, not targets.
- Lock contention: not present at the 4-thread operating point (spin ~0).
- Build flags, compiler upgrades, PGO, link-order tricks.
- Any change to collation *semantics*, sort order or comparison results.

---

## 6. Evidence

Full derivation in `findings/`; the single consolidated reference is
`findings/14-phase1-consolidated.md`. Machine-readable hotspots for the agent:
`profiling/matrix/hotspots.json`, keyed by the same `workload_id` as
`artemis_results.json`.

Level-1 top-down on the benchmark cores (E-cores, perf):

| id | workload | Retiring | Back-End | Front-End | Bad Spec | CPI | bound by |
|---|---|---|---|---|---|---|---|
| 9 | `distinct_ranges` | 39.7% | 33.1% | 19.9% | 7.3% | 0.34 | **retiring** |
| 1 | `oltp_read_only` | 32.1% | 26.4% | 35.0% | 6.6% | 0.42 | mixed |
| 10 | `tpcb_key` | 16.8% | 25.4% | 52.0% | 5.7% | 0.83 | front-end |
| 11 | `tpcb_no_key` | 16.6% | 26.1% | 52.2% | 5.0% | 0.85 | front-end |
| 5 | `point_select` | 16.0% | 21.9% | 58.4% | 3.6% | 0.87 | front-end |

Hottest MariaDB functions on workload 9 (VTune, profiler overhead excluded):

| CPU s | % | function | source |
|---|---|---|---|
| 39.24 | 18.0% | `MY_HASH_ADD` | strings_def.h |
| 17.08 | 7.8% | `my_uca_scanner_next_expansion_weight` | ctype-uca.c |
| 9.87 | 4.5% | `my_charlen_utf8mb4` | ctype-utf8.c |
| 8.89 | 4.1% | `my_ismbchar` | m_ctype.h |
| 5.92 | 2.7% | `my_uca_level_booster_simple_prefix_cmp` | ctype-uca.c |
| 5.34 | 2.5% | `skip_trailing_space` | strings_def.h |

Three independent instruments agree - the original perf profile, level-1 TMA
on the benchmark cores, and a P-core level-2 drill-down - on target and
mechanism. Level-1 agreement between core types is within ~3 points.

---

## 7. Evaluation pipeline and cost

Cheapest rejection first. All commands live on the runner host at pinned paths.

| Stage | Command | Cost | Rejects |
|---|---|---|---|
| Build | `/home/artemis-ai/mariadb/runner/artemis-build.sh` | ~30 s warm, ~190 s cold | compile errors |
| **Gate 0** hash exactness | inside `artemis-test.sh` | ~1 min | any changed hash value |
| Gate 1 correctness | `/home/artemis-ai/mariadb/runner/artemis-test.sh` | ~6 min | any mtr failure |
| Benchmark (search) | `/home/artemis-ai/mariadb/runner/artemis-bench-discovery-C.sh` | ~9 min | target 12 at 4 reps, guards 9/10/5/1 at 2 reps; writes flat `artemis_results.json` |
| Benchmark (full) | `/home/artemis-ai/mariadb/runner/artemis-bench-suite.sh` | ~37 min | all 12 workloads, for final reporting |

**Per candidate in Discovery: ~15 min per benchmark execution.** With the
10-repeat protocol used from 2026-09-22 a version costs ~1 h 45 min and a
10-version run ~21 h.

### `artemis_results.json` contract

Discovery runs use the **flat** form (one object, per-workload metric names):
`target_tps`, and for each workload `<field>_<short>` with short names
`read_only`, `point_select`, `distinct_ranges`, `tpcb_key`, `collation_heavy`,
e.g. `tps_collation_heavy`, `latency_p95_ms_collation_heavy`,
`errors_collation_heavy`. `target_tps` duplicates `tps_collation_heavy`.

The full suite writes the array form, one object per workload, every value a
finite number:

```json
{ "workload_id": 9, "tps": 14679.66, "qps": 44038.99,
  "latency_avg_ms": 0.27, "latency_p95_ms": 0.29, "tps_cv_pct": 1.50, "errors": 0 }
```

`tps` = mean of 4 measured 30 s reps after a discarded warm-up (guards: 2
reps). `tps_cv_pct` is the spread across reps; above ~5% treat that row as
unreliable for that run. `errors` counts failed reps; must be 0.

Workload ids: 1 `oltp_read_only` · 2 `oltp_read_write` · 3 `oltp_update_index`
· 4 `oltp_update_non_index` · 5 `oltp_point_select` · 6 `oltp_simple_ranges` ·
7 `oltp_sum_ranges` · 8 `oltp_order_ranges` · **9 `oltp_distinct_ranges`** ·
10 `tpcb_key` · 11 `tpcb_no_key` · **12 `collation_heavy`** (added
2026-09-22, not a TAF workload). Workloads 1-11 and the sysbench 1.1.0 client
are MariaDB Foundation's own TAF definitions (`github.com/MariaDB/TAF` @
`ee742552`); legacy suites are excluded.

### Baseline reference values (seed commit, 4-rep protocol)

| id | tps | id | tps |
|---|---|---|---|
| 1 | 5,137.8 | 9 | 14,679.7 |
| 5 | 226,643 | 10 | 13,594.4 |
| 12 | **1,775.3** (2026-09-22, fresh host) | | |

Values for 1/5/9/10 are from the 2026-09-08 freeze on a thermally degraded
host; a fresh host measured 7,529 / 248,947 / 23,233 / 19,348 on 2026-09-22.

Absolute numbers from this host are **not** quotable externally (thermally
constrained mobile CPU); only relative A/B comparisons are meaningful.

---

## 8. Measurement caveats the agent should know

- The host throttles to ~1.5 GHz under sustained load; profiles were taken at
  ~3.7 GHz. Function *ranking* is robust; the front-end/back-end split would
  shift somewhat at benchmark clock.
- 4 threads throughout. Contention effects at high thread counts are not
  visible here.
- Cross-build noise is ~3% at 10 pairs: two independent builds of identical
  source differ by that much from code-layout effects alone. This is why
  small wins cannot be claimed and why the benchmark repeats and interleaves.
- Four false results were caught while building this pipeline - a 33% phantom
  regression (thermal), an 8x phantom slowdown (core contention), a 15-19%
  phantom hotspot (the profiler's own collector), and a clean-looking profile
  of an idle server. Each is now a guard. Treat any surprisingly large effect
  with suspicion until it survives the A/B.

---

## 9. Reference index

| | Path |
|---|---|
| **Consolidated findings** | `findings/14-phase1-consolidated.md` |
| Agent-facing hotspots | `profiling/matrix/hotspots.json` |
| Human hotspot summary | `profiling/matrix/summary.md` |
| Level-1 TMA per workload | `profiling/matrix/*.topdown.txt` |
| Source-line hotspots | `profiling/matrix/*.srclines.csv` |
| P-core level-2 drill-downs | `profiling/pcore-drilldown/<workload>/` |
| Threading analysis | `profiling/threading/` |
| Hash-exactness gate (reference copy) | `gates/hashcheck/` |
| Benchmark method & noise floor | `findings/02-benchmark-method.md`, `findings/03-noise-floor.md` |
| Loop cost model | `findings/05-loop-budget.md` |
| Benchmark inventory (TAF) | `findings/06-benchmark-inventory.md` |
| Instrument validity on this CPU | `findings/07-profiling-capability.md` |
| Per-workload drill-downs | `findings/09-*`, `10-*`, `11-*` |
| Hash gate rationale & validation | `findings/15-hashcheck-gate.md` |
| Collation-heavy workload (12): design, profile, noise floor | `findings/17-collation-heavy-workload.md` |

The gate that actually runs is the copy on the runner host
(`/home/artemis-ai/mariadb/harness/hashcheck/`), outside this repository, with
its 16 MB reference fingerprint. The copy here is for reading, not execution.
