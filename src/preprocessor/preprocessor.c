#include "preprocessor.h"
#include "../fs/fs.h"
#include <ctype.h>

/* -------------------------------------------------------------------------
 * LineMap implementation
 * ---------------------------------------------------------------------- */

LineMap *linemap_create(void) {
    LineMap *m = malloc(sizeof(LineMap));
    m->count = 0; m->capacity = 64;
    m->entries = malloc(m->capacity * sizeof(LineMapEntry));
    return m;
}

void linemap_free(LineMap *map) {
    if (map) { free(map->entries); free(map); }
}

LineMap *parse_line_directives(const char *content) {
    LineMap *map = linemap_create();
    const char *p = content;
    int current_output_line = 1;

    while (*p) {
        const char *line_start = p;
        /* Advance to end of line */
        while (*p && *p != '\n') p++;

        /* Check if this is a gcc line directive: "# <digit>" */
        const char *check = line_start;
        if (check[0] == '#' && check[1] == ' ' && isdigit((unsigned char)check[2])) {
            /* Parse: # <linenum> "<file>" */
            int src_line = 0;
            const char *q = check + 2;
            while (*q >= '0' && *q <= '9') src_line = src_line * 10 + (*q++ - '0');
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '"') {
                q++;
                const char *fname_start = q;
                while (*q && *q != '"') q++;
                size_t fname_len = (size_t)(q - fname_start);
                if (fname_len < MAX_PATH - 1) {
                    if (map->count >= map->capacity) {
                        map->capacity *= 2;
                        map->entries = realloc(map->entries, map->capacity * sizeof(LineMapEntry));
                    }
                    LineMapEntry *e = &map->entries[map->count++];
                    e->preprocessed_line = current_output_line;
                    e->source_line = src_line;
                    strncpy(e->file, fname_start, fname_len);
                    e->file[fname_len] = '\0';
                }
            }
        }

        if (*p == '\n') { current_output_line++; p++; }
    }
    return map;
}

int linemap_lookup(const LineMap *map, int preprocessed_line,
                   char *file_out, int file_out_size) {
    /* Find the last entry with preprocessed_line <= query */
    int best = -1;
    for (int i = 0; i < map->count; i++) {
        if (map->entries[i].preprocessed_line <= preprocessed_line)
            best = i;
        else
            break;
    }
    if (best < 0) return 0;
    const LineMapEntry *e = &map->entries[best];
    int offset = preprocessed_line - e->preprocessed_line;
    strncpy(file_out, e->file, file_out_size - 1);
    file_out[file_out_size - 1] = '\0';
    return e->source_line + offset;
}

char *strip_line_directives(char *content) {
    size_t size = strlen(content);
    char *result = malloc(size + 1);
    char *dst = result;
    const char *p = content;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        /* Skip gcc line directive lines */
        if (line_start[0] == '#' && line_start[1] == ' ' &&
            isdigit((unsigned char)line_start[2])) {
            if (*p == '\n') p++;
            continue;
        }
        size_t len = (size_t)(p - line_start);
        memcpy(dst, line_start, len);
        dst += len;
        if (*p == '\n') { *dst++ = '\n'; p++; }
    }
    *dst = '\0';
    free(content);
    return result;
}

/* -------------------------------------------------------------------------
 * Directory helpers
 * ---------------------------------------------------------------------- */

void ensure_dir(const char *dir) {
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);
#else
    mkdir(dir, 0755);
#endif
}

/* -------------------------------------------------------------------------
 * C preprocessor execution
 * ---------------------------------------------------------------------- */

