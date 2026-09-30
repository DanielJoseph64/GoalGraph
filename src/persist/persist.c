#include "persist.h"

#include "graph.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HEADER_KEYWORD "goalgraph"
#define TEMP_SUFFIX ".tmp"
#define LINE_INITIAL_CAPACITY 256

/* --------------------------------------------------------------- errors -- */

static void clear_error(persist_error *err)
{
    if (err != NULL) {
        err->status = PERSIST_OK;
        err->line = 0;
        err->message[0] = '\0';
    }
}

/* Records an error in err (if non-NULL) and returns status. */
static persist_status fail(persist_error *err, persist_status status, size_t line,
                           const char *format, ...)
{
    if (err != NULL) {
        err->status = status;
        err->line = line;
        va_list args;
        va_start(args, format);
        const int written = vsnprintf(err->message, sizeof err->message, format, args);
        va_end(args);
        if (written < 0) {
            err->message[0] = '\0';
        }
    }
    return status;
}

const char *persist_status_str(persist_status status)
{
    switch (status) {
    case PERSIST_OK:
        return "ok";
    case PERSIST_ERR_NULL_ARG:
        return "null argument";
    case PERSIST_ERR_IO:
        return "i/o error";
    case PERSIST_ERR_NO_MEMORY:
        return "out of memory";
    case PERSIST_ERR_FORMAT:
        return "malformed input";
    case PERSIST_ERR_VERSION:
        return "unsupported format version";
    }
    return "unknown";
}

/* -------------------------------------------------------------- writing -- */

static bool write_escaped_byte(FILE *stream, unsigned char c)
{
    switch (c) {
    case '"':
        return fputs("\\\"", stream) >= 0;
    case '\\':
        return fputs("\\\\", stream) >= 0;
    case '\n':
        return fputs("\\n", stream) >= 0;
    case '\r':
        return fputs("\\r", stream) >= 0;
    case '\t':
        return fputs("\\t", stream) >= 0;
    default:
        break;
    }
    if (c < 0x20 || c == 0x7f) {
        return fprintf(stream, "\\x%02X", (unsigned)c) >= 0;
    }
    return fputc(c, stream) != EOF;
}

static bool write_string(FILE *stream, const char *s)
{
    if (fputc('"', stream) == EOF) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if (!write_escaped_byte(stream, *p)) {
            return false;
        }
    }
    return fputc('"', stream) != EOF;
}

static bool write_node(FILE *stream, const graph_node_info *n)
{
    return fputs("node ", stream) >= 0 && write_string(stream, n->id) &&
           fputc(' ', stream) != EOF && write_string(stream, n->category) &&
           fprintf(stream, " %s %d %d %lld %lld\n", node_status_str(n->status), n->effort,
                   n->enjoyment, (long long)n->created_at, (long long)n->updated_at) >= 0;
}

/* %.9g prints enough significant digits to round-trip any float exactly. */
static bool write_edge(FILE *stream, const char *source_id, const char *target_id,
                       const graph_edge *e)
{
    const edge_weights *w = &e->weights;
    return fputs("edge ", stream) >= 0 && write_string(stream, source_id) &&
           fputc(' ', stream) != EOF && write_string(stream, target_id) &&
           fprintf(stream, " %s %.9g %.9g %.9g %.9g %.9g\n", edge_type_str(e->type),
                   (double)w->effort, (double)w->enjoyment, (double)w->achievability,
                   (double)w->similarity, (double)w->time_cost) >= 0;
}

static persist_status write_nodes(const graph *g, FILE *stream, persist_error *err)
{
    const size_t n = graph_node_count(g);
    for (size_t i = 0; i < n; i++) {
        graph_node_info info = {0};
        if (graph_get_node(g, i, &info) != GRAPH_OK) {
            return fail(err, PERSIST_ERR_IO, 0, "cannot read node %zu", i);
        }
        if (!write_node(stream, &info)) {
            return fail(err, PERSIST_ERR_IO, 0, "write failed");
        }
    }
    return PERSIST_OK;
}

