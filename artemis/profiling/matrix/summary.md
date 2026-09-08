# VTune / perf profiling — Phase 1 results

Level-1 TMA from perf on the pinned E-cores (cores the benchmark runs on);
function attribution from VTune hotspots against the same process.

| id | workload | Retiring | Back-End | Front-End | Bad Spec | CPI | bound by |
|---|---|---|---|---|---|---|---|
| 9 | oltp_distinct_ranges | 39.7% | 33.1% | 19.9% | 7.3% | 0.34 | **retiring** |
| 10 | tpcb_key | 16.8% | 25.4% | 52.0% | 5.7% | 0.83 | **front_end** |
| 11 | tpcb_no_key | 16.6% | 26.1% | 52.2% | 5.0% | 0.85 | **front_end** |
| 5 | oltp_point_select | 16.0% | 21.9% | 58.4% | 3.6% | 0.87 | **front_end** |
| 1 | oltp_read_only | 32.1% | 26.4% | 35.0% | 6.6% | 0.42 | **front_end** |

## Actionable hotspots (MariaDB code only)

VTune collector time (`libtpsstool.so`) is excluded entirely - it is
measurement overhead, not server work. libc/kernel syscall time is
reported separately: not patchable, but its size signals I/O behaviour.

### [9] oltp_distinct_ranges — bound by retiring

*already retiring efficiently: only fewer instructions helps - strength reduction, batching, SIMD, or removing redundant work*

excluded: collector 11.6s, syscall/libc 22.7s

| CPU s | % | function | source |
|---|---|---|---|
| 39.24 | 18.0% | `MY_HASH_ADD` | strings_def.h |
| 17.08 | 7.8% | `my_uca_scanner_next_expansion_weight` | ctype-uca.c |
| 9.87 | 4.5% | `my_charlen_utf8mb4` | ctype-utf8.c |
| 8.89 | 4.1% | `my_ismbchar` | m_ctype.h |
| 5.92 | 2.7% | `my_uca_level_booster_simple_prefix_cmp` | ctype-uca.c |
| 5.34 | 2.5% | `skip_trailing_space` | strings_def.h |
| 4.40 | 2.0% | `my_uca_scanner_next_utf8mb4` | ctype-uca-scanner_next.inl |
| 4.09 | 1.9% | `hp_write_key` | hp_write.c |
Hottest source lines:

| CPU s | file:line |
|---|---|
| 42.50 | `strings_def.h:209` |
| 16.29 | `ctype-uca.c:31285` |
| 7.31 | `ctype-utf8.c:3008` |
| 6.12 | `m_ctype.h:1980` |
| 4.24 | `strings_def.h:100` |
| 2.77 | `m_ctype.h:1978` |

### [10] tpcb_key — bound by front_end

*instruction-supply bound: shrink instruction footprint on the hot path - reduce inlining of cold branches, hoist cold/error paths out of line, merge duplicated call sites, avoid megamorphic dispatch*

excluded: collector 33.7s, syscall/libc 42.3s

| CPU s | % | function | source |
|---|---|---|---|
| 12.35 | 6.7% | `cmp_dtuple_rec_bytes` | page0cur.cc |
| 1.64 | 0.9% | `page_cur_dtuple_cmp<false>` | page0cur.cc |
| 1.44 | 0.8% | `alloc_root` | my_alloc.c |
| 1.24 | 0.7% | `my_betoh64` | my_byteorder.h |
| 1.07 | 0.6% | `btr_cur_t::search_leaf` | btr0cur.cc |
| 0.96 | 0.5% | `mysql_execute_command` | sql_parse.cc |
| 0.96 | 0.5% | `cmp_data` | rem0cmp.cc |
| 0.84 | 0.5% | `std::__atomic_base<unsigned int>::fetch_sub` | atomic_base.h |
Hottest source lines:

| CPU s | file:line |
|---|---|
| 6.88 | `page0cur.cc:103` |
| 2.90 | `page0cur.cc:85` |
| 1.40 | `my_byteorder.h:56` |
| 1.25 | `my_byteorder.h:92` |
| 1.24 | `srw_lock.h:332` |
| 0.93 | `atomic_base.h:641` |

### [11] tpcb_no_key — bound by front_end

