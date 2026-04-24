#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "search_utils.h"

/* -----------------------------------------------------------------------
 * BMH preprocessing
 * ----------------------------------------------------------------------- */
void bmh_preprocess(const char *token, int tlen, int skip[BMH_ALPHA]) {
    /* Default: skip the full token length (token length as bad-char distance) */
    for (int i = 0; i < BMH_ALPHA; i++)
        skip[i] = tlen;

    /* For every character in the pattern except the last, record how far
     * it is from the end so we can skip past mismatches efficiently. */
    for (int i = 0; i < tlen - 1; i++)
        skip[(unsigned char)token[i]] = tlen - 1 - i;
}

/* -----------------------------------------------------------------------
 * Word-boundary helper
 * ----------------------------------------------------------------------- */
int is_word_boundary(const char *buf, long long pos,
                     long long blen, int tlen) {
    /* Check character before the match */
    if (pos > 0 && (isalnum((unsigned char)buf[pos - 1]) || buf[pos - 1] == '_'))
        return 0;
    /* Check character after the match */
    long long end = pos + tlen;
    if (end < blen && (isalnum((unsigned char)buf[end]) || buf[end] == '_'))
        return 0;
    return 1;
}

/* -----------------------------------------------------------------------
 * BMH search
 * ----------------------------------------------------------------------- */
int bmh_search(const char *buf, long long blen,
               const char *token, int tlen,
               const int skip[BMH_ALPHA],
               int whole_word,
               long long base_offset,
               long long base_line, long long base_col,
               Match *matches, int max_matches) {
    int found = 0;

    if (tlen == 0 || blen < tlen) return 0;

    /* Track current line and column as we advance through the buffer.
     * These start at the values provided by MPI_Exscan. */
    long long cur_line = base_line;
    long long cur_col  = base_col;
    long long line_tracker_pos = 0; /* how far we've counted newlines so far */

    long long pos = 0;
    while (pos <= blen - tlen) {
        /* BMH: compare from the end of the pattern */
        int j = tlen - 1;
        while (j >= 0 && buf[pos + j] == token[j])
            j--;

        if (j < 0) {
            /* Candidate match at buf[pos] — enforce whole-word boundary */
            if (!whole_word || is_word_boundary(buf, pos, blen, tlen)) {
                /* Advance line/col tracker up to pos */
                for (; line_tracker_pos < pos; line_tracker_pos++) {
                    if (buf[line_tracker_pos] == '\n') {
                        cur_line++;
                        cur_col = 1;
                    } else {
                        cur_col++;
                    }
                }

                if (found < max_matches) {
                    matches[found].global_byte_offset = base_offset + pos;
                    matches[found].line = (int)cur_line;
                    matches[found].col  = (int)cur_col;
                    found++;
                } else {
                    /* Safety: caller didn't allocate enough space */
                    fprintf(stderr,
                        "Warning: match buffer full (%d). Some results dropped.\n",
                        max_matches);
                    break;
                }
            }
            pos++; /* Move forward by 1 so we don't re-examine same position */
        } else {
            /* Use skip table to jump forward */
            int jump = skip[(unsigned char)buf[pos + tlen - 1]];
            pos += jump;
        }
    }
    return found;
}
