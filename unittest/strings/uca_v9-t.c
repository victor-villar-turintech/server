/* Copyright (c) 2026, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

/*
  Unit test for the UCA collation hot path (strings/ctype-uca.inl, ctype-uca-scanner_next.inl, strings_def.h).

  The optimisation of these functions must not change what the server computes: the collation hash decides
  partition placement and MEMORY hash buckets, the comparison decides equality and ordering, the sort key
  decides ORDER BY. This test pins the contract down in three ways:

    1. Golden hash vectors. 686 strings from 16 families (single bytes, byte pairs, trailing-space shapes,
       multi-byte text with expansions and contractions, invalid sequences, length sweeps, random printable
       and random bytes, benchmark-shaped padded columns) across 7 collations, with (nr1, nr2) recorded by the
       UNMODIFIED code through cs->coll->hash_sort(), exactly as partitioning calls it. Any difference fails.
    2. The two-byte hash step. When MY_HASH_ADD_MARIADB_2BYTES exists it must equal two applications of
       MY_HASH_ADD_MARIADB bit for bit, for every (hi, lo) and a range of accumulator states, and along
       long chains. Skipped (not failed) on a tree without the macro, so the test also runs on the original.
    3. Equivalence properties on a generated multilingual corpus, for several UCA collations: comparison is
       reflexive and antisymmetric; byte-identical strings compare equal and clear the prefix flag; strings that
       compare equal hash equal; trailing spaces are ignored by PAD collations and not by NOPAD ones; the order
       given by the comparison agrees with the order of the sort keys; all of it including 3- and 4-byte
       characters, ignorables, combining marks and invalid bytes.
*/

#include <tap.h>
#include <my_global.h>
#include <my_sys.h>
#include <m_ctype.h>
#include <string.h>
#include "strings_def.h"       /* MY_HASH_ADD_MARIADB_2BYTES, when the tree has it */
#include "uca_v9_vectors.h"

/* ---------------------------------------------------------------------------------------------------------- */
/* Deterministic pseudo-random numbers: the corpus must be the same on every run and every build.             */
static ulonglong rng_state= 0x9E3779B97F4A7C15ULL;
static unsigned rnd(unsigned n)
{
  rng_state= rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
  return (unsigned) ((rng_state >> 33) % n);
}

static void hash_of(CHARSET_INFO *cs, const uchar *s, size_t len, ulong *nr1, ulong *nr2)
{
  my_hasher_st h= my_hasher_mysql5x();
  my_ci_hash_sort(&h, cs, s, len);
  *nr1= h.m_nr1; *nr2= h.m_nr2;
}

static int sgn(int x) { return x < 0 ? -1 : x > 0 ? 1 : 0; }

/* ---------------------------------------------------------------------------------------------------------- */
/* 1. Golden vectors                                                                                           */
static void test_golden_vectors(void)
{
  int ci, total= 0;
#if SIZEOF_LONG != 8
  skip(7, "golden vectors were recorded with a 64-bit ulong hash accumulator");
  return;
#endif
  for (ci= 0; uca_v9_vector_collations[ci]; ci++)
  {
    CHARSET_INFO *cs= get_charset_by_name(uca_v9_vector_collations[ci], MYF(0));
    size_t i, n= 0, bad= 0;
    if (!cs) { skip(1, "collation %s not available", uca_v9_vector_collations[ci]); continue; }
    for (i= 0; i < sizeof(uca_v9_vectors) / sizeof(uca_v9_vectors[0]); i++)
    {
      const uca_v9_vector *v= &uca_v9_vectors[i];
      ulong nr1, nr2;
      if (v->cs != ci) continue;
      n++;
      hash_of(cs, (const uchar *) v->bytes, v->len, &nr1, &nr2);
      if (nr1 != v->nr1 || nr2 != v->nr2)
      {
        if (bad < 3)
          diag("%s family=%s len=%u: got (%lu,%lu) expected (%lu,%lu)", cs->coll_name.str, v->family,
               (uint) v->len, nr1, nr2, v->nr1, v->nr2);
        bad++;
      }
    }
    total+= (int) n;
    ok(bad == 0, "golden hash vectors: %s, %u vectors identical to the original code", cs->coll_name.str, (uint) n);
  }
  diag("golden vectors checked: %d", total);
}