*instruction-supply bound: shrink instruction footprint on the hot path - reduce inlining of cold branches, hoist cold/error paths out of line, merge duplicated call sites, avoid megamorphic dispatch*

excluded: collector 35.4s, syscall/libc 44.2s

| CPU s | % | function | source |
|---|---|---|---|
| 9.11 | 5.1% | `cmp_dtuple_rec_bytes` | page0cur.cc |
| 1.63 | 0.9% | `alloc_root` | my_alloc.c |
| 1.27 | 0.7% | `page_cur_dtuple_cmp<false>` | page0cur.cc |
| 1.10 | 0.6% | `my_betoh64` | my_byteorder.h |
| 0.95 | 0.5% | `mysql_execute_command` | sql_parse.cc |
| 0.94 | 0.5% | `btr_cur_t::search_leaf` | btr0cur.cc |
| 0.92 | 0.5% | `cmp_data` | rem0cmp.cc |
| 0.85 | 0.5% | `JOIN::prepare` | sql_select.cc |
Hottest source lines:

| CPU s | file:line |
|---|---|
| 5.13 | `page0cur.cc:103` |
| 2.67 | `page0cur.cc:85` |
| 1.24 | `smmintrin.h:843` |
| 1.11 | `my_byteorder.h:92` |
| 1.03 | `my_byteorder.h:56` |
| 0.89 | `srw_lock.h:332` |

### [5] oltp_point_select — bound by front_end

*instruction-supply bound: shrink instruction footprint on the hot path - reduce inlining of cold branches, hoist cold/error paths out of line, merge duplicated call sites, avoid megamorphic dispatch*

excluded: collector 36.4s, syscall/libc 24.8s

| CPU s | % | function | source |
|---|---|---|---|
| 6.44 | 4.5% | `cmp_dtuple_rec_bytes` | page0cur.cc |
| 1.80 | 1.3% | `alloc_root` | my_alloc.c |
| 1.56 | 1.1% | `l_find` | lf_hash.cc |
| 1.35 | 0.9% | `make_join_statistics` | sql_select.cc |
| 1.10 | 0.8% | `my_betoh64` | my_byteorder.h |
| 1.03 | 0.7% | `row_search_mvcc<InnoDBPolicy<(bool)1, (bool)1>>` | row0sel.cc |
| 0.90 | 0.6% | `JOIN::prepare` | sql_select.cc |
| 0.86 | 0.6% | `dispatch_command` | sql_parse.cc |
Hottest source lines:

| CPU s | file:line |
|---|---|
| 3.64 | `page0cur.cc:103` |
| 1.52 | `page0cur.cc:85` |
| 1.10 | `my_byteorder.h:92` |
| 0.78 | `sql_class.h:3514` |
| 0.76 | `stl_algobase.h:239` |
| 0.70 | `cancellation.c:38` |

### [1] oltp_read_only — bound by front_end

*instruction-supply bound: shrink instruction footprint on the hot path - reduce inlining of cold branches, hoist cold/error paths out of line, merge duplicated call sites, avoid megamorphic dispatch*

excluded: collector 10.7s, syscall/libc 13.2s

| CPU s | % | function | source |
|---|---|---|---|
| 6.35 | 6.0% | `MY_HASH_ADD` | strings_def.h |
| 2.82 | 2.7% | `cmp_dtuple_rec_bytes` | page0cur.cc |
| 2.59 | 2.5% | `my_uca_scanner_next_expansion_weight` | ctype-uca.c |
| 2.51 | 2.4% | `skip_trailing_space` | strings_def.h |
| 2.04 | 1.9% | `my_charlen_utf8mb4` | ctype-utf8.c |
| 1.78 | 1.7% | `my_uca_level_booster_simple_prefix_cmp` | ctype-uca.c |
| 1.69 | 1.6% | `row_search_mvcc<InnoDBPolicy<(bool)1, (bool)1>>` | row0sel.cc |
| 1.45 | 1.4% | `my_ismbchar` | m_ctype.h |
Hottest source lines:

| CPU s | file:line |
|---|---|
| 6.64 | `strings_def.h:209` |
| 2.51 | `ctype-uca.c:31285` |
| 1.49 | `ctype-utf8.c:3008` |
| 1.13 | `strings_def.h:100` |
| 1.03 | `strings_def.h:99` |
| 1.03 | `page0cur.cc:103` |

