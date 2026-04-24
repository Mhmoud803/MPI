#ifndef FILE_IO_H
#define FILE_IO_H

#include <stddef.h>

/**
 * Compute the byte range [*start, *start + *length) that rank `rank` is
 * responsible for, given a file of `file_size` bytes distributed among
 * `nranks` ranks. The last rank absorbs any remainder.
 */
void compute_range(int rank, int nranks, long long file_size,
                   long long *start, long long *length);

/**
 * Read exactly `length` bytes from file `path` starting at byte `offset`.
 * The caller must allocate buf (size >= length).
 *
 * @return  Number of bytes actually read (may be < length at EOF).
 *          Returns -1 on open/seek error.
 */
long long read_range(const char *path, long long offset,
                     long long length, char *buf);

/**
 * Return the total size of file `path` in bytes.
 * Returns -1 on error.
 */
long long file_size_bytes(const char *path);

#endif /* FILE_IO_H */
