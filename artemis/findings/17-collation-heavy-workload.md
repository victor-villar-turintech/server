# 17 — The collation-heavy workload (workload 12)

**Date:** 2026-09-22 · **Status:** measured on the fresh host, adopted as the
Discovery primary target.

## Why a new workload

The 2026-09 pilot targeted workload 9 (`oltp_distinct_ranges`). Twenty
AI-generated candidates, all bit-exact, all passing the 1,848-test suite,
measured between -0.09% and +0.13% in a paired, soaked, ABBA A/B against the
baseline. On that workload the hash step was 18% of server CPU; the implied
speed-up of the hash step itself was therefore under 1%.

Part of that ceiling is the workload, not the code. sysbench's `c` column is
`CHAR(120)` filled with ASCII digits and dashes: every character is a single
byte with a single 16-bit weight, there are no expansions or contractions, and
the column is always full so there is nothing for the trailing-space logic to
do. The per-weight overheads the candidates attacked are a small fraction of a
cheap loop.

Workload 12 was built to put as much server CPU as possible inside the four
target files, exercising the paths the pilot's prompt describes.

## Two things learned while designing it

1. **The UCA `hash_sort` skips trailing spaces.** It walks them with
   `scanner_next` and returns without hashing them (only *interior* space runs
   are replayed into the hash). A padded `CHAR(255)` column therefore adds no
   hashed weights. The first draft used one and the profile was dominated by
   `my_lengthsp_8bit` (27%, `strings/ctype-simple.c`, not a target file) from
   the join cache trimming padding, plus `memmove`. The table uses `VARCHAR`.
2. **A temp-table column over 512 bytes becomes a BLOB** and the DISTINCT or
   GROUP BY spills from the MEMORY engine to on-disk Aria, a different hash
   path. utf8mb4 columns that are hashed must be ≤ 128 characters. Long text is
   used only for ORDER BY, where sort keys go through `strnxfrm`.

## Data: `sbtext`

500,000 rows, generated deterministically (seed 20260922) by
`harness/gen-text-dataset.py` on the runner host and loaded once per datadir
by `harness/load-text.sh` (idempotent, ~17 s).

| column | type | content |
|---|---|---|
| `id` | INT PK | 1..500000 |
| `name` | VARCHAR(128) | 60-128 chars of mixed accented Latin (40%), Cyrillic (20%), Greek (15%), CJK (15%), ASCII (10%) words; 35% of rows repeat one of 500 hot values |
| `body` | VARCHAR(1024) | 200-600 chars, same mix |
| `tag` | VARCHAR(64) | one of 60 labels in five scripts |
| `grp` | INT, indexed | `id / 1000`, reserved |

Collation `utf8mb4_uca1400_ai_ci` (server default), InnoDB, fits in the 2 GB
buffer pool.

## Queries: `collation_heavy.lua`

Per event, each on a random window of 100 consecutive ids, in a transaction:

| count | query | server path exercised |
|---|---|---|
| 3 | `SELECT DISTINCT name` | MEMORY temp table hash index → `my_ci_hash_sort` → `my_uca_hash_sort_utf8mb4` → `MY_HASH_ADD` |
| 2 | `SELECT name, COUNT(*) GROUP BY name` | same hash path |
| 1 | `SELECT tag, COUNT(*) GROUP BY tag` | same, short keys |
| 1 | `SELECT body ORDER BY body LIMIT 20` | filesort sort keys → `strnxfrm` |
| 1 | `SELECT COUNT(*) FROM sbtext a STRAIGHT_JOIN sbtext b ON a.name = b.name` over two windows | block nested loop → `strnncollsp` / level booster |

Session `tmp_table_size` and `max_heap_table_size` are raised to 256 MB so
temp tables never spill (`Created_tmp_disk_tables` = 0 under load). 4 client
threads on CPUs 8-11, server on 4-7, as for every other workload.

## Profile (baseline build, `perf record -F 999 -g`, 25 s at steady state)

| symbol | file | self % |
|---|---|---|
| `my_uca_scanner_next_utf8mb4` | strings/ctype-uca.inl | **19.0** |
| `my_uca_hash_sort_utf8mb4` (MY_HASH_ADD inlined) | strings/ctype-uca.inl | **12.0** |
| `__memmove_avx_unaligned_erms` | libc | 7.9 |
| `my_charlen_utf8mb4` | strings/ctype-utf8.c | 7.5 |
| `my_uca_level_booster_simple_prefix_cmp` | strings/ctype-uca.c | 4.7 |
| `my_charpos_mb` | strings/ctype-mb.c | 4.5 |
| `my_uca_strnncollsp_onelevel_utf8mb4` | strings/ctype-uca.inl | 2.3 |
| `row_search_mvcc` | InnoDB | 2.1 |
| `my_uca_strnxfrm_onelevel_internal_utf8mb4` | strings/ctype-uca.inl | 2.0 |

