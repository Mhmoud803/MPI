CC      = mpicc
GCC     = gcc
CFLAGS  = -O2 -Wall -Wextra -D_FILE_OFFSET_BITS=64
LDFLAGS =

SRC_DIR = src
SRC     = $(SRC_DIR)/cli.c \
           $(SRC_DIR)/search_utils.c \
           $(SRC_DIR)/file_io.c \
           $(SRC_DIR)/mpi_search.c

BIN      = dist_search
GEN_BIN  = gen_text

.PHONY: all clean gen help test

all: $(BIN) $(GEN_BIN)

## Build the distributed search binary
$(BIN): main.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

## Build the text generator (plain gcc, no MPI needed)
$(GEN_BIN): gen_text.c
	$(GCC) $(CFLAGS) -o $@ $^

## Shorthand: just build the generator
gen: $(GEN_BIN)

## Remove compiled binaries
clean:
	rm -f $(BIN) $(GEN_BIN)

help:
	@echo "Targets:"
	@echo "  all       Build dist_search and gen_text (default)"
	@echo "  gen       Build only gen_text"
	@echo "  test      Run search-logic regression tests"
	@echo "  clean     Remove compiled binaries"
	@echo ""
	@echo "Run search:"
	@echo "  mpirun -np 4 ./dist_search --token FINDME --file corpus.txt --repeats 5"
	@echo ""
	@echo "Generate test corpus:"
	@echo "  ./gen_text --size-gb 10 --token FINDME --seed-count 1000 --out corpus.txt"

test: $(BIN)
	bash tests/test_logic.sh
