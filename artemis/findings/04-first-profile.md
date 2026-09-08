# First profile — evidence for the target shortlist

**Status: EVIDENCE, NOT A SHORTLIST.** The brief requires the 3-5 target
shortlist to reflect the client's steer (still pending) and to be reviewed
internally before Discovery runs. Nothing here is committed to.

Source: `results/profile-baseline-20260903T102652Z`
`perf record -F 999 --call-graph dwarf` against the exact benchmarked binary,
30s under sysbench `oltp_read_only`, 115K samples.

## What the workload actually exercises

Server default collation is **`utf8mb4_uca1400_ai_ci`** (verified, not assumed),
and sysbench's `c char(120)` / `pad char(60)` columns inherit it.

Confirmed call path into the top hotspot:

```
evaluate_join_record -> end_write -> write_row -> heap_write
  -> hp_write_key -> hp_rec_hashnr -> my_ci_hash_sort
    -> my_uca_hash_sort_utf8mb4 -> MY_HASH_ADD
```

That is `oltp_read_only`'s `SELECT DISTINCT c ... ORDER BY c` building a MEMORY
temporary table with a hash index over a UCA-collated CHAR column. Every row
inserted computes a full Unicode Collation Algorithm hash.

## Top self-time symbols

| Self % | Symbol | Area |
|---|---|---|
| **7.80** | `my_uca_hash_sort_utf8mb4` | collation |
| 3.28 | `__memmove_avx_unaligned_erms` (libc) | - |
| 2.67 | `my_lengthsp_8bit` | collation (trailing-space length) |
| 2.62 | `my_uca_level_booster_simple_prefix_cmp` | collation |
| 2.46 | `row_search_mvcc<>` | InnoDB read path |
| 2.10 | `buf_page_get_gen` | InnoDB buffer pool |
| 1.81 | `cmp_dtuple_rec_bytes` | InnoDB compare |
| 1.77 | `my_charpos_mb` | charset |
| 1.47 | `my_charlen_utf8mb4` | charset |
| 1.29 | `my_uca_scanner_next_utf8mb4` | collation |

**Charset/collation work totals ~17.6% of self time** and is the dominant theme
by a wide margin. The top symbol alone is 2.4x the next MariaDB symbol.

## Provisional hypotheses (NOT a committed shortlist)

Recorded so the shortlist can be assembled quickly once the steer arrives.

**H1 — `MY_HASH_ADD` is byte-at-a-time in the UCA hash loop.** 1.72 of the
1.96% with resolved stacks sits inside `MY_HASH_ADD`. Each 16-bit weight costs
two separate hash steps. Metric: tps on `distinct_ranges`.
*Hard constraint:* the source comment states the two bytes are deliberately
added in the "wrong" order to preserve compatibility with existing partitioned
tables. **Any change must be bit-exact in output**, so this is an optimisation
of how the same value is computed, not what it computes. Correctness gate must
include partitioning tests specifically.

**H2 — trailing-space scanning on CHAR columns.** `c` is `char(120)` and
space-padded. `hash_sort` walks trailing spaces one `scanner_next` call at a
time before discarding them. `my_lengthsp_8bit` (2.67%) is separately computing
trailing-space-stripped lengths. Hypothesis: the padding is being walked more
than once per row.

**H3 — UCA scanner per-character cost.**
`my_uca_level_booster_simple_prefix_cmp` (2.62%) plus
`my_uca_scanner_next_utf8mb4` (1.29%). `MY_UCA_ASCII_OPTIMIZE` is already
enabled, so the question is whether the ASCII fast path is actually being taken
for this data.

**H4 — `my_charpos_mb` / `my_charlen_utf8mb4` (3.24% combined).** Per-character
position scanning, O(n) per call. Worth checking for repeated scans of the same
string within one row.

**H5 — InnoDB read path.** `row_search_mvcc` + `buf_page_get_gen` +
`cmp_dtuple_rec_bytes` = 6.37%. Larger, more central, harder for a reviewer to
accept quickly. Listed for completeness; a poor fit for "minimum expert review
time" unless the client's steer points here.

## Two caveats that must travel with this profile

**1. The ranking may not transfer to MariaDB's hardware.** This profile was
taken with the CPU thermally pinned at 98-100 C and clocked around 1.0-2.2 GHz
against 4.0 GHz nominal (see artemis/findings/03-noise-floor.md). A low clock relative to
memory speed makes compute-bound code look cheaper and memory-bound code look
more expensive than it would on a server holding 3+ GHz. **The hotspot ordering
should be re-derived on unthrottled hardware before the shortlist is frozen.**

**2. It is workload-specific by construction.** This hotspot comes from
`oltp_read_only`'s DISTINCT query combined with the default UCA collation.
Whether MariaDB considers that representative is exactly open question #1 for MariaDB engineering. The mitigating argument is that `utf8mb4_uca1400_ai_ci` is the server
default in this build, so the cost is paid by default installs rather than
being a benchmark artifact -- but **confirm what the default is on whichever
branch we end up targeting**, since that is what makes the result interesting
to them.
