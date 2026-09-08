# MariaDB's publicly available benchmarks — inventory

Compiled 2026-09-04. Purpose: answer open question #1 (which benchmark does
MariaDB engineering trust) as far as possible from public sources, so the call
with MariaDB engineering can confirm a choice rather than start the discussion.

---

## A. Shipped in the MariaDB/server repo

Already cloned at `src/`.

| What | Path | Status |
|---|---|---|
| **mysql-test / mtr** | `mysql-test/` | 53 suites. **Correctness, not performance.** 1848 tests in `main,innodb` pass on our baseline in 5.9 min. This is the correctness gate, not a benchmark. |
| **sql-bench** | `sql-bench/` | MariaDB's legacy benchmark suite. 10 programs (`test-select`, `test-insert`, `test-connect`, `test-alter-table`, `test-create`, `test-transactions`, `test-big-tables`, `test-table-elimination`, `test-ATIS`, `test-wisconsin`) plus `run-all-tests.sh`, `compare-results.sh`, `graph-compare-results.sh`. Single-threaded, comparison-oriented. README still references MySQL 3.20 and Solid Server; last substantive change was in 2023 (PostgreSQL 14.9 support). **Archaic but not abandoned.** |
| **check_costs.pl** | `tests/check_costs.pl` | **Written by MariaDB Foundation, 2022.** Measures whether optimizer cost calculations are reasonable, for tuning cost variables. 12 access-path microbenchmarks — table scan (4 where-clause variants), plain table scan, index scan, index scan 4 parts, range scan, `eq_ref_index_join`, `eq_ref_cluster_join`, `eq_ref_join`, `eq_ref_btree` — run across aria/innodb/myisam/heap. Has built-in `--gprof` and `--test-runs` averaging. Uses a DBT3 `lineitem`-like table. |
| **ftbench** | `storage/myisam/ftbench/` | MyISAM fulltext search benchmark. |
| **RQG configs** | `randgen/conf/` | Random Query Generator configurations. |
| **large_tests / stress / perfschema_stress** | `mysql-test/suite/` | Long-running and stress suites, not throughput benchmarks. |

---

## B. TAF — MariaDB Foundation's current benchmark framework

**https://github.com/MariaDB/TAF** — cloned at `taf/` (304 MB).
Perl, deterministic, plugin-based, fully open source, actively developed
(HEAD 2026-08-16). Alpha announced 2025-ish, **2.5 BETA current**.

This is the single most important find: it is MariaDB's *own* benchmark
automation, which is exactly what the client asked results to be validated against.

**Test suites:** `sysbench-lua.pm`, `hammerdb-tprocc.pm` (TPROC-C / TPC-C),
`hammerdb-tproch.pm` (TPROC-H / TPC-H), plus a template.

**Bundled clients:** its own sysbench **1.1.0** (not distro 1.0.20),
HammerDB 6.0, and BMK-kit.

**sysbench test registry** (`test_suites/sysbench-lua.pm`):

*Standard (`@stdTests`, the default set):*
`OLTP_RO`, `OLTP_RW`, `UPDATE_KEY`, `UPDATE_NO_KEY`, `POINT_SELECT`,
`SELECT_SIMPLE_RANGES`, `SELECT_SUM_RANGES`, `SELECT_ORDER_RANGES`,
**`SELECT_DISTINCT_RANGES`**

*Extended (`@stdTestsExt`):* `DELETE`, `HOT-POINTS`, `INSERT`,
`OLTP_INSERT_INTO`, `OLTP_RO_MODIFIABLE`, `OLTP_RW_MODIFIABLE`,
`OLTP_WO_MODIFIABLE`, `OLTP_RW_PS_ONLY_MODIFIABLE`, `PARSER`, `PARSER-RO`,
`POINT_SELECT_MODIFIABLE`, `POINTS-COVERED-PK/SI`, `POINTS-NOTCOVERED-PK/SI`,
`RANDOM-POINTS`, `RANGE-COVERED-PK/SI`, `RANGE-NOTCOVERED-PK/SI`, `SCAN`,
`SELECT_*_RANGES_MODIFIABLE`, **`TPCB_KEY`**, **`TPCB_NO_KEY`**,
`UPDATE_*_TRANSACTIONAL_MODIFIABLE`

*BMK groups:* `BMK_RW_UPDATE_RANGE`, `BMK_WO_UPDATE_RANGE`,
`BMK_RW_UPDATE_INDEX_RANGE`, `BMK_RW_UPDATE_NON_INDEX_RANGE`,
`BMK_RW_PS_*`, `CONNECT`, plus secondary-index and update-range variants.

**Default protocol** (`properties/default/sysbench_lua_default.properties`):

