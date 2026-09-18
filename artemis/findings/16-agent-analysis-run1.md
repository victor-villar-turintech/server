# Agent analysis of the collation hash path (Discovery run 1)

Captured verbatim from the agent's own working notes on run
`e5fc3e58-9fdb-4ac4-8768-8fa30357be0f` (baseline `b2a8c223`, 2026-09-08).
The agent did NOT have access to the `artemis/` folder for this run (the
project mirror was still at the seed commit), so this analysis was derived
independently from the task brief and the source alone. It reproduces the
profiling conclusions and adds one result not previously identified:

**Decoupling identity.** Because `A << 8` has zeros in bits 0-5,
`(A' & 63) == (A & 63) ^ (m & 63)` where `m = ((A & 63) + B) * value`. The
6-bit sub-state therefore evolves independently of the wide state, splitting
the single ~7-cycle loop-carried chain into a short 6-bit chain feeding a
3-cycle wide chain that can overlap. Pure algebra on the existing expression -
bit-exact by construction.

---

## Note on the brief
`artemis/OPTIMISATION-OBJECTIVES.md` does not exist in this tree (no `artemis/` folder at all, and a repo-wide glob for `*OPTIMISATION*` returns nothing). There are also no earlier discovery runs on this project. So the task brief itself is the working spec: primary metric = tps of workload 9 (oltp_distinct_ranges), guards = workloads 1, 5, 10; hash output must stay bit-exact; changes confined to `include/m_ctype.h`, `strings/strings_def.h`, `strings/ctype-uca.inl`, `strings/ctype-uca.c`.

## Project conventions found (authoritative)
Root has no `CLAUDE.md`/`AGENTS.md`/`.cursorrules`/`.cursor/`/`.claude/`; `.github/` has only workflows + FUNDING. The one rule file is `CODING_STANDARDS.md`:
- Primary rule: match surrounding code; reuse existing helpers rather than inventing new ones.
- `a= 1;` spacing (no space before `=`), snake_case functions/vars, `*` binds to the name.
- 80-col limit, no tabs, no trailing whitespace, `/* */` multi-line comments with 2-space body indent.
- **Avoid `long`/`ulong`** — relevant here, because the hash state fields are declared `ulong`. Any new local must preserve the exact `ulong` (platform-dependent width!) semantics of the current arithmetic, so this rule partially conflicts with bit-exactness: locals mirroring the state should be typed to match the existing `ulong` behaviour rather than "improved" to a fixed-width type, or wrap-around/shift results could change on LP64 vs LLP64.

## Baseline: what the hot path does today

**The hash primitive** (`include/m_ctype.h:551`):
```c
#define MY_HASH_ADD_MARIADB(A, B, value) \
  do { A^= (((A & 63)+B)*((value)))+ (A << 8); B+=3; } while(0)
```
Used with `A = hasher->m_nr1`, `B = hasher->m_nr2`, both `ulong` inside a union with `uint64_t m_nr`.

**The per-byte dispatcher** (`strings/strings_def.h:203-210`): `MY_HASH_ADD()` is a static inline that checks `hasher->m_hash_byte` and either calls that function pointer or applies `MY_HASH_ADD_MARIADB` on `hasher->m_nr1/m_nr2` in place, through the pointer. `MY_HASH_ADD_STR` is the same shape for a whole buffer. `MY_HASH_ADD_16` exists but is deliberately NOT usable in the UCA path.

**The hash loop** (`strings/ctype-uca.inl:648-697` `hash_sort`, and `:700-718` `hash_sort_nopad`): initialises a `my_uca_scanner` + `my_uca_scanner_param`, then loops on `scanner_next()`; for each positive weight it calls `MY_HASH_ADD(hasher, s_res >> 8)` then `MY_HASH_ADD(hasher, s_res & 0xFF)` — high byte first, low byte second. Comments at lines 681-693 state explicitly that this reversed order is a legacy mistake that must be preserved or old partitioned tables break; that matches the hard constraint. `hash_sort` additionally has a run-length dance for space weights: it counts consecutive space weights, and only if a non-space weight follows does it replay `count` copies of the (space_weight>>8, space_weight&0xFF) byte pair — trailing spaces are dropped by returning early.

