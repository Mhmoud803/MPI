/*
 * gen_text.c
 *
 * Fast 10 GB corpus generator:
 * - Build one 10 MB random text chunk in memory.
 * - Inject " ManchesterUnited " exactly 10,000 times in that chunk.
 * - Write that same chunk 1,000 times to produce corpus_10gb.txt (10,000,000,000 bytes).
 *
 * Build: gcc -O3 -march=native -o gen_text gen_text.c
 * Run:   ./gen_text
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OUTPUT_FILE "corpus_10gb.txt"
#define CHUNK_SIZE (10U * 1000U * 1000U)   /* 10,000,000 bytes (10 MB decimal) */
#define CHUNK_WRITES 1000U                 /* 10,000,000 * 1000 = 10,000,000,000 bytes */
#define INJECTIONS_PER_CHUNK 10000U
#define TOKEN " ManchesterUnited "

/* Fast small PRNG (xorshift32), much faster than rand(). */
static inline uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void fill_random_text(char *buffer, size_t size, uint32_t *rng_state) {
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        " \n";
    const size_t charset_len = sizeof(charset) - 1U;

    for (size_t i = 0; i < size; ++i) {
        buffer[i] = charset[xorshift32(rng_state) % charset_len];
    }
}

int main(void) {
    const char *token = TOKEN;
    const size_t token_len = strlen(token);
    const size_t max_start = CHUNK_SIZE - token_len;
    uint32_t rng_state = (uint32_t)time(NULL) ^ 0xA5A5A5A5U;

    if (token_len == 0U || token_len > CHUNK_SIZE) {
        fprintf(stderr, "Invalid token length.\n");
        return 1;
    }

    char *chunk = (char *)malloc(CHUNK_SIZE);
    if (chunk == NULL) {
        perror("malloc chunk");
        return 1;
    }

    /* 1 byte per position to prevent overlapping injected tokens. */
    unsigned char *occupied = (unsigned char *)calloc(CHUNK_SIZE, 1U);
    if (occupied == NULL) {
        perror("calloc occupied");
        free(chunk);
        return 1;
    }

    fill_random_text(chunk, CHUNK_SIZE, &rng_state);

    size_t injected = 0;
    while (injected < INJECTIONS_PER_CHUNK) {
        const size_t pos = (size_t)(xorshift32(&rng_state) % (max_start + 1U));
        int overlaps = 0;

        for (size_t j = 0; j < token_len; ++j) {
            if (occupied[pos + j] != 0U) {
                overlaps = 1;
                break;
            }
        }

        if (overlaps) {
            continue;
        }

        memcpy(chunk + pos, token, token_len);
        for (size_t j = 0; j < token_len; ++j) {
            occupied[pos + j] = 1U;
        }
        ++injected;
    }

    FILE *fp = fopen(OUTPUT_FILE, "wb");
    if (fp == NULL) {
        perror("fopen output");
        free(occupied);
        free(chunk);
        return 1;
    }

    /* Large stdio write buffer to reduce syscall overhead. */
    if (setvbuf(fp, NULL, _IOFBF, 8U * 1024U * 1024U) != 0) {
        fprintf(stderr, "Warning: failed to set output buffer.\n");
    }

    for (size_t i = 0; i < CHUNK_WRITES; ++i) {
        size_t written = fwrite(chunk, 1, CHUNK_SIZE, fp);
        if (written != CHUNK_SIZE) {
            perror("fwrite");
            fclose(fp);
            free(occupied);
            free(chunk);
            return 1;
        }
    }

    if (fclose(fp) != 0) {
        perror("fclose");
        free(occupied);
        free(chunk);
        return 1;
    }

    free(occupied);
    free(chunk);

    printf("Generated %s\n", OUTPUT_FILE);
    printf("Chunk size: %u bytes\n", CHUNK_SIZE);
    printf("Chunk writes: %u\n", CHUNK_WRITES);
    printf("Injected token per chunk: %u\n", INJECTIONS_PER_CHUNK);
    printf("Expected token count in final file: %u\n", INJECTIONS_PER_CHUNK * CHUNK_WRITES);

    return 0;
}
