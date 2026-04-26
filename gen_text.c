/*
 * gen_text.c
 *
 * Dynamic CLI-driven corpus generator for benchmarking.
 *
 * Required arguments:
 *   --out PATH
 *   --size-mb N
 *   --token STRING
 *   --occurrences N
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WRITE_BUF_SIZE (8ULL * 1024ULL * 1024ULL)

typedef struct {
    const char *out_path;
    unsigned long long size_mb;
    const char *token;
    unsigned long long occurrences;
} GenConfig;

/* Fast PRNG (xorshift32). */
static inline uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s --out PATH --size-mb N --token STRING --occurrences N\n",
            prog);
}

static int parse_ull(const char *s, unsigned long long *out) {
    char *end = NULL;
    unsigned long long val;
    errno = 0;
    val = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return 0;
    *out = val;
    return 1;
}

static int parse_args(int argc, char **argv, GenConfig *cfg) {
    cfg->out_path = NULL;
    cfg->size_mb = 0;
    cfg->token = NULL;
    cfg->occurrences = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0) {
            if (++i >= argc) return 0;
            cfg->out_path = argv[i];
        } else if (strcmp(argv[i], "--size-mb") == 0) {
            if (++i >= argc || !parse_ull(argv[i], &cfg->size_mb)) return 0;
        } else if (strcmp(argv[i], "--token") == 0) {
            if (++i >= argc) return 0;
            cfg->token = argv[i];
        } else if (strcmp(argv[i], "--occurrences") == 0) {
            if (++i >= argc || !parse_ull(argv[i], &cfg->occurrences)) return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        } else {
            return 0;
        }
    }

    if (!cfg->out_path || !cfg->token || cfg->size_mb == 0 || cfg->occurrences == 0) return 0;
    if (cfg->token[0] == '\0') return 0;
    return 1;
}

int main(int argc, char **argv) {
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        " \n";

    GenConfig cfg;
    unsigned long long total_bytes;
    unsigned long long interval;
    size_t token_len;
    FILE *fp = NULL;
    char *buf = NULL;
    uint32_t rng_state = (uint32_t)time(NULL) ^ 0xA5A5A5A5U;
    unsigned long long written = 0;
    unsigned long long inject_idx = 0;
    unsigned long long next_inject_pos = 0;
    size_t in_token_offset = 0;
    size_t charset_len = sizeof(charset) - 1U;
    unsigned char blocked_first;

    if (!parse_args(argc, argv, &cfg)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    token_len = strlen(cfg.token);
    blocked_first = (unsigned char)cfg.token[0];
    total_bytes = cfg.size_mb * 1024ULL * 1024ULL;
    interval = total_bytes / cfg.occurrences;

    if (total_bytes == 0 || interval == 0) {
        fprintf(stderr, "Invalid configuration: computed byte budget is zero.\n");
        return EXIT_FAILURE;
    }
    if (interval < (unsigned long long)token_len) {
        fprintf(stderr,
                "Invalid configuration: interval (%llu) < token length (%zu).\n",
                interval, token_len);
        return EXIT_FAILURE;
    }

    fp = fopen(cfg.out_path, "wb");
    if (!fp) {
        perror("fopen");
        return EXIT_FAILURE;
    }

    if (setvbuf(fp, NULL, _IOFBF, WRITE_BUF_SIZE) != 0) {
        fprintf(stderr, "Warning: failed to set stdio buffer size.\n");
    }

    buf = (char *)malloc((size_t)WRITE_BUF_SIZE);
    if (!buf) {
        perror("malloc");
        fclose(fp);
        return EXIT_FAILURE;
    }

    while (written < total_bytes) {
        unsigned long long remaining = total_bytes - written;
        size_t chunk = (remaining > WRITE_BUF_SIZE) ? (size_t)WRITE_BUF_SIZE : (size_t)remaining;

        for (size_t i = 0; i < chunk; i++) {
            unsigned long long pos = written + (unsigned long long)i;

            if (in_token_offset > 0) {
                buf[i] = cfg.token[in_token_offset];
                in_token_offset++;
                if (in_token_offset == token_len) {
                    in_token_offset = 0;
                    inject_idx++;
                    if (inject_idx < cfg.occurrences) {
                        next_inject_pos = inject_idx * interval;
                    }
                }
                continue;
            }

            if (inject_idx < cfg.occurrences && pos == next_inject_pos) {
                buf[i] = cfg.token[0];
                if (token_len > 1) {
                    in_token_offset = 1;
                } else {
                    inject_idx++;
                    if (inject_idx < cfg.occurrences) {
                        next_inject_pos = inject_idx * interval;
                    }
                }
                continue;
            }

            for (;;) {
                char c = charset[xorshift32(&rng_state) % charset_len];
                if ((unsigned char)c != blocked_first) {
                    buf[i] = c;
                    break;
                }
            }
        }

        if (fwrite(buf, 1, chunk, fp) != chunk) {
            perror("fwrite");
            free(buf);
            fclose(fp);
            return EXIT_FAILURE;
        }
        written += (unsigned long long)chunk;
    }

    if (in_token_offset != 0) {
        fprintf(stderr, "Generation error: token boundary state not clean at EOF.\n");
        free(buf);
        fclose(fp);
        return EXIT_FAILURE;
    }
    if (inject_idx != cfg.occurrences) {
        fprintf(stderr,
                "Generation error: injected=%llu expected=%llu.\n",
                inject_idx, cfg.occurrences);
        free(buf);
        fclose(fp);
        return EXIT_FAILURE;
    }
    if (fclose(fp) != 0) {
        perror("fclose");
        free(buf);
        return EXIT_FAILURE;
    }

    free(buf);

    printf("Generated %s\n", cfg.out_path);
    printf("Total bytes: %llu\n", total_bytes);
    printf("Token: %s\n", cfg.token);
    printf("Occurrences: %llu\n", cfg.occurrences);
    printf("Interval: %llu bytes\n", interval);
    printf("Expected token count in final file: %llu\n", cfg.occurrences);
    return EXIT_SUCCESS;
}