**The weight feeder** (`strings/ctype-uca.c:31282-31296` and `strings/ctype-uca-scanner_next.inl`): `scanner_next` first calls `my_uca_scanner_next_expansion_weight` (a 2-instruction "is there another weight in the current expansion" check on `scanner->wbeg`), then, on the common path, either the 2-byte booster fast path (`MY_UCA_ASCII_OPTIMIZE`, enabled for utf8mb3/utf8mb4) or the single-ASCII-byte path that indexes `level->weights[0] + code * level->lengths[0]`. Note `strnxfrm_onelevel_internal` (`:769-810`) already contains a hand-written ASCII fast path that bypasses the scanner entirely — precedent in-file for adding such a fast path to `hash_sort`.

## Measurable characteristics / weaknesses

1. **Loop-carried dependency chain, ~7 cycles per byte.** Each byte serialises `and` → `add` → `imul` (3-4 cyc) → `add` → `xor`. At two bytes per weight, and one weight per character, this dominates a retiring-bound loop. Confirms the "retiring-bound, zero vector uOps" profile in the brief.
2. **A decoupling identity is available and value-preserving.** Since `A << 8` has zeros in bits 0..5, `(m + (A<<8)) mod 64 == m mod 64`, so `A' & 63 == (A & 63) ^ (m & 63)`. The 6-bit sub-state evolves *independently* of the wide state. That splits the single ~7-cycle chain into a short 6-bit chain (`add`, `imul`, `xor`) feeding a 3-cycle wide chain (`shl`, `add`, `xor`) that can overlap. This is pure algebra on the existing expression, so output stays bit-exact.
3. **Loop-invariant branch evaluated twice per weight.** `hasher->m_hash_byte` is tested on every single byte (2× per weight, 2× again for each replayed space byte), though it cannot change during a `hash_sort` call. Hoisting it once per call is free correctness-wise.
4. **State lives in memory, not registers.** Because `MY_HASH_ADD` may call a function pointer through `hasher`, the compiler must keep `m_nr1`/`m_nr2` live in memory across each byte — a load/store round trip per byte on top of the arithmetic. Hoisting the branch lets the MariaDB-algorithm loop keep `A`/`B` in locals and write back once. Aliasing caveat: `hasher` and the string could theoretically alias, so the write-back must happen at all exits (including the early `return` in the trailing-space path).
5. **`B` is a trivial induction variable** (`B += 3` per byte). It needs no per-byte store and can be folded: for a fused two-byte step the second byte's multiplier is `(a + B + 3)`, and `B` can be advanced by 6 per weight. Fully foldable at compile time.
6. **Two-byte-per-weight structure is fusable.** `s_res >> 8` and `s_res & 0xFF` are known together; a fused "add one 16-bit weight" helper removes one branch, one call-boundary and half the induction bookkeeping per weight, while keeping the high-byte-then-low-byte order that the contract requires.
7. **Redundant work in the space path.** The space run-length replay re-derives `space_weight >> 8` / `space_weight & 0xFF` inside the `do/while` on every iteration, and `my_space_weight(level)` is recomputed per call (a double indirection into `level->weights[0][0x20 * level->lengths[0]]`).
8. **Scanner call overhead per weight.** `scanner_next` is `static inline` but large (booster path + contraction handling); `hash_sort` has no ASCII fast path even though `strnxfrm` in the same file does. For ASCII inputs with no contractions and single 2-byte weights, weights could be produced with a couple of loads, feeding a tighter hash loop.

## Risk / correctness notes for later versions
- Bit-exactness is over `ulong` arithmetic: `A << 8` and the multiply wrap at 64 bits on Linux LP64 but 32 bits on Windows. Any refactor must keep the same declared type for the accumulator, or the differential gate (and Windows builds) could diverge.
- The same macro is used outside the target files (`sql/key.cc`, `storage/heap/hp_hash.c`, `storage/maria`, `storage/myisam`), so the macro's semantics must not change — only added, equivalent formulations, or internal use inside the four target files.
- `m_hash_byte == NULL` is exactly the mysql5x hasher (the only one using `m_nr1/m_nr2` and the only one where the macro path runs); base31/crc32c/xxh32/xxh3 all supply function pointers, and the `DBUG_ASSERT` guarantees `m_hash_byte`/`m_hash_str` are both-or-neither NULL. So hoisting the test once per `hash_sort` call is safe.
- The partition/hash consumers only read `m_nr1` (cast to uint32 in `finalize`), but `m_nr2` must still be left correct for callers that continue hashing more fields into the same hasher (e.g. multi-column keys in `sql/key.cc`, `Hasher::add`), so the write-back must include `B`.
