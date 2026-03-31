#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

#include "../common/common.h"

/* -------------------------------------------------------------------------
 * Directory helpers
 * ---------------------------------------------------------------------- */

/* Create directory (and any missing parents) if it does not already exist. */
void ensure_dir(const char *dir);

/* -------------------------------------------------------------------------
 * Line directive map — built from gcc's "# linenum \"file\"" output
 * ---------------------------------------------------------------------- */

typedef struct {
    int  preprocessed_line;   /* line number in preprocessor output */
    char file[MAX_PATH];      /* source file rel_path */
    int  source_line;         /* line number within that source file */
} LineMapEntry;

typedef struct {
    LineMapEntry *entries;
    int           count;
    int           capacity;
} LineMap;

LineMap *linemap_create(void);
void     linemap_free(LineMap *map);
/* Parse gcc line directives and build map. Returns map (caller frees). */
LineMap *parse_line_directives(const char *preprocessor_output);
/* Return source file and line for a given preprocessed line. Returns 0 if not found. */
int      linemap_lookup(const LineMap *map, int preprocessed_line,
                        char *file_out, int file_out_size);
/* Strip "# <num> \"...\"" lines from content. Returns new malloc'd string. */
char    *strip_line_directives(char *content);

/* -------------------------------------------------------------------------
 * C preprocessor execution
 *
 * Writes combined content to a temp file under .build_cache/, runs
 * gcc -E -x c with any -D flags and -I source dir, then reads and
 * returns the preprocessed result as a new malloc'd string.
 * If map_out is not NULL, parses line directives before stripping them.
 * Exits the process on failure.
 * ---------------------------------------------------------------------- */

char *run_preprocessor(const char *content, LineMap **map_out);

/* -------------------------------------------------------------------------
 * Pragma-marker conversion
 *
 * Converts every:
 *   #pragma BUNDLER_FILE <path>
 * line into:
 *   // === <path> ===
 *
 * Called AFTER preprocessing (and after strip_comments) so the pragma
 * markers survive both steps.
 *
 * Takes ownership of content (frees it). Returns new malloc'd string.
 * ---------------------------------------------------------------------- */

char *convert_pragma_markers(char *content);

/* -------------------------------------------------------------------------
 * #include_lib protection
 *
 * protect_lib_includes  – called BEFORE run_preprocessor.
 *   Converts  #include_lib "x"  →  // @@BUNDLER_LIB@@ "x"
 *   so the C preprocessor ignores the unknown directive.
 *
 * restore_lib_includes  – called AFTER run_preprocessor (and after
 *   strip_comments, so the marker comment survives that pass too).
 *   Converts  // @@BUNDLER_LIB@@ "x"  →  #include_lib "x"
 *
 * Both take ownership of content (free it) and return a new malloc'd string.
 * ---------------------------------------------------------------------- */

char *protect_lib_includes(char *content);
char *restore_lib_includes(char *content);

#endif /* PREPROCESSOR_H */