static persist_status write_edges(const graph *g, FILE *stream, persist_error *err)
{
    const size_t n = graph_node_count(g);
    for (size_t u = 0; u < n; u++) {
        const graph_edge *edges = NULL;
        size_t count = 0;
        graph_node_info source = {0};
        if (graph_out_edges(g, u, &edges, &count) != GRAPH_OK ||
            graph_get_node(g, u, &source) != GRAPH_OK) {
            return fail(err, PERSIST_ERR_IO, 0, "cannot read edges of node %zu", u);
        }
        for (size_t i = 0; i < count; i++) {
            graph_node_info target = {0};
            if (graph_get_node(g, edges[i].target, &target) != GRAPH_OK) {
                return fail(err, PERSIST_ERR_IO, 0, "cannot read node %zu", edges[i].target);
            }
            if (!write_edge(stream, source.id, target.id, &edges[i])) {
                return fail(err, PERSIST_ERR_IO, 0, "write failed");
            }
        }
    }
    return PERSIST_OK;
}

persist_status persist_write(const graph *g, FILE *stream, persist_error *err)
{
    clear_error(err);
    if (g == NULL || stream == NULL) {
        return fail(err, PERSIST_ERR_NULL_ARG, 0, "graph and stream are required");
    }

    if (fprintf(stream, "%s %d\n", HEADER_KEYWORD, PERSIST_FORMAT_VERSION) < 0) {
        return fail(err, PERSIST_ERR_IO, 0, "write failed");
    }
    persist_status status = write_nodes(g, stream, err);
    if (status == PERSIST_OK) {
        status = write_edges(g, stream, err);
    }
    if (status != PERSIST_OK) {
        return status;
    }
    if (fflush(stream) != 0 || ferror(stream)) {
        return fail(err, PERSIST_ERR_IO, 0, "write failed");
    }
    return PERSIST_OK;
}

/* ---------------------------------------------------------- line reading -- */

typedef struct line_reader {
    FILE *stream;
    char *buf;
    size_t capacity;
    size_t line_no; /* number of the line most recently read */
} line_reader;

/* Ensures room for at least needed bytes. needed <= PERSIST_MAX_LINE + 1. */
static bool reserve_line(line_reader *r, size_t needed)
{
    if (needed <= r->capacity) {
        return true;
    }
    size_t new_capacity = r->capacity > 0 ? r->capacity : LINE_INITIAL_CAPACITY;
    while (new_capacity < needed) {
        new_capacity *= 2; /* cannot overflow: needed is bounded by PERSIST_MAX_LINE + 1 */
    }
    char *const grown = realloc(r->buf, new_capacity);
    if (grown == NULL) {
        return false;
    }
    r->buf = grown;
    r->capacity = new_capacity;
    return true;
}

/*
 * Reads the next line (without its LF or CRLF terminator) into r->buf.
 * Sets *out_line to the NUL-terminated line, or to NULL at end of input.
 */
static persist_status read_line(line_reader *r, char **out_line, persist_error *err)
{
    *out_line = NULL;
    size_t len = 0;
    bool any = false;
    const size_t line_no = r->line_no + 1;

    for (int c = fgetc(r->stream); c != EOF; c = fgetc(r->stream)) {
        any = true;
        if (c == '\n') {
            break;
        }
        if (c == '\0') {
            return fail(err, PERSIST_ERR_FORMAT, line_no, "NUL byte in input");
        }
        if (len >= PERSIST_MAX_LINE) {
            return fail(err, PERSIST_ERR_FORMAT, line_no, "line longer than %zu bytes",
                        PERSIST_MAX_LINE);
        }
        if (!reserve_line(r, len + 2)) {
            return fail(err, PERSIST_ERR_NO_MEMORY, line_no, "out of memory reading line");
        }
        r->buf[len++] = (char)c;
    }
    if (ferror(r->stream)) {
        return fail(err, PERSIST_ERR_IO, line_no, "read failed");
    }
    if (!any) {
        return PERSIST_OK; /* end of input */
    }
    if (!reserve_line(r, len + 1)) {
        return fail(err, PERSIST_ERR_NO_MEMORY, line_no, "out of memory reading line");
    }
    if (len > 0 && r->buf[len - 1] == '\r') {
        len--;
    }
    r->buf[len] = '\0';
    r->line_no = line_no;
    *out_line = r->buf;
    return PERSIST_OK;
}

