/*
  Differential bit-exactness harness for MariaDB collation hashing.

  WHY THIS EXISTS
  MY_HASH_ADD_MARIADB determines partition placement for existing tables. Any
  optimisation of the collation hash path must produce BIT-IDENTICAL output or
  rows silently land in the wrong partition on an upgraded server. The
  mysql-test suite checks behaviour, not exhaustive hash equality, so it can
  pass a candidate that is subtly wrong.

  This program dumps (nr1, nr2) for a large corpus at the true contract
  boundary - cs->coll->hash_sort(), which is what partitioning calls. Run it
  against two builds and diff: any difference at all is a hard reject.

  DELIBERATELY OUTSIDE THE SOURCE TREE. It is compiled against a build, never
  built from it, so an optimising agent cannot modify the test that judges it.

  Build:  see hashcheck.sh
*/
#include <my_global.h>
#include <my_sys.h>
#include <m_ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Collations to cover. The server default is first; the others exercise
   different code paths through the same hash (nopad, case-sensitive,
   legacy non-UCA, and binary as a control). */
static const char *COLLATIONS[]= {
  "utf8mb4_uca1400_ai_ci",
  "utf8mb4_uca1400_as_cs",
  "utf8mb4_uca1400_nopad_ai_ci",
  "utf8mb4_general_ci",
  "utf8mb4_bin",
  "utf8mb3_uca1400_ai_ci",
  "latin1_swedish_ci",
  NULL
};

static void emit(CHARSET_INFO *cs, const char *label,
                 const uchar *s, size_t len)
{
  my_hasher_st h= my_hasher_mysql5x();
  size_t i;
  /* raw handler pointer: this is exactly what partitioning calls */
  cs->coll->hash_sort(&h, cs, s, len);
  printf("%s\t%s\t%zu\t%lu\t%lu\t",
         cs->coll_name.str, label, len,
         (unsigned long) h.m_nr1, (unsigned long) h.m_nr2);
  for (i= 0; i < len; i++) printf("%02x", s[i]);
  printf("\n");
}

/* Deterministic PRNG: the corpus must be identical across runs and builds. */
static unsigned long rng_state= 0x9E3779B97F4A7C15UL;
static unsigned rnd(unsigned n)
{
  rng_state= rng_state * 6364136223846793005UL + 1442695040888963407UL;
  return (unsigned) ((rng_state >> 33) % n);
}

