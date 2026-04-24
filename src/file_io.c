#include <stdio.h>
#include <stdlib.h>
#include "file_io.h"

/* -----------------------------------------------------------------------
 * Compute the [start, start+length) byte range for rank `rank`.
 * ----------------------------------------------------------------------- */
void compute_range(int rank, int nranks, long long file_size,
                   long long *start, long long *length) {
    long long chunk = file_size / nranks;
    *start  = (long long)rank * chunk;

    if (rank == nranks - 1)
        *length = file_size - *start;   /* last rank absorbs remainder */
    else
        *length = chunk;
}

/* -----------------------------------------------------------------------
 * Read `length` bytes from `path` beginning at byte `offset`.
 * ----------------------------------------------------------------------- */
long long read_range(const char *path, long long offset,
                     long long length, char *buf) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror("file_io: fopen");
        return -1;
    }

    /* Use a large kernel buffer to amortise syscall overhead */
    setvbuf(fp, NULL, _IOFBF, 64 * 1024 * 1024);

    if (fseeko(fp, (off_t)offset, SEEK_SET) != 0) {
        perror("file_io: fseeko");
        fclose(fp);
        return -1;
    }

    long long bytes_read = (long long)fread(buf, 1, (size_t)length, fp);

    if (ferror(fp)) {
        perror("file_io: fread");
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return bytes_read;
}

/* -----------------------------------------------------------------------
 * Return total file size in bytes.
 * ----------------------------------------------------------------------- */
long long file_size_bytes(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror("file_io: fopen (size check)");
        return -1;
    }
    if (fseeko(fp, 0, SEEK_END) != 0) {
        perror("file_io: fseeko (size check)");
        fclose(fp);
        return -1;
    }
    long long size = (long long)ftello(fp);
    fclose(fp);
    return size;
}