/* ------------------------------------------------------------ tokenizing -- */

/* Parsing context for one line. Tokens are terminated in place. */
typedef struct cursor {
    char *pos;
    size_t line;
    persist_error *err;
} cursor;

static bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static void skip_blanks(cursor *c)
{
    while (is_blank(*c->pos)) {
        c->pos++;
    }
}

/* Returns the next blank-delimited word, or NULL at end of line. */
static char *next_word(cursor *c)
{
    skip_blanks(c);
    if (*c->pos == '\0') {
        return NULL;
    }
    char *const start = c->pos;
    while (*c->pos != '\0' && !is_blank(*c->pos)) {
        c->pos++;
    }
    if (*c->pos != '\0') {
        *c->pos = '\0';
        c->pos++;
    }
    return start;
}

static persist_status require_word(cursor *c, const char *field, char **out)
{
    *out = next_word(c);
    if (*out == NULL) {
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "missing %s", field);
    }
    return PERSIST_OK;
}

static persist_status expect_end(cursor *c)
{
    skip_blanks(c);
    if (*c->pos != '\0') {
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "unexpected extra field");
    }
    return PERSIST_OK;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/*
 * Decodes one escape sequence starting just after a backslash at *src.
 * Writes the byte to *out and advances *src past the sequence.
 */
static persist_status decode_escape(cursor *c, const char **src, char *out)
{
    const char e = **src;
    (*src)++;
    switch (e) {
    case '"':
    case '\\':
        *out = e;
        return PERSIST_OK;
    case 'n':
        *out = '\n';
        return PERSIST_OK;
    case 'r':
        *out = '\r';
        return PERSIST_OK;
    case 't':
        *out = '\t';
        return PERSIST_OK;
    case 'x': {
        const int hi = hex_value((*src)[0]);
        const int lo = hi >= 0 ? hex_value((*src)[1]) : -1;
        if (lo < 0) {
            return fail(c->err, PERSIST_ERR_FORMAT, c->line, "\\x needs two hex digits");
        }
        if (hi == 0 && lo == 0) {
            return fail(c->err, PERSIST_ERR_FORMAT, c->line, "\\x00 is not allowed");
        }
        *src += 2;
        *out = (char)(unsigned char)(hi * 16 + lo);
        return PERSIST_OK;
    }
    default:
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "invalid escape sequence");
    }
}

/* Parses a double-quoted string, decoding it in place. */
static persist_status parse_string(cursor *c, const char *field, char **out)
{
    skip_blanks(c);
    if (*c->pos == '\0') {
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "missing %s", field);
    }
    if (*c->pos != '"') {
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "%s must be a quoted string", field);
    }

    char *const start = c->pos;
    char *dst = start; /* decoded output never outruns the input */
    const char *src = start + 1;
    while (*src != '"') {
        if (*src == '\0') {
            return fail(c->err, PERSIST_ERR_FORMAT, c->line, "unterminated %s", field);
        }
        if (*src == '\\') {
            src++;
            const persist_status status = decode_escape(c, &src, dst);
            if (status != PERSIST_OK) {
                return status;
            }
            dst++;
        } else {
            *dst++ = *src++;
        }
    }
    src++; /* closing quote */
    if (*src != '\0' && !is_blank(*src)) {
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "missing space after %s", field);
    }
    *dst = '\0';
    c->pos = start + (src - start);
    *out = start;
    return PERSIST_OK;
}

/* ------------------------------------------------------ value parsing -- */

static bool parse_integer(const char *word, long long min, long long max, long long *out)
{
    char *end = NULL;
    errno = 0;
    const long long value = strtoll(word, &end, 10);
    if (end == word || *end != '\0' || errno == ERANGE || value < min || value > max) {
        return false;
    }
    *out = value;
    return true;
}