/* ---------------------------------------------------------------------------------------------------------- */
/* 2. The two-byte hash step                                                                                     */
static void test_hash_macro(void)
{
#if defined(MY_HASH_ADD_MARIADB_2BYTES) && SIZEOF_LONG == 8
  static const ulong states[]= { 1, 4, 0, 63, 64, 255, 256, 65535, 0x12345678UL, 0xFFFFFFFFUL,
                                 (ulong) 0x7FFFFFFFFFFFFFFFULL, (ulong) ~0UL, 1234567890123UL };
  size_t si, sj; unsigned hi, lo, i;
  ulong mism= 0, a6mism= 0, chains= 0;
  for (si= 0; si < sizeof(states) / sizeof(states[0]); si++)
    for (sj= 0; sj < sizeof(states) / sizeof(states[0]); sj++)
      for (hi= 0; hi < 256; hi+= 1)
        for (lo= 0; lo < 256; lo+= (hi & 7) ? 17 : 1)      /* every lo for one hi in eight, a sweep for the rest */
        {
          ulong A1= states[si], B1= states[sj], A2= A1, B2= B1, A6= A2 & 63;
          MY_HASH_ADD_MARIADB(A1, B1, (uchar) hi);
          MY_HASH_ADD_MARIADB(A1, B1, (uchar) lo);
          MY_HASH_ADD_MARIADB_2BYTES(A2, B2, A6, (uchar) hi, (uchar) lo);
          if (A1 != A2 || B1 != B2) mism++;
          if (A6 != (A2 & 63)) a6mism++;
        }
  ok(mism == 0, "two-byte hash step equals two one-byte steps over %u state pairs x byte pairs (mismatches: %lu)",
     (uint) (sizeof(states) / sizeof(states[0]) * sizeof(states) / sizeof(states[0])), mism);
  ok(a6mism == 0, "the carried six-bit sub-state stays equal to the low six bits of the accumulator (mismatches: %lu)", a6mism);
  /* long chains from random states: the two forms must track each other byte for byte */
  rng_state= 0x9E3779B97F4A7C15ULL;
  for (i= 0; i < 2000; i++)
  {
    ulong A1= ((ulong) rnd(0xFFFFFFFF) << 16) ^ rnd(0xFFFF), B1= rnd(0xFFFFFFFF), A2= A1, B2= B1, A6= A2 & 63;
    unsigned k, len= 2 * (1 + rnd(300));
    for (k= 0; k < len; k+= 2)
    {
      uchar h= (uchar) rnd(256), l= (uchar) rnd(256);
      MY_HASH_ADD_MARIADB(A1, B1, h); MY_HASH_ADD_MARIADB(A1, B1, l);
      MY_HASH_ADD_MARIADB_2BYTES(A2, B2, A6, h, l);
      if (A1 != A2 || B1 != B2 || A6 != (A2 & 63)) { chains++; break; }
    }
  }
  ok(chains == 0, "2000 random chains of up to 600 bytes: identical at every step (diverged: %lu)", chains);
#else
  skip(3, "MY_HASH_ADD_MARIADB_2BYTES not in this tree (unmodified hash step)");
#endif
}

/* ---------------------------------------------------------------------------------------------------------- */
/* 3. Equivalence properties on a generated corpus                                                              */
static const char *const tokens[]= {
  "a", "b", "z", "A", "Z", "0", "9", " ", "  ", "-", "_",
  "\xc3\xa9", "\xc3\x89", "\xc3\x84", "\xc3\xa4", "\xc3\x85", "\xc3\x9f", "ss", "\xc3\xa6", "ae",
  "e\xcc\x81", "\xcc\x81",                       /* decomposed e-acute, lone combining acute */
  "\xef\xac\x81", "fi",                          /* fi ligature and its expansion */
  "\xd0\x96", "\xd0\xb6", "\xce\xb1", "\xce\x91", "\xce\xbb",
  "\xe2\x82\xac", "\xe4\xb8\xad", "\xe6\x96\x87", "\xe6\x97\xa5", "\xed\x95\x9c",   /* euro, CJK, Hangul (3-byte) */
  "\xf0\x9f\x98\x80", "\xf0\x9d\x94\x98",                                          /* emoji, math fraktur (4-byte) */
  "\xe2\x80\x8b", "\xc2\xad",                                                       /* zero-width space, soft hyphen (ignorable) */
  "L\xc2\xb7L", "ll", "ch", "Ch",                                                   /* contraction material */
  NULL };
#define NTOK (sizeof(tokens) / sizeof(tokens[0]) - 1)

static size_t gen_string(uchar *buf, size_t cap, unsigned max_tokens, my_bool allow_invalid)
{
  size_t len= 0; unsigned n= rnd(max_tokens + 1), i;
  for (i= 0; i < n; i++)
  {
    const char *t;
    size_t tl;
    if (allow_invalid && rnd(20) == 0)
    {
      static const char *const bad[]= { "\xc3", "\xe2\x82", "\xf0\x9f\x98", "\xff", "\x80", "\xc0\x80", "\xed\xa0\x80" };
      t= bad[rnd(7)];
    }
    else
      t= tokens[rnd((unsigned) NTOK)];
    tl= strlen(t);
    if (len + tl + 1 > cap) break;
    memcpy(buf + len, t, tl); len+= tl;
  }
  return len;
}

