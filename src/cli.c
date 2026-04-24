#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "cli.h"

void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: mpirun -np <N> %s [OPTIONS]\n"
        "\n"
        "Required:\n"
        "  --token TOKEN       Whole-word token to search for\n"
        "  --file  PATH        Input text file (default: corpus.txt)\n"
        "\n"
        "Optional:\n"
        "  --impl  mpi|serial  Implementation variant (default: mpi)\n"
        "  --repeats N         Number of timed runs (default: 1)\n"
        "  --output FILE       Write match results to FILE (default: stdout)\n"
        "  --chunk-mb N        Per-rank read buffer size in MB (default: 64)\n"
        "  --whole-word 0|1    1=whole-word matches, 0=substring (default: 1)\n"
        "  --verbose           Print extra progress/debug info\n"
        "  --debug             Alias for --verbose\n"
        "  --help              Show this message\n",
        prog);
}

void parse_args(int argc, char **argv, Config *cfg, int rank) {
    const char *missing_opt = NULL;

    /* Set defaults */
    strncpy(cfg->impl,   "mpi",        sizeof(cfg->impl) - 1);
    cfg->impl[sizeof(cfg->impl) - 1] = '\0';
    strncpy(cfg->file,   "corpus.txt", sizeof(cfg->file) - 1);
    cfg->file[sizeof(cfg->file) - 1] = '\0';
    cfg->output[0] = '\0';
    cfg->token[0]  = '\0';
    cfg->repeats   = 1;
    cfg->chunk_mb  = 64;
    cfg->whole_word = 1;
    cfg->verbose   = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            if (rank == 0) print_usage(argv[0]);
            MPI_Finalize();
            exit(EXIT_SUCCESS);

        } else if (strcmp(argv[i], "--impl") == 0) {
            missing_opt = "--impl";
            if (++i >= argc) goto missing;
            strncpy(cfg->impl, argv[i], sizeof(cfg->impl) - 1);
            cfg->impl[sizeof(cfg->impl) - 1] = '\0';

        } else if (strcmp(argv[i], "--repeats") == 0) {
            missing_opt = "--repeats";
            if (++i >= argc) goto missing;
            cfg->repeats = atoi(argv[i]);
            if (cfg->repeats < 1) cfg->repeats = 1;

        } else if (strcmp(argv[i], "--output") == 0) {
            missing_opt = "--output";
            if (++i >= argc) goto missing;
            strncpy(cfg->output, argv[i], sizeof(cfg->output) - 1);
            cfg->output[sizeof(cfg->output) - 1] = '\0';

        } else if (strcmp(argv[i], "--token") == 0) {
            missing_opt = "--token";
            if (++i >= argc) goto missing;
            strncpy(cfg->token, argv[i], sizeof(cfg->token) - 1);
            cfg->token[sizeof(cfg->token) - 1] = '\0';

        } else if (strcmp(argv[i], "--file") == 0) {
            missing_opt = "--file";
            if (++i >= argc) goto missing;
            strncpy(cfg->file, argv[i], sizeof(cfg->file) - 1);
            cfg->file[sizeof(cfg->file) - 1] = '\0';

        } else if (strcmp(argv[i], "--chunk-mb") == 0) {
            missing_opt = "--chunk-mb";
            if (++i >= argc) goto missing;
            cfg->chunk_mb = atoi(argv[i]);
            if (cfg->chunk_mb < 1) cfg->chunk_mb = 1;

        } else if (strcmp(argv[i], "--whole-word") == 0) {
            missing_opt = "--whole-word";
            if (++i >= argc) goto missing;
            cfg->whole_word = atoi(argv[i]) ? 1 : 0;

        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = 1;

        } else if (strcmp(argv[i], "--debug") == 0) {
            cfg->verbose = 1;

        } else {
            if (rank == 0)
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
            if (rank == 0) print_usage(argv[0]);
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
    }

    /* --token is mandatory */
    if (cfg->token[0] == '\0') {
        if (rank == 0) {
            fprintf(stderr, "Error: --token is required.\n");
            print_usage(argv[0]);
        }
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    if (strcmp(cfg->impl, "mpi") != 0 && strcmp(cfg->impl, "serial") != 0) {
        if (rank == 0) {
            fprintf(stderr, "Error: --impl must be 'mpi' or 'serial' (got '%s').\n", cfg->impl);
            print_usage(argv[0]);
        }
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    return;

missing:
    if (rank == 0)
        fprintf(stderr, "Error: option %s requires an argument.\n",
                missing_opt ? missing_opt : "(unknown)");
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
}