static persist_status parse_int_field(cursor *c, const char *field, int *out)
{
    char *word = NULL;
    persist_status status = require_word(c, field, &word);
    if (status != PERSIST_OK) {
        return status;
    }
    long long value = 0;
    if (!parse_integer(word, GRAPH_SCORE_MIN, GRAPH_SCORE_MAX, &value)) {
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "%s must be an integer in %d..%d",
                    field, GRAPH_SCORE_MIN, GRAPH_SCORE_MAX);
    }
    *out = (int)value;
    return PERSIST_OK;
}

static persist_status parse_time_field(cursor *c, const char *field, time_t *out)
{
    char *word = NULL;
    persist_status status = require_word(c, field, &word);
    if (status != PERSIST_OK) {
        return status;
    }
    long long value = 0;
    if (!parse_integer(word, LLONG_MIN, LLONG_MAX, &value) ||
        (long long)(time_t)value != value) {
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "%s must be an integer timestamp",
                    field);
    }
    *out = (time_t)value;
    return PERSIST_OK;
}

/* Rejects non-numbers and non-finite values; subnormal results are accepted. */
static persist_status parse_weight_field(cursor *c, const char *field, float *out)
{
    char *word = NULL;
    persist_status status = require_word(c, field, &word);
    if (status != PERSIST_OK) {
        return status;
    }
    char *end = NULL;
    const float value = strtof(word, &end);
    if (end == word || *end != '\0' || !isfinite(value)) {
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "%s must be a finite number", field);
    }
    *out = value;
    return PERSIST_OK;
}

static persist_status parse_node_status(cursor *c, node_status *out)
{
    char *word = NULL;
    persist_status status = require_word(c, "status", &word);
    if (status != PERSIST_OK) {
        return status;
    }
    for (int s = 0; s < NODE_STATUS_COUNT; s++) {
        if (strcmp(word, node_status_str((node_status)s)) == 0) {
            *out = (node_status)s;
            return PERSIST_OK;
        }
    }
    return fail(c->err, PERSIST_ERR_FORMAT, c->line, "unknown status \"%.32s\"", word);
}

static persist_status parse_edge_type(cursor *c, edge_type *out)
{
    char *word = NULL;
    persist_status status = require_word(c, "edge type", &word);
    if (status != PERSIST_OK) {
        return status;
    }
    for (int t = 0; t < EDGE_TYPE_COUNT; t++) {
        if (strcmp(word, edge_type_str((edge_type)t)) == 0) {
            *out = (edge_type)t;
            return PERSIST_OK;
        }
    }
    return fail(c->err, PERSIST_ERR_FORMAT, c->line, "unknown edge type \"%.32s\"", word);
}

/* ------------------------------------------------------------- records -- */

static persist_status parse_header(cursor *c, const char *keyword)
{
    if (strcmp(keyword, HEADER_KEYWORD) != 0) {
        return fail(c->err, PERSIST_ERR_FORMAT, c->line,
                    "expected \"%s <version>\" header", HEADER_KEYWORD);
    }
    char *word = NULL;
    persist_status status = require_word(c, "format version", &word);
    if (status != PERSIST_OK) {
        return status;
    }
    long long version = 0;
    if (!parse_integer(word, 0, INT_MAX, &version)) {
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "format version must be an integer");
    }
    if (version != PERSIST_FORMAT_VERSION) {
        return fail(c->err, PERSIST_ERR_VERSION, c->line,
                    "format version %lld is not supported (expected %d)", version,
                    PERSIST_FORMAT_VERSION);
    }
    return expect_end(c);
}

static persist_status parse_node_fields(cursor *c, graph_node_spec *spec)
{
    char *id = NULL;
    char *category = NULL;
    persist_status status = parse_string(c, "node id", &id);
    if (status == PERSIST_OK) {
        status = parse_string(c, "category", &category);
    }
    if (status == PERSIST_OK) {
        status = parse_node_status(c, &spec->status);
    }
    if (status == PERSIST_OK) {
        status = parse_int_field(c, "effort", &spec->effort);
    }
    if (status == PERSIST_OK) {
        status = parse_int_field(c, "enjoyment", &spec->enjoyment);
    }
    if (status == PERSIST_OK) {
        status = parse_time_field(c, "created_at", &spec->created_at);
    }
    if (status == PERSIST_OK) {
        status = parse_time_field(c, "updated_at", &spec->updated_at);
    }
    if (status == PERSIST_OK) {
        status = expect_end(c);
    }
    spec->id = id;
    spec->category = category;
    return status;
}

