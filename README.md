# MPI Distributed Text Search

A high-performance, distributed computing project that searches for a specific substring (token) inside a massive 10 GB plain-text file. 

This project demonstrates advanced High-Performance Computing (HPC) concepts, utilizing the **Boyer-Moore-Horspool (BMH)** algorithm for sub-linear string matching and **Message Passing Interface (MPI)** for parallel execution across a containerized cluster.

## Team Members
* **Mahmoud Atef Mahmoud AbdelAziz** - Core Search Algorithm & MPI Architecture
* **Mohamed Alaa** - Project Initialization & Data Generation
* **Abdelrahman Osama** - Infrastructure, Testing & Visualization

## Architecture Highlights
* **Single-Pass Design:** Reads the massive dataset exactly once per rank, preventing heavy disk I/O bottlenecks.
* **Dynamic Memory Allocation:** Stores match metadata (`local_row`, `col`, `byte_offset`) in dynamically resizing RAM arrays to prevent memory fragmentation.
* **MPI Prefix Sum:** Uses `MPI_Exscan` to instantly calculate accurate global line numbers without requiring a pre-count pass.
* **Deferred I/O:** Flushes all results to isolated, rank-specific text files (`matches_rank_<N>.txt`) to completely eliminate file-locking race conditions.

---

## Quick Start Guide

### 1. Prerequisites
This project is fully containerized to simulate a multi-node HPC cluster. You must have the following installed on your host machine:
* [Docker](https://docs.docker.com/get-docker/)
* [Docker Compose](https://docs.docker.com/compose/install/)

### 2. Booting the Cluster
Bring up the MPI cluster in the background using Docker Compose:
```bash
docker compose up -d --scale worker=3
```
Attach your terminal to the master node where you will compile and run the code:
```bash
docker compose exec master bash
```

### 3. Generating the Dataset
(Inside the mpi-master container)
Before searching, generate the 10 GB text corpus. This is handled by our custom C generator to ensure a standardized testing baseline:
```bash
make clean
make
```

### 4. Running the Automated Benchmarks
(Inside the mpi-master container)
We have provided an automated bash script that handles compiling the MPI C code, running the sequential baseline, and executing the parallel runs across 2, 4, and 8 cores.
```bash
chmod +x run_experiments.sh
./run_experiments.sh
```
Or can run speciifc configurations:
```bash
mpirun --hostfile hostfile -np 4 --allow-run-as-root ./dist_search --file corpus_10gb.txt --token "ManchesterUnited" --impl mpi --expected-count 10000000 --verbose
```

## Viewing the Results
Once the `run_experiments.sh` script completes, all outputs will be neatly organized in the `results/` directory:

* **Match Outputs:** Check `results/matches_rank_<N>.txt` to see the exact global row, column, and byte offset for every match found by a specific worker.
* **Raw Metrics:** `results/metrics.csv` contains the exact elapsed times (in seconds) for every run.
* **Performance Visualizations:** The pipeline automatically triggers `plot_results.py` to generate `results/scaling_plot.png` and `results/scaling_plot.pdf`. These graphs visualize the cluster's Time vs. MPI Processes and actual Speedup vs. Ideal Speedup (Amdahl's Law).

## Cleanup
To cleanly shut down the cluster and remove the network containers, run this from your host machine:
```bash
docker compose down
```