static void corpus(CHARSET_INFO *cs)
{
  uchar buf[512];
  unsigned i, j, len, trial;

  /* 1. Empty and all single bytes - covers the ASCII fast path and every
        invalid-byte branch. */
  emit(cs, "empty", (const uchar*) "", 0);
  for (i= 0; i < 256; i++)
  {
    buf[0]= (uchar) i;
    emit(cs, "byte", buf, 1);
  }

  /* 2. All 2-byte combinations of a reduced alphabet, plus every byte paired
        with 'a' - catches ordering and carry effects in the hash chain. */
  for (i= 0; i < 256; i++)
  {
    buf[0]= (uchar) i; buf[1]= 'a';
    emit(cs, "byte_a", buf, 2);
    buf[0]= 'a'; buf[1]= (uchar) i;
    emit(cs, "a_byte", buf, 2);
  }

  /* 3. Trailing-space handling: PAD vs NOPAD collations diverge here, and
        hash_sort has a dedicated space-run branch. */
  for (len= 0; len <= 8; len++)
  {
    memset(buf, ' ', sizeof(buf));
    emit(cs, "spaces", buf, len);
    buf[0]= 'x'; memset(buf + 1, ' ', len);
    emit(cs, "x_spaces", buf, len + 1);
    memset(buf, ' ', len); buf[len]= 'x';
    emit(cs, "spaces_x", buf, len + 1);
    /* interior spaces must NOT be collapsed the same way as trailing */
    buf[0]= 'x'; memset(buf + 1, ' ', len); buf[len + 1]= 'y';
    emit(cs, "x_sp_y", buf, len + 2);
  }

  /* 4. Multi-byte UTF-8 across plane boundaries, including 4-byte sequences
        (emoji) and characters with UCA expansions/contractions. */
  {
    static const char *mb[]= {
      "\xc3\xa9",                     /* e-acute, 2-byte */
      "\xc3\x84",                     /* A-umlaut */
      "\xe2\x82\xac",                 /* euro, 3-byte */
      "\xe4\xb8\xad\xe6\x96\x87",     /* CJK */
      "\xf0\x9f\x98\x80",             /* emoji, 4-byte */
      "\xc3\x9f",                     /* sharp s - UCA expansion to 'ss' */
      "ss",                           /* its expansion target */
      "\xef\xac\x81",                 /* fi ligature - expansion */
      "\xcc\x81",                     /* combining acute */
      "e\xcc\x81",                    /* e + combining acute vs precomposed */
      "L\xc2\xb7L",                   /* Catalan l-middot - contraction */
      "\xd0\x96\xd0\xb6",             /* Cyrillic */
      "\xce\xb1\xce\xb2\xce\xb3",     /* Greek */
      NULL };
    for (i= 0; mb[i]; i++)
      emit(cs, "mb", (const uchar*) mb[i], strlen(mb[i]));
    /* mixed with ASCII and with each other */
    for (i= 0; mb[i]; i++)
      for (j= 0; mb[j]; j++)
      {
        char tmp[64];
        size_t n= (size_t) snprintf(tmp, sizeof(tmp), "%sa%s", mb[i], mb[j]);
        emit(cs, "mb_mix", (const uchar*) tmp, n);
      }
  }

  /* 5. Invalid / truncated UTF-8 - the error branches must hash identically. */
  {
    static const char *bad[]= {
      "\xc3", "\xe2\x82", "\xf0\x9f\x98", "\xff", "\xfe\xff",
      "\x80", "\xc0\x80", "\xed\xa0\x80", NULL };
    for (i= 0; bad[i]; i++)
      emit(cs, "invalid", (const uchar*) bad[i], strlen(bad[i]));
  }

  /* 6. Length sweep: every length 0..128 of repeating ASCII, since the hash
        chain is length-dependent and boundaries matter. */
  for (len= 0; len <= 128; len++)
  {
    for (i= 0; i < len; i++) buf[i]= (uchar) ('a' + (i % 26));
    emit(cs, "len_sweep", buf, len);
  }

  /* 7. Deterministic pseudo-random strings, printable and full-byte, at a
        range of lengths. Bulk coverage of the chain. */
  for (trial= 0; trial < 4000; trial++)
  {
    len= 1 + rnd(200);
    for (i= 0; i < len; i++) buf[i]= (uchar) (32 + rnd(95));
    emit(cs, "rand_ascii", buf, len);
  }
  for (trial= 0; trial < 4000; trial++)
  {
    len= 1 + rnd(200);
    for (i= 0; i < len; i++) buf[i]= (uchar) rnd(256);
    emit(cs, "rand_bytes", buf, len);
  }

  /* 8. sysbench-shaped: CHAR(120) and CHAR(60) space-padded, the exact shape
        the benchmark hashes. */
  for (trial= 0; trial < 500; trial++)
  {
    unsigned content= 1 + rnd(60);
    memset(buf, ' ', 120);
    for (i= 0; i < content; i++) buf[i]= (uchar) ('0' + rnd(10));
    emit(cs, "sysbench_c120", buf, 120);
    memset(buf, ' ', 60);
    for (i= 0; i < content && i < 60; i++) buf[i]= (uchar) ('a' + rnd(26));
    emit(cs, "sysbench_pad60", buf, 60);
  }
}

int main(void)
{
  int i, found= 0;
  MY_INIT("hash_dump");
  for (i= 0; COLLATIONS[i]; i++)
  {
    CHARSET_INFO *cs= get_charset_by_name(COLLATIONS[i], MYF(0));
    if (!cs)
    {
      fprintf(stderr, "hash_dump: collation not available: %s\n", COLLATIONS[i]);
      continue;
    }
    found++;
    rng_state= 0x9E3779B97F4A7C15UL;   /* identical corpus per collation */
    corpus(cs);
  }
  if (!found) { fprintf(stderr, "hash_dump: no collations resolved\n"); return 2; }
  fprintf(stderr, "hash_dump: %d collations covered\n", found);
  my_end(0);
  return 0;
}