static persist_status parse_node(cursor *c, graph *g)
{
    graph_node_spec spec = {0};
    const persist_status status = parse_node_fields(c, &spec);
    if (status != PERSIST_OK) {
        return status;
    }
    if (spec.id[0] == '\0') {
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "node id must not be empty");
    }

    switch (graph_add_node(g, &spec, NULL)) {
    case GRAPH_OK:
        return PERSIST_OK;
    case GRAPH_ERR_NO_MEMORY:
        return fail(c->err, PERSIST_ERR_NO_MEMORY, c->line, "out of memory adding node");
    case GRAPH_ERR_DUPLICATE_ID:
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "duplicate node id \"%.64s\"",
                    spec.id);
    default:
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "invalid node");
    }
}

static persist_status parse_endpoint(cursor *c, const graph *g, const char *field, size_t *out)
{
    char *id = NULL;
    const persist_status status = parse_string(c, field, &id);
    if (status != PERSIST_OK) {
        return status;
    }
    if (graph_find_node(g, id, out) != GRAPH_OK) {
        return fail(c->err, PERSIST_ERR_FORMAT, c->line, "unknown node id \"%.64s\"", id);
    }
    return PERSIST_OK;
}

static persist_status parse_edge(cursor *c, graph *g)
{
    size_t source = 0;
    size_t target = 0;
    edge_type type = EDGE_PREREQ;
    edge_weights w = {0};

    persist_status status = parse_endpoint(c, g, "source id", &source);
    if (status == PERSIST_OK) {
        status = parse_endpoint(c, g, "target id", &target);
    }
    if (status == PERSIST_OK) {
        status = parse_edge_type(c, &type);
    }
    if (status == PERSIST_OK) {
        status = parse_weight_field(c, "effort weight", &w.effort);
    }
    if (status == PERSIST_OK) {
        status = parse_weight_field(c, "enjoyment weight", &w.enjoyment);
    }
    if (status == PERSIST_OK) {
        status = parse_weight_field(c, "achievability weight", &w.achievability);
    }
    if (status == PERSIST_OK) {
        status = parse_weight_field(c, "similarity weight", &w.similarity);
    }
    if (status == PERSIST_OK) {
        status = parse_weight_field(c, "time_cost weight", &w.time_cost);
    }
    if (status == PERSIST_OK) {
        status = expect_end(c);
    }
    if (status != PERSIST_OK) {
        return status;
    }

    switch (graph_add_edge(g, source, target, type, &w)) {
    case GRAPH_OK:
        return PERSIST_OK;
    case GRAPH_ERR_NO_MEMORY:
        return fail(c->err, PERSIST_ERR_NO_MEMORY, c->line, "out of memory adding edge");
    default:
        return fail(c->err, PERSIST_ERR_FORMAT, c->line,
                    source == target ? "edge from a node to itself" : "invalid edge");
    }
}

/* Parses one non-blank, non-comment line. */
static persist_status parse_record(cursor *c, graph *g, bool *have_header)
{
    const char *const keyword = next_word(c);
    if (!*have_header) {
        const persist_status status = parse_header(c, keyword);
        *have_header = status == PERSIST_OK;
        return status;
    }
    if (strcmp(keyword, "node") == 0) {
        return parse_node(c, g);
    }
    if (strcmp(keyword, "edge") == 0) {
        return parse_edge(c, g);
    }
    return fail(c->err, PERSIST_ERR_FORMAT, c->line, "unknown record type \"%.32s\"", keyword);
}

