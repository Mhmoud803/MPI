/*
 * main.c — Entry point for the distributed text search program.
 *
 * Usage: mpirun -np <N> ./dist_search --token TOKEN --file PATH [OPTIONS]
 * Run with --help for full option list.
 */
#include <mpi.h>
#include <string.h>
#include "src/cli.h"
#include "src/mpi_search.h"

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    Config cfg;
    parse_args(argc, argv, &cfg, rank);

    if (strcmp(cfg.impl, "serial") == 0) {
        run_serial_search(&cfg);
    } else {
        run_distributed_search(&cfg);
    }

    MPI_Finalize();
    return 0;
}
