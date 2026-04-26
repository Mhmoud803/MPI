/*
 * mpi_search.c — Distributed search orchestrator
 *
 * Algorithm overview:
 *  1. Rank 0 stats the file and broadcasts file_size.
 *  2. Each rank computes its owned byte range [my_start, my_end).
 *  3. Each rank streams its full owned range in --chunk-mb windows.
 *  4. For each window, we read:
 *       - 1-byte left context (if available) for word-boundary checks,
 *       - (token_len-1) right lookahead for boundary-spanning matches.
 *  5. BMH search runs on this extended window; only matches whose start
 *     offset belongs to the rank's current owned subwindow are accepted.
 *  6. Each rank writes its own local offsets to rank-local output.
 *  7. Rank 0 verifies total match count via MPI_Reduce.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>

#include "mpi_search.h"
#include "cli.h"
#include "file_io.h"
#include "search_utils.h"

typedef struct {
    Match *data;
    int size;
    int capacity;
} MatchVec;

static void vec_init(MatchVec *v) {
    v->size = 0;
    v->capacity = 4096;
    v->data = (Match *)malloc((size_t)v->capacity * sizeof(Match));
    if (!v->data) {
        fprintf(stderr, "malloc failed for match vector\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
}

static void vec_push(MatchVec *v, long long off) {
    if (v->size == v->capacity) {
        int new_cap = v->capacity * 2;
        Match *next = (Match *)realloc(v->data, (size_t)new_cap * sizeof(Match));
        if (!next) {
            fprintf(stderr, "realloc failed while growing match vector\n");
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
        v->data = next;
        v->capacity = new_cap;
    }
    v->data[v->size].global_byte_offset = off;
    v->data[v->size].line = 0;
    v->data[v->size].col = 0;
    v->size++;
}

static void vec_free(MatchVec *v) {
    free(v->data);
    v->data = NULL;
    v->size = 0;
    v->capacity = 0;
}

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

static FILE *open_rank_output_file(const char *base_output, int rank) {
    const char *dot = strrchr(base_output, '.');
    const char *slash = strrchr(base_output, '/');
    int has_ext = (dot != NULL && (slash == NULL || dot > slash));
    int need = 0;
    char *path = NULL;
    FILE *fp = NULL;

    if (has_ext) {
        size_t stem_len = (size_t)(dot - base_output);
        need = snprintf(NULL, 0, "%.*s_rank_%d%s", (int)stem_len, base_output, rank, dot);
        path = (char *)malloc((size_t)need + 1);
        if (!path) return NULL;
        snprintf(path, (size_t)need + 1, "%.*s_rank_%d%s", (int)stem_len, base_output, rank, dot);
    } else {
        need = snprintf(NULL, 0, "%s_rank_%d.txt", base_output, rank);
        path = (char *)malloc((size_t)need + 1);
        if (!path) return NULL;
        snprintf(path, (size_t)need + 1, "%s_rank_%d.txt", base_output, rank);
    }

    fp = fopen(path, "w");
    free(path);
    return fp;
}

/* Shared streaming search core used by both MPI and serial execution paths.
 * It scans [start,end) in chunked windows and appends owned match offsets. */
