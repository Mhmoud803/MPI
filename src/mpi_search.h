#ifndef MPI_SEARCH_H
#define MPI_SEARCH_H

#include "cli.h"

/**
 * Entry point for the distributed MPI search.
 * Runs cfg->repeats timed iterations and writes results to
 * cfg->output (or stdout if empty).
 * Must be called after MPI_Init and before MPI_Finalize.
 */
void run_distributed_search(const Config *cfg);
void run_serial_search(const Config *cfg);

#endif /* MPI_SEARCH_H */
