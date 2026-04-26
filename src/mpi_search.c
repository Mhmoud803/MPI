/*
 * mpi_search.c — Single-pass distributed search with deferred write phase.
 *
 * Single heavy pass per rank:
 *   - read owned bytes once,
 *   - count newlines,
 *   - collect match metadata in dynamic memory.
 * Then:
 *   - MPI_Exscan computes global line offsets,
 *   - each rank writes its own output file.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <mpi.h>

#include "mpi_search.h"
#include "cli.h"
#include "file_io.h"
#include "search_utils.h"

typedef struct {
    unsigned long long local_row;
    unsigned long long col;
    long long byte_offset;
} LocalMatch;

static void print_verification_status(FILE *out, long long observed, long long expected) {
    if (expected < 0) {
        fprintf(out, "[VERIFICATION SKIPPED] pass --expected-count N to enable strict assertion\n");
        return;
    }
    if (observed == expected) {
        fprintf(out, "[VERIFICATION PASSED] total=%lld expected=%lld\n", observed, expected);
    } else {
        fprintf(out, "[VERIFICATION FAILED] total=%lld expected=%lld\n", observed, expected);
    }
}

static void ensure_results_dir(void) {
    if (mkdir("results", 0777) != 0 && errno != EEXIST) {
        perror("mkdir results");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
}

static void collect_matches_single_pass(const Config *cfg,
                                        int rank,
                                        long long file_size,
                                        long long my_start,
                                        long long my_end,
                                        LocalMatch **matches_out,
                                        size_t *match_count_out,
                                        unsigned long long *newline_count_out) {
    int tlen = (int)strlen(cfg->token);
    int skip[BMH_ALPHA];
    long long chunk_bytes = (long long)cfg->chunk_mb * 1024LL * 1024LL;
    long long remaining = my_end - my_start;
    unsigned long long local_row = 0; /* starts at row 0 in this rank chunk */
    unsigned long long local_col = 0; /* starts at col 0 in current row */
    size_t array_capacity = 1000;
    size_t local_match_count = 0;
    LocalMatch *matches = NULL;

    if (chunk_bytes < 1) chunk_bytes = 1;
    bmh_preprocess(cfg->token, tlen, skip);
    matches = (LocalMatch *)malloc(array_capacity * sizeof(LocalMatch));
    if (!matches) {
        fprintf(stderr, "Rank %d: initial malloc failed for matches\n", rank);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    for (long long cur = my_start; remaining > 0; ) {
        long long own_len = remaining;
        int overlap = (tlen > 1) ? (tlen - 1) : 0;
        long long left_extra = (cur > 0) ? 1 : 0;
        long long right_extra = overlap;
        long long read_start, read_len, got;
        char *buf;
        long long owned_start_idx;
        long long owned_end_idx;
        long long tracker;
        long long pos = 0;

        if (own_len > chunk_bytes) own_len = chunk_bytes;
        if (cur + own_len + right_extra > file_size)
            right_extra = file_size - (cur + own_len);
        if (right_extra < 0) right_extra = 0;

        read_start = cur - left_extra;
        read_len = left_extra + own_len + right_extra;
        buf = (char *)malloc((size_t)read_len + 1);
        if (!buf) {
            fprintf(stderr, "Rank %d: malloc failed in pass2\n", rank);
            free(matches);
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }

        got = read_range(cfg->file, read_start, read_len, buf);
        if (got < 0) {
            fprintf(stderr, "Rank %d: read_range failed in pass2\n", rank);
            free(buf);
            free(matches);
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
        buf[got] = '\0';

        owned_start_idx = left_extra;
        owned_end_idx = left_extra + own_len;
        tracker = owned_start_idx;

        while (pos <= got - tlen) {
            int j = tlen - 1;
            while (j >= 0 && buf[pos + j] == cfg->token[j]) j--;

            if (j < 0) {
                long long off = read_start + pos;
                if ((!cfg->whole_word || is_word_boundary(buf, pos, got, tlen)) &&
                    off >= cur && off < cur + own_len &&
                    off >= my_start && off < my_end) {
                    while (tracker < pos && tracker < owned_end_idx) {
                        if (buf[tracker] == '\n') {
                            local_row++;
                            local_col = 0;
                        } else {
                            local_col++;
                        }
                        tracker++;
                    }

                    if (local_match_count == array_capacity) {
                        size_t new_capacity = array_capacity * 2;
                            LocalMatch *next = (LocalMatch *)realloc(matches, new_capacity * sizeof(LocalMatch));
                        if (!next) {
                            fprintf(stderr, "Rank %d: realloc failed for matches\n", rank);
                            free(buf);
                            free(matches);
                            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
                        }
                        matches = next;
                        array_capacity = new_capacity;
                    }

                    matches[local_match_count].local_row = local_row;
                    matches[local_match_count].col = local_col;
                    matches[local_match_count].byte_offset = off;
                    local_match_count++;
                }
                pos++;
            } else {
                pos += skip[(unsigned char)buf[pos + tlen - 1]];
            }
        }

        while (tracker < owned_end_idx) {
            if (buf[tracker] == '\n') {
                local_row++;
                local_col = 0;
            } else {
                local_col++;
            }
            tracker++;
        }

        free(buf);
        cur += own_len;
        remaining -= own_len;
    }

    *matches_out = matches;
    *match_count_out = local_match_count;
    *newline_count_out = local_row;
}

static void write_matches_phase(int rank,
                                const char *out_path,
                                const LocalMatch *matches,
                                size_t match_count,
                                unsigned long long global_line_offset) {
    FILE *out = fopen(out_path, "w");
    if (!out) {
        perror("fopen rank output");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    for (size_t i = 0; i < match_count; i++) {
        unsigned long long global_row = global_line_offset + matches[i].local_row;
        fprintf(out, "row=%llu col=%llu offset=%lld\n",
                global_row, matches[i].col, matches[i].byte_offset);

        if (i < 10) {
            printf("[Rank %d] row=%llu col=%llu offset=%lld\n",
                   rank, global_row, matches[i].col, matches[i].byte_offset);
        }
    }

    fclose(out);
}

/* -----------------------------------------------------------------------
 * Single search run — called cfg->repeats times.
 * ----------------------------------------------------------------------- */
static double search_once(const Config *cfg, int rank, int nranks) {
    long long file_size = 0;
    long long my_start, my_length, my_end;
    LocalMatch *matches = NULL;
    size_t local_match_count = 0;
    unsigned long long chunk_newline_count = 0;
    unsigned long long global_line_offset = 0;
    long long reduced_total = 0;
    char out_path[256];
    double t_start, t_end;

    if (rank == 0) {
        file_size = file_size_bytes(cfg->file);
        if (file_size < 0) {
            fprintf(stderr, "Error: cannot stat file '%s'\n", cfg->file);
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
    }
    MPI_Bcast(&file_size, 1, MPI_LONG_LONG, 0, MPI_COMM_WORLD);

    if (file_size == 0) {
        if (rank == 0) fprintf(stderr, "Warning: file is empty.\n");
        return 0.0;
    }

    compute_range(rank, nranks, file_size, &my_start, &my_length);
    my_end = my_start + my_length;
    ensure_results_dir();

    MPI_Barrier(MPI_COMM_WORLD);
    t_start = MPI_Wtime();

    /* Single heavy pass: count newlines + collect matches in dynamic memory. */
    collect_matches_single_pass(cfg, rank, file_size, my_start, my_end,
                                &matches, &local_match_count, &chunk_newline_count);

    /* Prefix sum to convert local rows to global rows. */
    MPI_Exscan(&chunk_newline_count, &global_line_offset, 1,
               MPI_UNSIGNED_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
    if (rank == 0) global_line_offset = 0;

    /* Deferred write phase: emit results from local dynamic array. */
    snprintf(out_path, sizeof(out_path), "results/matches_rank_%d.txt", rank);
    write_matches_phase(rank, out_path, matches, local_match_count, global_line_offset);

    {
        long long local_match_count_ll = (long long)local_match_count;
        MPI_Reduce(&local_match_count_ll, &reduced_total, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    }
    t_end = MPI_Wtime();

    free(matches);

    printf("Rank %d finished: found %zu matches. Results saved to %s.\n",
           rank, local_match_count, out_path);

    if (rank == 0) {
        print_verification_status(stdout, reduced_total, cfg->expected_count);
        fflush(stdout);
    }

    return t_end - t_start;
}

void run_distributed_search(const Config *cfg) {
    int rank, nranks;
    double total_time = 0.0;

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    if (cfg->verbose) {
        char node_name[MPI_MAX_PROCESSOR_NAME];
        int node_name_len = 0;
        MPI_Get_processor_name(node_name, &node_name_len);
        node_name[node_name_len] = '\0';
        fprintf(stderr, "[DEBUG] Process %d/%d on %s\n", rank, nranks, node_name);
        fflush(stderr);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0 && cfg->verbose) {
        fprintf(stderr,
                "[Config] file=%s token='%s' ranks=%d repeats=%d chunk_mb=%d whole_word=%d expected_count=%lld\n",
                cfg->file, cfg->token, nranks, cfg->repeats, cfg->chunk_mb,
                cfg->whole_word, cfg->expected_count);
    }

    for (int r = 0; r < cfg->repeats; r++) {
        double elapsed = search_once(cfg, rank, nranks);
        total_time += elapsed;
        if (rank == 0) {
            fprintf(stderr, "TIMING run=%d elapsed=%.6f\n", r + 1, elapsed);
        }
    }

    if (rank == 0) {
        fprintf(stderr, "TIMING average=%.6f repeats=%d ranks=%d\n",
                total_time / cfg->repeats, cfg->repeats, nranks);
    }
}

void run_serial_search(const Config *cfg) {
    int rank, nranks;
    double total_time = 0.0;

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    if (nranks != 1) {
        if (rank == 0) fprintf(stderr, "Error: --impl serial requires np=1.\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    if (cfg->verbose) {
        fprintf(stderr,
                "[Config] impl=serial file=%s token='%s' repeats=%d chunk_mb=%d whole_word=%d expected_count=%lld\n",
                cfg->file, cfg->token, cfg->repeats, cfg->chunk_mb,
                cfg->whole_word, cfg->expected_count);
    }

    for (int r = 0; r < cfg->repeats; r++) {
        double elapsed = search_once(cfg, rank, nranks);
        total_time += elapsed;
        fprintf(stderr, "TIMING impl=serial run=%d elapsed=%.6f\n", r + 1, elapsed);
    }
    fprintf(stderr, "TIMING impl=serial average=%.6f repeats=%d ranks=1\n",
            total_time / cfg->repeats, cfg->repeats);
}