char *run_preprocessor(const char *content, LineMap **map_out) {
    /* Place temp files in .build_cache next to the source directory (or cwd) */
    char cache_dir[MAX_PATH];
    if (g_input_dir[0] != '\0') {
        char parent[MAX_PATH];
        get_dirname(g_input_dir, parent);
        snprintf(cache_dir, sizeof(cache_dir), "%s/.build_cache", parent);
    } else {
        snprintf(cache_dir, sizeof(cache_dir), ".build_cache");
    }

    ensure_dir(cache_dir);

    char input_path[MAX_PATH], output_path[MAX_PATH], err_path[MAX_PATH];
    snprintf(input_path,  sizeof(input_path),  "%s/_combined_input.c",   cache_dir);
    snprintf(output_path, sizeof(output_path), "%s/_combined_output.c",  cache_dir);
    snprintf(err_path,    sizeof(err_path),    "%s/_preproc_error.txt",  cache_dir);

    /* Write combined input to temp file */
    FILE *f = fopen(input_path, "wb");
    if (!f) {
        fprintf(stderr, "Error: Could not write preprocessor input to '%s'\n",
                input_path);
        exit(1);
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);

    /* Build forward-slash copies of every path that goes into the shell
     * command string.  We do NOT modify input_path / output_path / err_path
     * themselves because fopen() and remove() still need the original form. */
    char cmd_input[MAX_PATH], cmd_output[MAX_PATH], cmd_err[MAX_PATH];
    char cmd_idir[MAX_PATH];
    strncpy(cmd_input,  input_path,  MAX_PATH - 1);  cmd_input[MAX_PATH  - 1] = '\0';
    strncpy(cmd_output, output_path, MAX_PATH - 1);  cmd_output[MAX_PATH - 1] = '\0';
    strncpy(cmd_err,    err_path,    MAX_PATH - 1);  cmd_err[MAX_PATH    - 1] = '\0';
    strncpy(cmd_idir,   g_input_dir, MAX_PATH - 1);  cmd_idir[MAX_PATH   - 1] = '\0';
    for (char *p = cmd_input;  *p; p++) if (*p == '\\') *p = '/';
    for (char *p = cmd_output; *p; p++) if (*p == '\\') *p = '/';
    for (char *p = cmd_err;    *p; p++) if (*p == '\\') *p = '/';
    for (char *p = cmd_idir;   *p; p++) if (*p == '\\') *p = '/';

    /* Build command: gcc -E -x c [-D...] [-I src_dir] input -o output */
    size_t cmd_size = 1024 + (size_t)g_define_count * 256 + MAX_PATH * 3;
    char  *cmd      = (char *)malloc(cmd_size);
    int    pos      = 0;

    pos += snprintf(cmd + pos, cmd_size - (size_t)pos, "gcc -E -C -x c");

    for (int i = 0; i < g_define_count; i++)
        pos += snprintf(cmd + pos, cmd_size - (size_t)pos, " %s", g_defines[i]);

    if (cmd_idir[0] != '\0')
        pos += snprintf(cmd + pos, cmd_size - (size_t)pos,
                        " -I \"%s\"", cmd_idir);

    pos += snprintf(cmd + pos, cmd_size - (size_t)pos,
                    " \"%s\" -o \"%s\" 2>\"%s\"",
                    cmd_input, cmd_output, cmd_err);


    int ret = system(cmd);
    free(cmd);

    if (ret != 0) {
        fprintf(stderr, "\nError: Preprocessor failed.\n");
        size_t err_size;
        char  *err_content = read_file_content(err_path, &err_size);
        if (err_content && err_size > 0) {
            fprintf(stderr, "%s\n", err_content);
            free(err_content);
        }
        fprintf(stderr, "Hint: Ensure gcc is in your PATH, or use "
                        "--no-preprocess to skip this step.\n");
        remove(input_path);
        remove(err_path);
        exit(1);
    }

    size_t  out_size;
    char   *output = read_file_content(output_path, &out_size);
    if (!output) {
        fprintf(stderr, "Error: Could not read preprocessor output '%s'\n",
                output_path);
        remove(input_path);
        remove(err_path);
        exit(1);
    }

    remove(input_path);
    remove(output_path);
    remove(err_path);

    if (map_out) {
        *map_out = parse_line_directives(output);
    }
    output = strip_line_directives(output);
    return output;
}

/* -------------------------------------------------------------------------
 * #include_lib protection helpers
 * ---------------------------------------------------------------------- */