| Parameter | TAF default | Our harness today |
|---|---|---|
| duration | **300 s** | 30 s |
| threads | **8,16,32,64,128,246,512,1024** | 4 |
| rows | 1,000,000 | 800,000 (8 x 100k) |
| tables | 1 | 8 |
| engine | InnoDB | InnoDB |
| distribution | uniform | uniform |
| sysbench | bundled 1.1.0 | distro 1.0.20 |

**Profiling:** TAF ships a `perf` plugin (`libs/profile_libs/Perf.pm`) with
per-iteration profiling and automatic flamegraph generation — the same thing
our `profile.sh` does.

**TPC-B:** `TPCB_KEY` / `TPCB_NO_KEY` were added in June 2026 by Jonathan
Miller (MariaDB Foundation). Three point selects, three updates, one insert.
The announcement claims TPC-B is *"simple, direct, and very sensitive to
performance changes"* and that historically **"seventy percent of all
regressions came from TPC-B"**. If that holds, it is the highest-sensitivity
workload they have, and a strong candidate for our A/B metric.

---

## C. Published documentation and results (mariadb.com)

The "Benchmarks and Long Running Tests" section lists 15 pages:
Benchmark Builds; Benchmarking Aria; DBT3 Automation Scripts; DBT3 Benchmark
Results InnoDB; DBT3 Benchmark Results MyISAM; DBT3 Example Preparation Time;
MariaDB 5.3 Async I/O on Windows with InnoDB; MariaDB 5.3/MySQL 5.5 Windows
performance patches; mariadb-tools; Performance of MEMORY Tables; Recommended
Settings for Benchmarks; RQG Performance Comparisons; run-sql-bench.pl;
Segmented Key Cache Performance; sysbench Benchmark Setup.

The documented **sysbench setup** page is legacy (sysbench 0.5 naming:
`oltp_simple.lua`, `oltp_complex_ro.lua`, `update_index.lua`, ...; Launchpad
`lp:sysbench` and `lp:mariadb-tools`): 5-minute runs, 2,000,000 rows,
1/4/8/16/32/64/128 threads, 1 GB buffer pool, 256 MB logs, O_DIRECT,
doublewrite off. **Superseded in practice by TAF** but useful as evidence of
the protocol shape they have long used.

---

## D. External benchmarks MariaDB engages with

- **HammerDB** (TPROC-C, TPROC-H) — integrated as a TAF suite. HammerDB
  published work with MariaDB engineering in 2025 on scaling bottlenecks
  ("Scaling Databases in the Chiplet Era").
- **MariaDB Enterprise Server 11.8** — published Oct 2025 claiming 2.5x OLTP
  improvement.
- **Percona MySQL ecosystem benchmark reports** (2026) — third-party, includes
  MariaDB.
- **OpenBenchmarking / Phoronix `pts/mariadb`** — third-party, sysbench-based.
- **buildbot.mariadb.org** — CI across 100+ builds, primarily correctness
  coverage rather than throughput benchmarking.

---

## Implications for our work

1. **Open question #1 is largely answered.** TAF is MariaDB's own framework
   and satisfies the client's "validated by MariaDB's own tests and benchmarks"
   requirement directly. The conversation with MariaDB engineering becomes *confirm TAF and pick the
   workload*, not *ask what they use*.

2. **Our current hotspot sits on a workload they measure in isolation.**
   `SELECT_DISTINCT_RANGES` is in TAF's default standard test set, and the
   profile hotspot `my_uca_hash_sort_utf8mb4` (7.80% self) is driven by exactly
   that DISTINCT path. That is a meaningful coincidence in our favour — but see
   caveat 4.

3. **TPC-B may be the better metric.** If the Foundation's 70% claim holds, a
   result on `TPCB_KEY`/`TPCB_NO_KEY` carries more weight with them than one on
   a read-only workload. Worth asking MariaDB engineering directly.

4. **Their protocol needs far more machine than we have.** TAF sweeps 8 to
   1024 threads at 300 s per point. Our runner has 4 pinned server cores and is
   thermally throttled at 98-100 C. **We cannot reproduce their protocol on this
   host** — a single TAF-default sweep would take hours and be meaningless at
   1024 threads on 4 cores. This is now the strongest concrete argument for
   dedicated bare metal, and it is a hardware requirement rather than a
   preference.

5. **Cheap alignment available now.** Switching our harness to 1 table x 1M
   rows and TAF's sysbench 1.1.0 costs almost nothing and removes an easy
   objection. Matching duration and thread sweep does not fit the current host.

## Open questions for MariaDB engineering

- Confirm TAF is the framework they want results in, and which TAF test.
- Confirm whether TPC-B really is their most regression-sensitive workload.
- Confirm thread count and duration they consider meaningful — and therefore
  what hardware the pilot result has to be produced on.
- Confirm the branch (we are on `main` = 13.1.0 alpha).