/* A variant of s that the collation treats as equal: case change (ai_ci), canonical equivalents, expansions,
   trailing spaces (PAD). Returns the new length, or 0 if no variant applies. */
static size_t equal_variant(CHARSET_INFO *cs, const uchar *s, size_t len, uchar *out, size_t cap, my_bool pad)
{
  size_t i, o= 0;
  my_bool changed= FALSE, ci= (cs->state & MY_CS_CSSORT) ? FALSE : TRUE;   /* _ci collations fold case */
  my_bool ai= strstr(cs->coll_name.str, "_ai_") != NULL;
  for (i= 0; i < len && o + 8 < cap; )
  {
    if (ci && s[i] >= 'a' && s[i] <= 'z' && rnd(2)) { out[o++]= (uchar) (s[i] - 32); i++; changed= TRUE; continue; }
    if (ci && s[i] >= 'A' && s[i] <= 'Z' && rnd(2)) { out[o++]= (uchar) (s[i] + 32); i++; changed= TRUE; continue; }
    if (i + 1 < len && s[i] == 0xc3 && s[i+1] == 0xa9 && rnd(2))              /* e-acute -> e + combining acute */
    { memcpy(out + o, "e\xcc\x81", 3); o+= 3; i+= 2; changed= TRUE; continue; }
    if (ai && i + 1 < len && s[i] == 0xc3 && s[i+1] == 0x9f && rnd(2))        /* sharp s -> ss (primary level) */
    { out[o++]= 's'; out[o++]= 's'; i+= 2; changed= TRUE; continue; }
    if (ai && i + 2 < len && s[i] == 0xef && s[i+1] == 0xac && s[i+2] == 0x81 && rnd(2))   /* fi ligature -> fi */
    { out[o++]= 'f'; out[o++]= 'i'; i+= 3; changed= TRUE; continue; }
    out[o++]= s[i++];
  }
  if (i < len) return 0;
  if (pad && rnd(2)) { out[o++]= ' '; out[o++]= ' '; changed= TRUE; }
  return changed ? o : 0;
}