/* Converts  #include_lib "x"  ->  // @@BUNDLER_LIB@@ "x"  (line-for-line) */
char *protect_lib_includes(char *content) {
    static const char directive[] = "#include_lib";
    static const char marker[]    = "// @@BUNDLER_LIB@@";
    static const size_t dir_len   = sizeof(directive) - 1;
    static const size_t mark_len  = sizeof(marker)    - 1;

    /* Count occurrences to size the output buffer */
    int    count = 0;
    char  *pos   = content;
    while ((pos = strstr(pos, directive)) != NULL) { count++; pos += dir_len; }
    if (count == 0) return content;

    /* Each replacement: remove dir_len chars, insert mark_len chars */
    size_t old_len = strlen(content);
    size_t new_len = old_len - (size_t)count * dir_len + (size_t)count * mark_len;
    char  *result  = (char *)malloc(new_len + 1);
    char  *dst     = result;
    const char *src = content;

    while ((pos = strstr(src, directive)) != NULL) {
        size_t before = (size_t)(pos - src);
        memcpy(dst, src, before);
        dst += before;
        memcpy(dst, marker, mark_len);
        dst += mark_len;
        src  = pos + dir_len;
    }
    strcpy(dst, src);
    free(content);
    return result;
}

/* Converts  // @@BUNDLER_LIB@@ "x"  ->  #include_lib "x"  (line-for-line) */
char *restore_lib_includes(char *content) {
    static const char marker[]    = "// @@BUNDLER_LIB@@";
    static const char directive[] = "#include_lib";
    static const size_t mark_len  = sizeof(marker)    - 1;
    static const size_t dir_len   = sizeof(directive)  - 1;

    int    count = 0;
    char  *pos   = content;
    while ((pos = strstr(pos, marker)) != NULL) { count++; pos += mark_len; }
    if (count == 0) return content;

    size_t old_len = strlen(content);
    size_t new_len = old_len - (size_t)count * mark_len + (size_t)count * dir_len;
    char  *result  = (char *)malloc(new_len + 1);
    char  *dst     = result;
    const char *src = content;

    while ((pos = strstr(src, marker)) != NULL) {
        size_t before = (size_t)(pos - src);
        memcpy(dst, src, before);
        dst += before;
        memcpy(dst, directive, dir_len);
        dst += dir_len;
        src  = pos + mark_len;
    }
    strcpy(dst, src);
    free(content);
    return result;
}

/* -------------------------------------------------------------------------
 * Pragma-marker conversion
 * ---------------------------------------------------------------------- */

char *convert_pragma_markers(char *content) {
    static const char   prefix[]    = "#pragma BUNDLER_FILE ";
    static const size_t prefix_len  = sizeof(prefix) - 1;

    size_t  old_len = strlen(content);
    char   *result  = (char *)malloc(old_len + 1);
    char   *dst     = result;
    const char *p   = content;

    while (*p) {
        const char *line_start = p;
        const char *line_end   = p;
        while (*line_end && *line_end != '\n') line_end++;

        /* Skip leading whitespace when checking the prefix */
        const char *check = line_start;
        while (check < line_end && (*check == ' ' || *check == '\t')) check++;

        if ((size_t)(line_end - check) > prefix_len &&
            strncmp(check, prefix, prefix_len) == 0) {
            const char *path_start = check + prefix_len;
            size_t      path_len   = (size_t)(line_end - path_start);

            /* Trim trailing whitespace / CR from path */
            while (path_len > 0 &&
                   (path_start[path_len - 1] == ' '  ||
                    path_start[path_len - 1] == '\t' ||
                    path_start[path_len - 1] == '\r'))
                path_len--;

            dst += sprintf(dst, "// === ");
            memcpy(dst, path_start, path_len);
            dst += path_len;
            dst += sprintf(dst, " ===");
        } else {
            size_t line_len = (size_t)(line_end - line_start);
            memcpy(dst, line_start, line_len);
            dst += line_len;
        }

        if (*line_end == '\n') {
            *dst++ = '\n';
            p = line_end + 1;
        } else {
            p = line_end;
        }
    }

    *dst = '\0';
    free(content);
    return result;
}
