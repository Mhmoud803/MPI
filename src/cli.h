#ifndef CLI_H
#define CLI_H

/* Maximum path length for file arguments */
#define MAX_PATH 1024
#define MAX_TOKEN 256

typedef struct {
    char impl[32];        /* --impl : "mpi" (default) */
    int  repeats;         /* --repeats N : number of timed runs */
    char output[MAX_PATH];/* --output FILE : result file, "" = stdout */
    char token[MAX_TOKEN];/* --token TOKEN : whole-word token to search */
    char file[MAX_PATH];  /* --file PATH  : input text file */
    int  chunk_mb;        /* --chunk-mb N : per-rank read buffer in MB */
    int  whole_word;      /* --whole-word 0|1 : enforce token boundaries */
    int  verbose;         /* --verbose    : print extra info */
    long long expected_count; /* --expected-count N : optional verification target */
} Config;

/**
 * Parse command-line arguments into cfg.
 * Prints usage and calls MPI_Abort on error.
 * rank is passed so only rank 0 prints usage messages.
 */
void parse_args(int argc, char **argv, Config *cfg, int rank);

/** Print usage string to stderr. */
void print_usage(const char *prog);

#endif /* CLI_H */
