#ifndef SEARCH_UTILS_H
#define SEARCH_UTILS_H

#include <stddef.h>

#define BMH_ALPHA 256  /* Alphabet size for bad-character table */

/**
 * A single token match.
 * global_byte_offset : byte position from the very start of the file.
 * line               : 1-based line number.
 * col                : 1-based column number (byte offset within line).
 */
typedef struct {
    long long global_byte_offset;
    int line;
    int col;
} Match;

/**
 * Precompute the Boyer-Moore-Horspool bad-character skip table.
 *
 * @param token  Pattern to search for.
 * @param tlen   Length of the pattern.
 * @param skip   Output: 256-entry skip table (caller-allocated).
 */
void bmh_preprocess(const char *token, int tlen, int skip[BMH_ALPHA]);

/**
 * Search for token inside buf using the precomputed skip table.
 * Whole-word boundary check is applied at each candidate position.
 *
 * @param buf            Buffer to search.
 * @param blen           Length of buf (bytes actually valid for search).
 * @param token          The search pattern.
 * @param tlen           Pattern length.
 * @param skip           Precomputed BMH skip table.
 * @param base_offset    Byte offset of buf[0] inside the file (used to
 *                       populate Match.global_byte_offset).
 * @param base_line      1-based line number at buf[0] (from MPI_Exscan).
 * @param base_col       1-based column number at buf[0].
 * @param[out] matches   Caller-allocated array; must be large enough.
 * @param max_matches    Capacity of matches[].
 * @return               Number of matches found (≤ max_matches).
 */
int bmh_search(const char *buf, long long blen,
               const char *token, int tlen,
               const int skip[BMH_ALPHA],
               int whole_word,
               long long base_offset,
               long long base_line, long long base_col,
               Match *matches, int max_matches);

/**
 * Check whether the match at buf[pos..pos+tlen-1] is a whole-word match.
 * A match is whole-word if the characters immediately before and after it
 * are non-alphanumeric (or at the buffer boundary).
 *
 * @param buf   Buffer (the *search* buffer, NOT the overlap-prepended one).
 * @param pos   Start position of match inside buf.
 * @param blen  Length of buf.
 * @param tlen  Token length.
 * @return      1 if whole-word, 0 otherwise.
 */
int is_word_boundary(const char *buf, long long pos,
                     long long blen, int tlen);

#endif /* SEARCH_UTILS_H */
