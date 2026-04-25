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
 *  6. Rank 0 gathers all offsets, sorts globally, then computes line/col
 *     in a single sequential scan of the file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <mpi.h>

#include "mpi_search.h"
#include "cli.h"
#include "file_io.h"
#include "search_utils.h"

#define EXPECTED_TOTAL_OCCURRENCES 10000000LL

typedef struct {
    Match *data;
    int size;
    int capacity;
} MatchVec;

/* -----------------------------------------------------------------------
 * Comparison function for qsort on Match structs (sort by byte offset).
 * ----------------------------------------------------------------------- */
static int match_cmp(const void *a, const void *b) {
    const Match *ma = (const Match *)a;
    const Match *mb = (const Match *)b;
    if (ma->global_byte_offset < mb->global_byte_offset) return -1;
    if (ma->global_byte_offset > mb->global_byte_offset) return  1;
    return 0;
}

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

static void annotate_line_col_from_file(const char *path, Match *matches, int n) {
    if (n <= 0) return;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror("Cannot open file for line/col annotation");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    const size_t BUFSZ = 64ULL * 1024ULL * 1024ULL;
    char *buf = (char *)malloc(BUFSZ);
    if (!buf) {
        fprintf(stderr, "malloc failed for annotation buffer\n");
        fclose(fp);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    long long pos = 0;
    int line = 1;
    int col = 1;
    int mi = 0;

    while (mi < n) {
        size_t got = fread(buf, 1, BUFSZ, fp);
        if (got == 0) break;

        for (size_t i = 0; i < got && mi < n; i++) {
            while (mi < n && matches[mi].global_byte_offset == pos) {
                matches[mi].line = line;
                matches[mi].col = col;
                mi++;
            }

            if (buf[i] == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
            pos++;
        }
    }

    free(buf);
    fclose(fp);
}

/* -----------------------------------------------------------------------
 * Single search run — called cfg->repeats times.
 * Returns elapsed wall-clock seconds (MPI_Wtime).
 * ----------------------------------------------------------------------- */
static double search_once(const Config *cfg, int rank, int nranks, FILE *out) {
    int tlen = (int)strlen(cfg->token);

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

    /* Each rank is first limited to its exact mathematical ownership window.
     * Example: 100 bytes / 2 ranks => rank windows [0,50) and [50,100).
     * --chunk-mb only controls sub-chunking *within* this owned window. */
    long long my_end = my_start + my_length;
    long long my_remaining_total = my_length;
    long long chunk_bytes = (long long)cfg->chunk_mb * 1024LL * 1024LL;
    if (chunk_bytes < 1) chunk_bytes = 1;

    int skip[BMH_ALPHA];
    bmh_preprocess(cfg->token, tlen, skip);
    MatchVec local;
    vec_init(&local);

    int overlap = (tlen > 1) ? (tlen - 1) : 0;
    int max_local_buf_matches = 65536;
    Match *scratch = (Match *)malloc((size_t)max_local_buf_matches * sizeof(Match));
    if (!scratch) {
        fprintf(stderr, "Rank %d: malloc for scratch matches failed\n", rank);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    for (long long cur = my_start; my_remaining_total > 0; ) {
        /* Small files: own_len becomes the exact remaining owned range.
         * Large files: own_len is bounded by chunk_bytes for streaming/OOM safety. */
        long long own_len = my_remaining_total;
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
            if (off >= cur && off < cur + own_len && off >= my_start && off < my_end)
                vec_push(&local, off);
        }

        free(buf);
        cur += own_len;
        my_remaining_total -= own_len;
    }

    free(scratch);

    long long local_count = (long long)local.size;
    long long reduced_total = 0;
    MPI_Reduce(&local_count, &reduced_total, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);

    int *recv_counts = NULL;
    int *displs      = NULL;
    int total_matches = 0;

    if (rank == 0) {
        recv_counts = (int *)malloc(sizeof(int) * nranks);
        displs      = (int *)malloc(sizeof(int) * nranks);
    }

    /* Each element is a Match (3 fields). We pack them as raw bytes via a
     * derived datatype to keep the gather simple. */
    MPI_Gather(&local.size, 1, MPI_INT,
               recv_counts,  1, MPI_INT,
               0, MPI_COMM_WORLD);

    Match *all_matches = NULL;
    if (rank == 0) {
        displs[0] = 0;
        for (int i = 0; i < nranks; i++) {
            total_matches += recv_counts[i];
            if (i > 0) displs[i] = displs[i-1] + recv_counts[i-1];
        }
        all_matches = (Match *)malloc(sizeof(Match) * (total_matches + 1));
        if (!all_matches) {
            fprintf(stderr, "Rank 0: malloc for all_matches failed\n");
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
    }

    /* Create an MPI datatype for Match */
    MPI_Datatype mpi_match_type;
    int          blklens[3] = {1, 1, 1};
    MPI_Aint     disps[3];
    MPI_Datatype types[3]   = {MPI_LONG_LONG, MPI_INT, MPI_INT};
    disps[0] = offsetof(Match, global_byte_offset);
    disps[1] = offsetof(Match, line);
    disps[2] = offsetof(Match, col);
    MPI_Type_create_struct(3, blklens, disps, types, &mpi_match_type);
    MPI_Type_commit(&mpi_match_type);

    MPI_Gatherv(local.data, local.size, mpi_match_type,
                all_matches,  recv_counts, displs, mpi_match_type,
                0, MPI_COMM_WORLD);

    MPI_Type_free(&mpi_match_type);

    double t_end = MPI_Wtime();

    if (rank == 0) {
        qsort(all_matches, total_matches, sizeof(Match), match_cmp);
        annotate_line_col_from_file(cfg->file, all_matches, total_matches);

        fprintf(out, "# Found %d occurrence(s) of \"%s\"\n",
                total_matches, cfg->token);
        for (int i = 0; i < total_matches; i++) {
            fprintf(out, "line %d, col %d\n",
                    all_matches[i].line, all_matches[i].col);
        }
        fflush(out);

        if (reduced_total == EXPECTED_TOTAL_OCCURRENCES) {
            fprintf(out, "[VERIFICATION PASSED] total=%lld expected=%lld\n",
                    reduced_total, (long long)EXPECTED_TOTAL_OCCURRENCES);
        } else {
            fprintf(out, "[VERIFICATION FAILED] total=%lld expected=%lld\n",
                    reduced_total, (long long)EXPECTED_TOTAL_OCCURRENCES);
        }
        fflush(out);

        free(recv_counts);
        free(displs);
        free(all_matches);
    }

    vec_free(&local);

    return t_end - t_start;
}

static double serial_search_once(const Config *cfg, FILE *out) {
    int tlen = (int)strlen(cfg->token);
    long long file_size = file_size_bytes(cfg->file);
    if (file_size < 0) {
        fprintf(stderr, "Error: cannot stat file '%s'\n", cfg->file);
        exit(EXIT_FAILURE);
    }
    if (file_size == 0) {
        fprintf(stderr, "Warning: file is empty.\n");
        return 0.0;
    }

    long long chunk_bytes = (long long)cfg->chunk_mb * 1024LL * 1024LL;
    if (chunk_bytes < 1) chunk_bytes = 1;

    int skip[BMH_ALPHA];
    bmh_preprocess(cfg->token, tlen, skip);

    MatchVec all;
    vec_init(&all);

    int max_buf_matches = 65536;
    Match *scratch = (Match *)malloc((size_t)max_buf_matches * sizeof(Match));
    if (!scratch) {
        fprintf(stderr, "malloc failed for serial scratch matches\n");
        exit(EXIT_FAILURE);
    }

    int overlap = (tlen > 1) ? (tlen - 1) : 0;

    double t_start = MPI_Wtime();
    for (long long cur = 0; cur < file_size; ) {
        long long own_len = file_size - cur;
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
            fprintf(stderr, "malloc failed in serial loop (%lld bytes)\n", read_len);
            exit(EXIT_FAILURE);
        }

        long long got = read_range(cfg->file, read_start, read_len, buf);
        if (got < 0) {
            fprintf(stderr, "read_range failed in serial streaming loop\n");
            exit(EXIT_FAILURE);
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
                scratch, max_buf_matches
            );
            if (found < max_buf_matches) break;

            int new_cap = max_buf_matches * 2;
            Match *next = (Match *)realloc(scratch, (size_t)new_cap * sizeof(Match));
            if (!next) {
                fprintf(stderr, "realloc failed for serial scratch matches\n");
                exit(EXIT_FAILURE);
            }
            scratch = next;
            max_buf_matches = new_cap;
        }

        for (int i = 0; i < found; i++) {
            long long off = scratch[i].global_byte_offset;
            if (off >= cur && off < cur + own_len) vec_push(&all, off);
        }

        free(buf);
        cur += own_len;
    }
    double t_end = MPI_Wtime();

    qsort(all.data, all.size, sizeof(Match), match_cmp);
    annotate_line_col_from_file(cfg->file, all.data, all.size);

    fprintf(out, "# Found %d occurrence(s) of \"%s\"\n", all.size, cfg->token);
    for (int i = 0; i < all.size; i++) {
        fprintf(out, "line %d, col %d\n", all.data[i].line, all.data[i].col);
    }
    if ((long long)all.size == EXPECTED_TOTAL_OCCURRENCES) {
        fprintf(out, "[VERIFICATION PASSED] total=%d expected=%lld\n",
                all.size, (long long)EXPECTED_TOTAL_OCCURRENCES);
    } else {
        fprintf(out, "[VERIFICATION FAILED] total=%d expected=%lld\n",
                all.size, (long long)EXPECTED_TOTAL_OCCURRENCES);
    }
    fflush(out);

    free(scratch);
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

    /* Open output file (rank 0 only) */
    FILE *out = stdout;
    if (rank == 0 && cfg->output[0] != '\0') {
        out = fopen(cfg->output, "w");
        if (!out) {
            perror("Cannot open output file");
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
    }

    if (rank == 0 && cfg->verbose) {
        fprintf(stderr,
                "[Config] file=%s token='%s' ranks=%d repeats=%d chunk_mb=%d whole_word=%d\n",
                cfg->file, cfg->token, nranks, cfg->repeats, cfg->chunk_mb, cfg->whole_word);
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
        if (out != stdout) fclose(out);
    }
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
                "[Config] impl=serial file=%s token='%s' repeats=%d chunk_mb=%d whole_word=%d\n",
                cfg->file, cfg->token, cfg->repeats, cfg->chunk_mb, cfg->whole_word);
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
