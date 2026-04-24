# MPI Distributed Token Search (HPC Project)

This project implements a distributed text search in C using MPI (SPMD model) for very large files (target: 10 GB).

## Build

```bash
make all
```

This builds:
- `dist_search` (MPI distributed search)
- `gen_text` (large corpus generator)

## Generate a large corpus

```bash
./gen_text --size-gb 10 --token FINDME --seed-count 1000 --out corpus.txt
```

This also creates `corpus.txt.expected` with ground-truth `line,col` entries for seeded token positions.

## Run search

```bash
mpirun -np 4 ./dist_search --token FINDME --file corpus.txt --chunk-mb 64
```

### Key CLI options

- `--token TOKEN` (required)
- `--file PATH`
- `--repeats N`
- `--chunk-mb N`
- `--whole-word 0|1` (`1` default; set `0` for substring mode on no-space inputs)
- `--output FILE` (write occurrences to file)
- `--verbose`

## Reproducible experiments

Run:

```bash
bash run_experiments.sh --np "1 2 4" --repeats 5 --token FINDME --file corpus.txt --chunk-mb 64
```

Outputs:
- `results_<timestamp>.csv` : per-run elapsed time
- `summary_<timestamp>.csv` : per-`np` average and sample standard deviation
- `sysinfo_<timestamp>.txt` : system/toolchain information

## Notes on algorithm and correctness

- File ownership is split by byte ranges across ranks.
- Each rank streams through its full owned range in `chunk_mb` windows (memory-bounded).
- For each window, one-byte left context and `(token_len-1)` right lookahead are read to preserve boundary correctness:
  - whole-word boundary check at window starts
  - matches spanning chunk/rank boundaries
- Only matches whose **start offset** lies inside the current owned window are accepted (no duplicates).
- Rank 0 gathers and sorts global byte offsets, then computes final line/column positions in one sequential pass over the file.

## Deliverables mapping

- **Code quality:** modular files under `src/`, MPI entry in `main.c`
- **CLI controls:** chunk size, repeats, boundary mode, output path
- **Performance:** machine-parseable timing and experiment CSVs
- **Reproducibility:** one-click `run_experiments.sh` + system logs
