#!/usr/bin/env bash
# run_experiments.sh - Full pipeline runner for MPI distributed search.
# - Ensures 10 GB corpus exists (auto-generates if missing)
# - Runs experiments while printing line/col matches on every run
# - Writes static metrics + system info files under results/
# - Auto-generates static performance plot at end

set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────────
NP_LIST="2 4 8"          # Space-separated MPI process counts to test
REPEATS=5
TOKEN="ManchesterUnited"
CORPUS="corpus_10gb.txt"
CHUNK_MB=64
BINARY="./dist_search"
RESULTS_DIR="results"
VENV_PY=".venv/bin/python"
GEN_BIN="./gen_text"
EXPECTED_COUNT=10000000

# ── Parse args ───────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case $1 in
        --np)        NP_LIST="$2";   shift 2 ;;
        --repeats)   REPEATS="$2";   shift 2 ;;
        --token)     TOKEN="$2";     shift 2 ;;
        --file)      CORPUS="$2";    shift 2 ;;
        --chunk-mb)  CHUNK_MB="$2";  shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

mkdir -p "$RESULTS_DIR"
CSV_FILE="${RESULTS_DIR}/metrics.csv"
LOG_FILE="${RESULTS_DIR}/sysinfo.txt"
PLOT_PNG="${RESULTS_DIR}/performance_plot.png"
PLOT_PDF="${RESULTS_DIR}/performance_plot.pdf"
TIMING_TMP="${RESULTS_DIR}/.timing_tmp.txt"

# ── Sanity checks ─────────────────────────────────────────────────────────────
if [[ ! -f "$BINARY" ]]; then
    echo "ERROR: $BINARY not found. Run 'make all' first."
    exit 1
fi
# Ensure generator exists for auto corpus creation
if [[ ! -f "$CORPUS" ]]; then
    echo "Corpus '$CORPUS' not found. Preparing 10 GB corpus..."
    if [[ ! -x "$GEN_BIN" ]]; then
        echo "Generator binary missing, building it..."
        make gen
    fi
    "$GEN_BIN"
fi

# ── System info ───────────────────────────────────────────────────────────────
{
    echo "=== System Info ==="
    echo "--- Hostname ---"
    hostname
    echo "--- CPU ---"
    lscpu 2>/dev/null || echo "(lscpu unavailable)"
    echo "--- Memory ---"
    free -h 2>/dev/null || echo "(free unavailable)"
    echo "--- MPI ---"
    mpirun --version 2>&1 | head -5 || true
    echo "--- Disk (corpus) ---"
    ls -lh "$CORPUS"
} > "$LOG_FILE"
echo "System info saved to $LOG_FILE"

# ── CSV header (force overwrite) ──────────────────────────────────────────────
echo "impl,np,run,elapsed_s" > "$CSV_FILE"

# ── Serial baseline first ─────────────────────────────────────────────────────
echo ""
echo "=========================================="
echo " SERIAL BASELINE | Repeats=$REPEATS | Token=$TOKEN"
echo "=========================================="

for ((RUN=1; RUN<=REPEATS; RUN++)); do
    echo -n "  Serial run $RUN/$REPEATS ... "

    "$BINARY" \
        --impl serial \
        --repeats 1 \
        --token "$TOKEN" \
        --file  "$CORPUS" \
        --chunk-mb "$CHUNK_MB" \
        --expected-count "$EXPECTED_COUNT" \
        --verbose \
        2> "$TIMING_TMP"

    ELAPSED=$(awk '
        /TIMING impl=serial run=/ {
            if (match($0, /elapsed=[0-9.]+/)) {
                v = substr($0, RSTART + 8, RLENGTH - 8);
                print v;
            }
        }' "$TIMING_TMP")
    if [[ -z "$ELAPSED" ]]; then
        ELAPSED=$(awk '
            /elapsed=[0-9.]+/ {
                if (match($0, /elapsed=[0-9.]+/)) {
                    v = substr($0, RSTART + 8, RLENGTH - 8);
                    print v;
                }
            }' "$TIMING_TMP" | head -n 1)
    fi
    if [[ -z "$ELAPSED" ]]; then ELAPSED="ERROR"; fi

    awk '/^TIMING/ { print }' "$TIMING_TMP"
    rm -f "$TIMING_TMP"

    echo "elapsed=${ELAPSED}s"
    printf "serial,1,%s,%s\n" "$RUN" "$ELAPSED" >> "$CSV_FILE"
done

# ── Experiment loop ───────────────────────────────────────────────────────────
for NP in $NP_LIST; do
    echo ""
    echo "=========================================="
    echo " NP=$NP | Repeats=$REPEATS | Token=$TOKEN"
    echo "=========================================="

    for ((RUN=1; RUN<=REPEATS; RUN++)); do
        echo -n "  Run $RUN/$REPEATS ... "

        # Print all line/col matches to terminal for every run (stdout).
        # Capture only timing lines from stderr for metrics parsing.
        mpirun -np "$NP" "$BINARY" \
            --impl mpi \
            --repeats 1 \
            --token "$TOKEN" \
            --file  "$CORPUS" \
            --chunk-mb "$CHUNK_MB" \
            --expected-count "$EXPECTED_COUNT" \
            --verbose \
            2> "$TIMING_TMP"

        ELAPSED=$(awk '
            /^TIMING run=/ {
                if (match($0, /elapsed=[0-9.]+/)) {
                    v = substr($0, RSTART + 8, RLENGTH - 8);
                    print v;
                }
            }' "$TIMING_TMP")
        if [[ -z "$ELAPSED" ]]; then
            # Fallback parser: accept any TIMING line that has elapsed=
            ELAPSED=$(awk '
                /elapsed=[0-9.]+/ {
                    if (match($0, /elapsed=[0-9.]+/)) {
                        v = substr($0, RSTART + 8, RLENGTH - 8);
                        print v;
                    }
                }' "$TIMING_TMP" | head -n 1)
        fi
        if [[ -z "$ELAPSED" ]]; then ELAPSED="ERROR"; fi

        # Reprint timing diagnostics after run for visibility
        awk '
            /^TIMING/ { print }
        ' "$TIMING_TMP"

        rm -f "$TIMING_TMP"

        echo "elapsed=${ELAPSED}s"
        printf "mpi,%s,%s,%s\n" "$NP" "$RUN" "$ELAPSED" >> "$CSV_FILE"
    done
done

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "Results written to: $CSV_FILE"
echo ""
echo "=== Averages per NP ==="
# Use awk to compute averages
awk -F',' '
NR>1 && $4 != "ERROR" {
    key = $1 ":" $2;
    sum[key] += $4; count[key]++
}
END {
    for (k in sum) {
        split(k, parts, ":");
        printf "  impl=%-6s np=%-4s avg_elapsed=%.4f s  (over %d runs)\n",
               parts[1], parts[2], sum[k]/count[k], count[k];
    }
}' "$CSV_FILE" | sort

# ── Plot generation ──────────────────────────────────────────────────────────
if [[ -x "$VENV_PY" ]]; then
    PLOT_PY="$VENV_PY"
elif command -v python3 &>/dev/null; then
    PLOT_PY="python3"
else
    PLOT_PY=""
fi

if [[ -n "$PLOT_PY" ]]; then
    echo ""
    echo "Generating plots into ${RESULTS_DIR}/ ..."
    "$PLOT_PY" plot_results.py \
        --csv "$CSV_FILE" \
        --png "$PLOT_PNG" \
        --pdf "$PLOT_PDF"
else
    echo "(python3 not available — skipping plot generation)"
fi
