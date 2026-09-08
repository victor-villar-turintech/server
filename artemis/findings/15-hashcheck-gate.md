# Differential bit-exactness gate — built and validated, 2026-09-08

`artemis/gates/hashcheck/` — `hash_dump.c` (the corpus) and `hashcheck.sh` (the gate).
Reference fingerprint: `results/hashcheck/reference.tsv (runner host; 16 MB, not in repo)`, **70,868 vectors**,
sha256 `e199dab1…`, 7 collations.

## Why

`MY_HASH_ADD_MARIADB` output determines **partition placement for existing
tables**. Any optimisation of the collation hash path must be bit-exact, or rows
silently land in the wrong partition on an upgraded server. That is the failure
mode that would sink a PR, so it needs a gate of its own.

## What it does

Dumps `(nr1, nr2)` at the true contract boundary - `cs->coll->hash_sort()`, the
same call partitioning makes - across a deterministic corpus, then requires two
builds to produce **byte-identical** output.

Corpus: empty and all 256 single bytes; every byte paired with 'a' in both
orders; trailing/leading/interior space runs (PAD vs NOPAD divergence); multi-
byte UTF-8 including expansions (sharp-s, fi-ligature), contractions (Catalan
l-middot), combining marks, CJK and 4-byte emoji, plus all pairwise mixes;
invalid and truncated UTF-8; a length sweep 0-128; 8,000 deterministic
pseudo-random strings; and 1,000 sysbench-shaped `CHAR(120)`/`CHAR(60)`
space-padded values. Seven collations, PRNG re-seeded per collation so the
corpus is identical everywhere.

**Deliberately outside the source tree.** It is compiled against a build, never
built from one, so an optimising agent cannot modify the test that judges it.

## Validation

| Test | Expected | Result |
|---|---|---|
| Identical builds | PASS | PASS, 70,868 vectors identical |
| Gross sabotage (`A & 63` → `A & 127`) | FAIL | FAIL, 64,728 vectors differ |
| Subtle sabotage (wrong only when weight byte `== 0x80`) | FAIL | FAIL |
| Restored source | PASS | PASS, exit 0 |

Two bugs found in the gate itself while validating it:
- the initial version called a C++ convenience wrapper unavailable in C; it now
  uses the raw `coll->hash_sort` pointer, which is the truer contract anyway
- piping `diff` into `head` sent SIGPIPE and killed the script mid-verdict, so
  it exited **141 instead of 1**. For a gate the exit code *is* the contract, so
  the full diff now goes to a file and is read from there. Exit is now a
  reliable 0/1.

## An honest correction

I expected `mysql-test` to miss hash changes, and used that to justify this
gate. **It does not.** Both sabotages were caught - by
`main.partition_key_algorithm`, `main.ctype_uca_partitions` and
`innodb.partition_locking`. MariaDB's own coverage of this property is better
than I assumed, and that is worth saying plainly.

So the gate is **complementary, not a safety net for a hole that does not
exist**. Its remaining value:

1. **It cannot be gamed by the thing it judges.** `partition_key_algorithm`
   asserts against a recorded `.result` file *inside the repository*. An agent
   that changes the hash and updates that file passes mtr. This is a live
   failure mode for automated optimisation, not a hypothetical. The external
   gate has no in-tree expected output to edit.
2. **Speed** - ~1 min versus ~6 min for mtr, so it is the cheaper first
   rejection in a Discovery loop evaluating many candidates.
3. **Diagnosis** - mtr reports "test failed"; this reports exactly which inputs
   diverge, with collation, label, length and the input bytes in hex. Directly
   actionable for an agent iterating on a candidate.
4. **Breadth** - 70,868 vectors including all byte values, invalid UTF-8 and
   length boundaries, versus a fixed set of values in the mtr test. Neither
   sabotage exercised that difference, so this is a plausible rather than
   demonstrated advantage, and is stated as such.

## Use in the Discovery loop

```
build → hashcheck (~1 min) → mtr (~6 min) → A/B benchmark (~14 min)
```

Cheapest rejection first. Any hashcheck failure is a **hard reject**: no
benchmark number is computed for a candidate that changes hash output, however
fast it is.

```
artemis/gates/hashcheck/hashcheck.sh --record <BUILD>          # capture reference
artemis/gates/hashcheck/hashcheck.sh <REF_BUILD> <CAND_BUILD>  # gate; 0 pass, 1 fail
```