static void stream_and_collect_offsets(const Config *cfg,
                                       int rank,
                                       long long file_size,
                                       long long start,
                                       long long end,
                                       MatchVec *out) {
    int tlen = (int)strlen(cfg->token);
    long long chunk_bytes = (long long)cfg->chunk_mb * 1024LL * 1024LL;
    long long remaining = end - start;
    int skip[BMH_ALPHA];
    int overlap = (tlen > 1) ? (tlen - 1) : 0;
    int max_local_buf_matches = 65536;
    Match *scratch = NULL;

    if (chunk_bytes < 1) chunk_bytes = 1;
    bmh_preprocess(cfg->token, tlen, skip);

    scratch = (Match *)malloc((size_t)max_local_buf_matches * sizeof(Match));
    if (!scratch) {
        fprintf(stderr, "Rank %d: malloc for scratch matches failed\n", rank);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    for (long long cur = start; remaining > 0; ) {
        long long own_len = remaining;
        if (own_len > chunk_bytes) own_len = chunk_bytes;

        long long left_extra = (cur > 0) ? 1 : 0;
        long long right_extra = overlap;
        if (cur + own_len + right_extra > file_size)
            right_extra = file_size - (cur + own_len);
        if (right_extra < 0) right_extra = 0;

        long long read_start = cur - left_extra;
        long long read_len = left_extra + own_len + right_extra;
        char *buf = (char *)malloc((size_t)read_len + 1);
        if (!buf) {
            fprintf(stderr, "Rank %d: malloc failed (%lld bytes)\n", rank, read_len);
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }

        long long got = read_range(cfg->file, read_start, read_len, buf);
        if (got < 0) {
            fprintf(stderr, "Rank %d: read_range failed in streaming loop\n", rank);
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
        buf[got] = '\0';

        int found;
        for (;;) {
            found = bmh_search(
                buf, got,
                cfg->token, tlen, skip,
                cfg->whole_word,
                read_start,
                1, 1,
                scratch, max_local_buf_matches
            );

            if (found < max_local_buf_matches) break;

            int new_cap = max_local_buf_matches * 2;
            Match *next = (Match *)realloc(scratch, (size_t)new_cap * sizeof(Match));
            if (!next) {
                fprintf(stderr, "Rank %d: realloc failed for scratch matches\n", rank);
                MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
            }
            scratch = next;
            max_local_buf_matches = new_cap;
        }

        for (int i = 0; i < found; i++) {
            long long off = scratch[i].global_byte_offset;
            if (off >= cur && off < cur + own_len && off >= start && off < end)
                vec_push(out, off);
        }

        free(buf);
        cur += own_len;
        remaining -= own_len;
    }

    free(scratch);
}

/* -----------------------------------------------------------------------
 * Single search run — called cfg->repeats times.
 * Returns elapsed wall-clock seconds (MPI_Wtime).
 * ----------------------------------------------------------------------- */
static double search_once(const Config *cfg, int rank, int nranks, FILE *out) {
    /* ------------------------------------------------------------------ */
    /* STEP 1: Get file size and broadcast it                              */
    /* ------------------------------------------------------------------ */
    long long file_size = 0;
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

    long long my_start, my_length;
    compute_range(rank, nranks, file_size, &my_start, &my_length);
    MPI_Barrier(MPI_COMM_WORLD);
    double t_start = MPI_Wtime();

    /* Each rank searches only within its exact mathematical ownership window. */
    long long my_end = my_start + my_length;
    MatchVec local;
    vec_init(&local);
    stream_and_collect_offsets(cfg, rank, file_size, my_start, my_end, &local);

    long long local_count = (long long)local.size;
    long long reduced_total = 0;
    MPI_Reduce(&local_count, &reduced_total, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);

    double t_end = MPI_Wtime();

    if (cfg->output[0] != '\0') {
        fprintf(out, "# Rank %d found %d occurrence(s) of \"%s\"\n",
                rank, local.size, cfg->token);
        for (int i = 0; i < local.size; i++) {
            fprintf(out, "%lld\n", local.data[i].global_byte_offset);
        }
    } else {
        fprintf(out, "[Rank %d] Found %d occurrence(s) of \"%s\"\n",
                rank, local.size, cfg->token);
        for (int i = 0; i < local.size; i++) {
            fprintf(out, "[Rank %d] Match at offset %lld\n",
                    rank, local.data[i].global_byte_offset);
        }
    }
    fflush(out);

    if (rank == 0) {
        print_verification_status(out, reduced_total, cfg->expected_count);
        fflush(out);
    }

    vec_free(&local);

    return t_end - t_start;
}

static double serial_search_once(const Config *cfg, FILE *out) {
    long long file_size = file_size_bytes(cfg->file);
    if (file_size < 0) {
        fprintf(stderr, "Error: cannot stat file '%s'\n", cfg->file);
        exit(EXIT_FAILURE);
    }
    if (file_size == 0) {
        fprintf(stderr, "Warning: file is empty.\n");
        return 0.0;
    }

    MatchVec all;
    vec_init(&all);

    double t_start = MPI_Wtime();
    stream_and_collect_offsets(cfg, 0, file_size, 0, file_size, &all);
    double t_end = MPI_Wtime();

    fprintf(out, "# Found %d occurrence(s) of \"%s\"\n", all.size, cfg->token);
    for (int i = 0; i < all.size; i++) {
        fprintf(out, "%lld\n", all.data[i].global_byte_offset);
    }
    print_verification_status(out, (long long)all.size, cfg->expected_count);
    fflush(out);

    vec_free(&all);
    return t_end - t_start;
}

/* -----------------------------------------------------------------------
 * Public entry point
 * ----------------------------------------------------------------------- */
void run_distributed_search(const Config *cfg) {
    int rank, nranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    if (cfg->verbose) {
        char node_name[MPI_MAX_PROCESSOR_NAME];
        int node_name_len = 0;
        MPI_Get_processor_name(node_name, &node_name_len);
        node_name[node_name_len] = '\0';

        fprintf(stderr,
                "[DEBUG] Process %d out of %d initialized on node %s\n",
                rank, nranks, node_name);
        fflush(stderr);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* Open rank-local output when --output is provided; otherwise use stdout. */
    FILE *out = stdout;
    if (cfg->output[0] != '\0') {
        out = open_rank_output_file(cfg->output, rank);
        if (!out) {
            perror("Cannot open output file");
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
    }

    if (rank == 0 && cfg->verbose) {
        fprintf(stderr,
                "[Config] file=%s token='%s' ranks=%d repeats=%d chunk_mb=%d whole_word=%d expected_count=%lld\n",
                cfg->file, cfg->token, nranks, cfg->repeats, cfg->chunk_mb, cfg->whole_word,
                cfg->expected_count);
    }

    double total_time = 0.0;

    for (int r = 0; r < cfg->repeats; r++) {
        if (rank == 0 && cfg->verbose)
            fprintf(stderr, "--- Run %d/%d ---\n", r + 1, cfg->repeats);

        double elapsed = search_once(cfg, rank, nranks, out);

        total_time += elapsed;

        if (rank == 0) {
            /* Print timing in a machine-parseable format for the CSV script */
            fprintf(stderr, "TIMING run=%d elapsed=%.6f\n", r + 1, elapsed);
        }
    }

    if (rank == 0) {
        fprintf(stderr, "TIMING average=%.6f repeats=%d ranks=%d\n",
                total_time / cfg->repeats, cfg->repeats, nranks);
    }
    if (out != stdout) fclose(out);
}

void run_serial_search(const Config *cfg) {
    int rank, nranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    if (nranks != 1) {
        if (rank == 0) {
            fprintf(stderr, "Error: --impl serial requires a single process (np=1).\n");
        }
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    FILE *out = stdout;
    if (cfg->output[0] != '\0') {
        out = fopen(cfg->output, "w");
        if (!out) {
            perror("Cannot open output file");
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
    }

    if (cfg->verbose) {
        fprintf(stderr,
                "[Config] impl=serial file=%s token='%s' repeats=%d chunk_mb=%d whole_word=%d expected_count=%lld\n",
                cfg->file, cfg->token, cfg->repeats, cfg->chunk_mb, cfg->whole_word,
                cfg->expected_count);
    }

    double total_time = 0.0;
    for (int r = 0; r < cfg->repeats; r++) {
        if (cfg->verbose) fprintf(stderr, "--- Serial run %d/%d ---\n", r + 1, cfg->repeats);
        double elapsed = serial_search_once(cfg, out);
        total_time += elapsed;
        fprintf(stderr, "TIMING impl=serial run=%d elapsed=%.6f\n", r + 1, elapsed);
    }
    fprintf(stderr, "TIMING impl=serial average=%.6f repeats=%d ranks=1\n",
            total_time / cfg->repeats, cfg->repeats);

    if (out != stdout) fclose(out);
}