persist_status persist_read(FILE *stream, graph **out, persist_error *err)
{
    clear_error(err);
    if (out != NULL) {
        *out = NULL;
    }
    if (stream == NULL || out == NULL) {
        return fail(err, PERSIST_ERR_NULL_ARG, 0, "stream and out are required");
    }

    graph *g = graph_create();
    if (g == NULL) {
        return fail(err, PERSIST_ERR_NO_MEMORY, 0, "out of memory creating graph");
    }
    line_reader reader = {.stream = stream};
    bool have_header = false;
    persist_status status = PERSIST_OK;

    for (;;) {
        char *line = NULL;
        status = read_line(&reader, &line, err);
        if (status != PERSIST_OK || line == NULL) {
            break;
        }
        cursor c = {.pos = line, .line = reader.line_no, .err = err};
        skip_blanks(&c);
        if (*c.pos == '\0' || *c.pos == '#') {
            continue;
        }
        status = parse_record(&c, g, &have_header);
        if (status != PERSIST_OK) {
            break;
        }
    }
    if (status == PERSIST_OK && !have_header) {
        status = fail(err, PERSIST_ERR_FORMAT, reader.line_no, "missing \"%s\" header",
                      HEADER_KEYWORD);
    }

    free(reader.buf);
    if (status != PERSIST_OK) {
        graph_free(g);
        return status;
    }
    *out = g;
    return PERSIST_OK;
}

/* ---------------------------------------------------------------- files -- */

/* Returns a new "<path>.tmp" string (caller frees), or NULL on failure. */
static char *temp_path_for(const char *path)
{
    const size_t len = strlen(path);
    const size_t suffix_len = sizeof TEMP_SUFFIX - 1;
    if (len > SIZE_MAX - suffix_len - 1) {
        return NULL;
    }
    char *const tmp = malloc(len + suffix_len + 1);
    if (tmp == NULL) {
        return NULL;
    }
    memcpy(tmp, path, len);
    memcpy(tmp + len, TEMP_SUFFIX, suffix_len + 1);
    return tmp;
}

persist_status persist_save(const graph *g, const char *path, persist_error *err)
{
    clear_error(err);
    if (g == NULL || path == NULL) {
        return fail(err, PERSIST_ERR_NULL_ARG, 0, "graph and path are required");
    }

    char *const tmp_path = temp_path_for(path);
    if (tmp_path == NULL) {
        return fail(err, PERSIST_ERR_NO_MEMORY, 0, "out of memory");
    }

    FILE *stream = fopen(tmp_path, "wb");
    if (stream == NULL) {
        const int saved_errno = errno;
        free(tmp_path);
        return fail(err, PERSIST_ERR_IO, 0, "cannot create temporary file: %s",
                    strerror(saved_errno));
    }

    persist_status status = persist_write(g, stream, err);
    if (fclose(stream) != 0 && status == PERSIST_OK) {
        status = fail(err, PERSIST_ERR_IO, 0, "closing temporary file failed: %s",
                      strerror(errno));
    }
    stream = NULL;
    if (status == PERSIST_OK && rename(tmp_path, path) != 0) {
        status = fail(err, PERSIST_ERR_IO, 0, "cannot replace target file: %s",
                      strerror(errno));
    }
    if (status != PERSIST_OK && remove(tmp_path) != 0) {
        /* The original file is untouched either way; only a stray temp file remains. */
        const size_t used = err != NULL ? strlen(err->message) : 0;
        if (err != NULL && used < sizeof err->message &&
            snprintf(err->message + used, sizeof err->message - used,
                     " (temporary file left behind)") < 0) {
            err->message[used] = '\0';
        }
    }

    free(tmp_path);
    return status;
}

persist_status persist_load(const char *path, graph **out, persist_error *err)
{
    clear_error(err);
    if (out != NULL) {
        *out = NULL;
    }
    if (path == NULL || out == NULL) {
        return fail(err, PERSIST_ERR_NULL_ARG, 0, "path and out are required");
    }

    FILE *const stream = fopen(path, "rb");
    if (stream == NULL) {
        return fail(err, PERSIST_ERR_IO, 0, "cannot open file: %s", strerror(errno));
    }

    graph *g = NULL;
    persist_status status = persist_read(stream, &g, err);
    if (fclose(stream) != 0 && status == PERSIST_OK) {
        status = fail(err, PERSIST_ERR_IO, 0, "closing file failed: %s", strerror(errno));
    }
    if (status != PERSIST_OK) {
        graph_free(g);
        return status;
    }
    *out = g;
    return PERSIST_OK;
}