Inside the target files: **~43%** of server CPU (`scanner_next` lives in `strings/ctype-uca-scanner_next.inl`, added to the target set on 2026-09-23 from the GPT-5.6 Sol run onwards). All collation and
charset code: 56%. Throughput 1,763 transactions/s (17,630 queries/s).

Compared with workload 9, where `MY_HASH_ADD` was 18% and collation ~40%, the
agents now have more than twice the addressable CPU, and the scanner is a
first-class target rather than a feeder.

## Noise floor (A/A, same binary, 600 s soak, 10 ABBA pairs)

`results/ab-noise_a-vs-noise_b-20260922T162426Z` on the runner host, fresh
host (idle ~5 h, 29.6 C at start), `noise_a` and `noise_b` sha256-identical
binaries of the seed build.

| | value |
|---|---|
| within-pair differences (pp) | -0.17 -0.21 -0.78 +0.05 +0.80 -0.19 -0.13 -0.32 -0.27 +0.42 |
| within-pair sd | **0.43 pp** |
| paired median "improvement" on a known null | -0.18% |
| 95% CI on the null | **-0.27% .. +0.14%** |
| Wilcoxon signed-rank | W=19, p=0.43 |
| verdict | NO-EFFECT (correct) |

The same order as workload 9 (0.88 pp in the 2026-09-03 A/A, 0.14-0.41 pp
across the 21 campaign runs), so the adopted 1.0% same-build floor and the
~3% cross-build floor carry over unchanged.

## Runner validation (original code, dev project, script "MariaDB Discovery collation-heavy")

Changeset validation `07f616ca` on 2026-09-22 (empty changeset, original
version 6ce83837, runner panther-lake), the exact path Discovery uses:

| step | result | time |
|---|---|---|
| `artemis-build.sh` | pass (ccache) | 30 s |
| `artemis-test.sh` (hash gate + 1,848 mtr tests) | pass | 5 min 31 s |
| `artemis-bench-discovery-C.sh` | pass, 31 flat metrics written | 9 min 6 s |

| workload | reps | tps | cv |
|---|---|---|---|
| **12 collation_heavy (target)** | 4 | **1,775.3** | 0.10% |
| 9 distinct_ranges | 2 | 23,218.2 | 0.01% |
| 10 tpcb_key | 2 | 19,433.8 | 0.07% |
| 5 point_select | 2 | 256,114.6 | 0.04% |
| 1 read_only | 2 | 7,536.3 | 0.09% |

Guards match the full-suite validation of the same morning (23,233 / 19,348 /
248,947 / 7,529) within the expected same-session spread. The text table was
loaded into the runner's datadir by the prepare step in 12 s.

## Effect on Discovery cost, and the 60-minute cap

The first Discovery run on this workload (2026-09-22 18:09) had its baseline
cancelled by the platform exactly 60 minutes after the validation was created,
with 6 of 10 repeats done: any validation, build and tests included, must
finish inside 60 minutes. The search benchmark was therefore trimmed:
`artemis-bench-discovery-C.sh` runs workload 12 at 4 reps (130 s) and guards
9, 10, 5, 1 at 1 rep plus a warm-up (40 s each), and skips the 120 s soak when
the previous execution ended less than 10 minutes earlier, which is always the
case for back-to-back repeats. Measured on the baseline build: 6.9 min with
the soak, 4.9 min without (target 1,778 and 1,781 tps). Build + tests + 8
repeats ~ 47 min per version; a 10-version run ~9 h.

## Files on the runner host (not in this repository)

`harness/sysbench/collation_heavy.lua` (symlinked into the TAF sysbench lua dir by the suite),
`harness/gen-text-dataset.py`, `harness/load-text.sh`,
`runner/artemis-bench-suite.sh` (workload 12), `runner/artemis-prepare.sh`,
`runner/artemis-bench-discovery-C.sh`, `harness/discovery-task-collation.txt`
(the Discovery prompt), `harness/build-fitness-schema.py` (profile `collation`).
