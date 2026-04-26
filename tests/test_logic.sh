#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

run_and_assert_count_one() {
  local test_name="$1"
  local mode="$2"
  local token="$3"
  local whole_word="$4"
  local file_path="$5"
  local out_file="$TMP_DIR/${test_name}_${mode}.out"
  local err_file="$TMP_DIR/${test_name}_${mode}.err"

  if [[ "$mode" == "serial" ]]; then
    ./dist_search \
      --impl serial \
      --token "$token" \
      --file "$file_path" \
      --whole-word "$whole_word" \
      --chunk-mb 1 \
      --repeats 1 \
      >"$out_file" 2>"$err_file"
  else
    mpirun -np 2 ./dist_search \
      --impl mpi \
      --token "$token" \
      --file "$file_path" \
      --whole-word "$whole_word" \
      --chunk-mb 1 \
      --repeats 1 \
      >"$out_file" 2>"$err_file"
  fi

  local count
  count="$(awk '/^# Found / {print $3; exit}' "$out_file")"
  if [[ "$count" != "1" ]]; then
    echo "${test_name} ${mode} failed: expected count=1 got ${count:-<none>}"
    exit 1
  fi
}

make dist_search >/dev/null

# Case 1: Whole-word normal, token fully inside first chunk.
FILE1="$TMP_DIR/case1_whole_word_normal.txt"
python3 - <<'PY' "$FILE1"
from pathlib import Path
import sys

out = Path(sys.argv[1])
token = b" ManchesterUnited "
data = bytearray(b" " * 100)
start = 10
data[start:start + len(token)] = token
data[0:8] = b"wordA x "
data[30:38] = b" wordB y"
out.write_bytes(data)
PY
run_and_assert_count_one "case1_whole_word_normal" "serial" " ManchesterUnited " "1" "$FILE1"
run_and_assert_count_one "case1_whole_word_normal" "mpi"    " ManchesterUnited " "1" "$FILE1"

# Case 2: Whole-word boundary overlap, token straddles byte 50.
FILE2="$TMP_DIR/case2_whole_word_boundary.txt"
python3 - <<'PY' "$FILE2"
from pathlib import Path
import sys

out = Path(sys.argv[1])
token = b" ManchesterUnited "
data = bytearray(b" " * 100)
start = 50 - (len(token) // 2)
data[start:start + len(token)] = token
data[5:14] = b"alpha bet"
data[80:89] = b"gamma de "
out.write_bytes(data)
PY
run_and_assert_count_one "case2_whole_word_boundary" "serial" " ManchesterUnited " "1" "$FILE2"
run_and_assert_count_one "case2_whole_word_boundary" "mpi"    " ManchesterUnited " "1" "$FILE2"

# Case 3: Substring normal, token fully inside second chunk, random letters only.
FILE3="$TMP_DIR/case3_substring_normal.txt"
python3 - <<'PY' "$FILE3"
from pathlib import Path
import random
import string
import sys

out = Path(sys.argv[1])
rng = random.Random(3001)
alphabet = string.ascii_letters.encode()
data = bytearray(rng.choice(alphabet) for _ in range(100))
token = b"ManchesterUnited"
start = 70
data[start:start + len(token)] = token
out.write_bytes(data)
PY
run_and_assert_count_one "case3_substring_normal" "serial" "ManchesterUnited" "0" "$FILE3"
run_and_assert_count_one "case3_substring_normal" "mpi"    "ManchesterUnited" "0" "$FILE3"

# Case 4: Substring boundary overlap, random letters, token straddles byte 50.
FILE4="$TMP_DIR/case4_substring_boundary.txt"
python3 - <<'PY' "$FILE4"
from pathlib import Path
import random
import string
import sys

out = Path(sys.argv[1])
rng = random.Random(4001)
alphabet = string.ascii_letters.encode()
data = bytearray(rng.choice(alphabet) for _ in range(100))
token = b"ManchesterUnited"
start = 50 - (len(token) // 2)
data[start:start + len(token)] = token
out.write_bytes(data)
PY
run_and_assert_count_one "case4_substring_boundary" "serial" "ManchesterUnited" "0" "$FILE4"
run_and_assert_count_one "case4_substring_boundary" "mpi"    "ManchesterUnited" "0" "$FILE4"

echo "[ALL 4 SEARCH TESTS PASSED]"
