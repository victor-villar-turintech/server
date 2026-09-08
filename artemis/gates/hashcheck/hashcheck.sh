#!/usr/bin/env bash
# DIFFERENTIAL BIT-EXACTNESS GATE for collation-hash changes.
#
#   hashcheck.sh <REFERENCE_BUILD> <CANDIDATE_BUILD>
#   hashcheck.sh --record <BUILD>          capture a reference fingerprint
#
# Compiles hash_dump.c against each build, runs it, and requires the outputs to
# be BYTE-IDENTICAL. Any difference is a hard reject: these hashes determine
# partition placement for existing tables, so a candidate that changes even one
# value would silently misplace rows on an upgraded server.
#
# This gate is stricter than mysql-test and answers a different question. mtr
# asks "does the server still behave correctly"; this asks "does it produce the
# same bits". A candidate must pass BOTH.
set -uo pipefail
export MDB_ROOT=/home/artemis-ai/mariadb
source "$MDB_ROOT/harness/00-env.sh"
SELF=$(cd "$(dirname "$0")" && pwd)
WORK=${MDB_HASHCHECK_WORK:-$MDB_RESULTS/hashcheck}
mkdir -p "$WORK"

build_dump() {           # $1 = build dir, $2 = output binary
  local B=$1 OUT=$2
  [[ -x "$B/sql/mariadbd" ]] || { echo "hashcheck: not a build: $B" >&2; return 2; }
  gcc -O2 -o "$OUT" "$SELF/hash_dump.c" \
      -I"$B/include" -I"$MDB_SRC/include" -I"$MDB_SRC" \
      "$B/strings/libstrings.a" "$B/mysys/libmysys.a" "$B/strings/libstrings.a" \
      "$B/dbug/libdbug.a" "$B/mysys_ssl/libmysys_ssl.a" \
      -lpthread -lm -ldl -lz -lcrypto -lssl 2> "$OUT.build.log"
}

run_dump() {             # $1 = binary, $2 = output file
  "$1" > "$2" 2> "$2.stderr"
}

if [[ "${1:-}" == "--record" ]]; then
  B=${2:?usage: hashcheck.sh --record <BUILD>}
  build_dump "$B" "$WORK/dump-ref" || { cat "$WORK/dump-ref.build.log"; exit 2; }
  run_dump "$WORK/dump-ref" "$WORK/reference.tsv" || exit 2
  echo "hashcheck: reference recorded ($(wc -l < "$WORK/reference.tsv") vectors)"
  sha256sum "$WORK/reference.tsv"
  exit 0
fi

REF=${1:?usage: hashcheck.sh <REFERENCE_BUILD> <CANDIDATE_BUILD>}
CAND=${2:?usage: hashcheck.sh <REFERENCE_BUILD> <CANDIDATE_BUILD>}

for pair in "ref:$REF" "cand:$CAND"; do
  tag=${pair%%:*}; dir=${pair#*:}
  build_dump "$dir" "$WORK/dump-$tag" || {
    echo "hashcheck: FAILED to compile against $dir" >&2
    tail -20 "$WORK/dump-$tag.build.log" >&2; exit 2; }
  run_dump "$WORK/dump-$tag" "$WORK/$tag.tsv" || {
    echo "hashcheck: dump failed for $dir" >&2
    tail -5 "$WORK/$tag.tsv.stderr" >&2; exit 2; }
done

N=$(wc -l < "$WORK/ref.tsv")
(( N > 0 )) || { echo "hashcheck: reference dump is empty" >&2; exit 2; }

if cmp -s "$WORK/ref.tsv" "$WORK/cand.tsv"; then
  echo "HASH-EXACT: PASS  ($N vectors identical, $(grep -c . <(cut -f1 "$WORK/ref.tsv" | sort -u)) collations)"
  exit 0
fi

# Write the FULL diff to a file first. Piping diff into head sends SIGPIPE to
# diff and killed this script before it could report its verdict - the gate
# must return a reliable exit code, since that is its entire contract.
diff "$WORK/ref.tsv" "$WORK/cand.tsv" > "$WORK/diff.txt" 2>/dev/null
NDIFF=$(grep -c '^<' "$WORK/diff.txt" || true)
echo "HASH-EXACT: FAIL  candidate changes collation hash output" >&2
echo "  differing vectors: $NDIFF of $N" >&2
echo "  affected collations: $(grep '^<' "$WORK/diff.txt" | cut -f1 | sed 's/^< //' | sort -u | tr '\n' ' ')" >&2
echo "  full diff: $WORK/diff.txt" >&2
echo "  first differences (collation / label / len / nr1 / nr2 / hex):" >&2
sed -n '1,12p' "$WORK/diff.txt" >&2
echo >&2
echo "  This is a HARD REJECT. These hashes determine partition placement;" >&2
echo "  changing them would misplace rows in existing partitioned tables." >&2
exit 1