static void test_properties(const char *name)
{
  CHARSET_INFO *cs= get_charset_by_name(name, MYF(0));
  static uchar s[256], t[256], u[256], ks[8192], kt[8192];   /* static: keeps the frame small */
  size_t slen, tlen, ulen, klen;
  unsigned trial, nkeys;
  ulong refl= 0, prefix_bad= 0, antisym= 0, hash_eq_bad= 0, equal_pairs= 0, pad_bad= 0, order_bad= 0, compared= 0;
  my_bool pad;
  if (!cs) { skip(7, "collation %s not available", name); return; }
  pad= (cs->state & MY_CS_NOPAD) ? FALSE : TRUE;
  klen= cs->coll->strnxfrmlen(cs, sizeof(s));
  if (klen > sizeof(ks))
  {
    diag("%s: sort-key length %u exceeds the test buffer; the sort-key order check is skipped", name, (uint) klen);
    klen= 0;
  }
  nkeys= (unsigned) klen;
  rng_state= 0x9E3779B97F4A7C15ULL;
  for (trial= 0; trial < 6000; trial++)
  {
    my_bool is_prefix= FALSE;
    ulong h1, h2, g1, g2;
    int c_st, c_ts;
    slen= gen_string(s, sizeof(s), 24, trial % 3 == 0);
    tlen= gen_string(t, sizeof(t), 24, trial % 3 == 0);
    /* reflexive; for identical strings the prefix flag, initialised to FALSE, must stay FALSE
       (the original code either leaves it untouched or clears it; the optimised code clears it) */
    if (my_ci_strnncollsp(cs, s, slen, s, slen) != 0 ||
        my_ci_strnncollsp_nchars(cs, s, slen, s, slen, 1000, 0) != 0) refl++;
    if (my_ci_strnncoll(cs, s, slen, s, slen, &is_prefix) != 0 || is_prefix) prefix_bad++;
    /* b is a proper prefix of a: result 0 and the flag set, independent of trailing bytes */
    if (slen > 0 && slen + 2 < sizeof(u))
    {
      memcpy(u, s, slen); u[slen]= 'q'; u[slen + 1]= 'z'; ulen= slen + 2; is_prefix= FALSE;
      if (my_ci_strnncoll(cs, u, ulen, s, slen, &is_prefix) != 0 || !is_prefix) prefix_bad++;
    }
    /* antisymmetric */
    c_st= my_ci_strnncollsp(cs, s, slen, t, tlen); c_ts= my_ci_strnncollsp(cs, t, tlen, s, slen);
    if (sgn(c_st) != -sgn(c_ts)) antisym++;
    /* equal => equal hash (random pairs and constructed equal variants) */
    hash_of(cs, s, slen, &h1, &h2);
    if (c_st == 0) { equal_pairs++; hash_of(cs, t, tlen, &g1, &g2); if (h1 != g1 || h2 != g2) hash_eq_bad++; }
    if ((ulen= equal_variant(cs, s, slen, u, sizeof(u), pad)) > 0)
    {
      if (my_ci_strnncollsp(cs, s, slen, u, ulen) == 0)
      {
        equal_pairs++; hash_of(cs, u, ulen, &g1, &g2);
        if (h1 != g1 || h2 != g2) hash_eq_bad++;
      }
    }
    /* trailing spaces: ignored by PAD collations (compare and hash), significant for NOPAD */
    if (slen + 2 < sizeof(u) && slen > 0 && s[slen - 1] != ' ' && s[slen - 1] >= 'a' && s[slen - 1] <= 'z')
    {
      memcpy(u, s, slen); u[slen]= ' '; u[slen + 1]= ' '; ulen= slen + 2;
      hash_of(cs, u, ulen, &g1, &g2);
      if (pad)
      { if (my_ci_strnncollsp(cs, s, slen, u, ulen) != 0 || h1 != g1 || h2 != g2) pad_bad++; }
      else
      { if (my_ci_strnncollsp(cs, s, slen, u, ulen) == 0) pad_bad++; }
    }
    /* order by comparison == order by sort key (full-length, space-padded keys, as filesort builds them) */
    if (klen)
    {
      my_strnxfrm_ret_t r1= cs->coll->strnxfrm(cs, ks, klen, nkeys, s, slen, MY_STRXFRM_PAD_WITH_SPACE | MY_STRXFRM_PAD_TO_MAXLEN);
      my_strnxfrm_ret_t r2= cs->coll->strnxfrm(cs, kt, klen, nkeys, t, tlen, MY_STRXFRM_PAD_WITH_SPACE | MY_STRXFRM_PAD_TO_MAXLEN);
      if (r1.m_result_length == klen && r2.m_result_length == klen)
      {
        compared++;
        if (sgn(memcmp(ks, kt, klen)) != sgn(c_st)) order_bad++;
      }
    }
  }
  ok(refl == 0, "%s: comparison is reflexive on 6000 strings (failures: %lu)", name, refl);
  ok(prefix_bad == 0, "%s: identical strings compare equal without raising the prefix flag; a proper prefix raises it (failures: %lu)", name, prefix_bad);
  ok(antisym == 0, "%s: comparison is antisymmetric on 6000 pairs (failures: %lu)", name, antisym);
  ok(hash_eq_bad == 0, "%s: strings that compare equal hash equal, %lu equal pairs (failures: %lu)", name, equal_pairs, hash_eq_bad);
  ok(pad_bad == 0, "%s: trailing spaces %s (failures: %lu)", name, pad ? "ignored by compare and hash (PAD)" : "significant (NOPAD)", pad_bad);
  if (klen) ok(order_bad == 0, "%s: order by comparison agrees with order by sort key on %lu pairs (failures: %lu)", name, compared, order_bad);
  else skip(1, "%s: sort-key order check skipped (buffer)", name);
  ok(equal_pairs > 50, "%s: the corpus produced enough equal pairs to be meaningful (%lu)", name, equal_pairs);
}

int main(int argc __attribute__((unused)), char **argv)
{
  static const char *const colls[]= {
    "utf8mb4_uca1400_ai_ci", "utf8mb4_uca1400_as_cs", "utf8mb4_uca1400_nopad_ai_ci", "utf8mb4_uca1400_nopad_as_cs",
    "utf8mb3_uca1400_ai_ci", "utf8mb4_uca1400_spanish2_ai_ci", "utf8mb4_uca1400_swedish_ai_ci", "utf8mb4_uca1400_german2_as_cs",
    "utf8mb4_unicode_520_ci", NULL };
  int i;
  MY_INIT(argv[0]);
  plan(NO_PLAN);
  test_golden_vectors();
  test_hash_macro();
  for (i= 0; colls[i]; i++) test_properties(colls[i]);
  my_end(0);
  return exit_status();
}
