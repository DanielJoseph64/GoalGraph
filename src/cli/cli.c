#include "cli_internal.h"

#include "graph.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REPL_MAX_LINE ((size_t)1 << 20) /* 1 MiB */
#define REPL_INITIAL_CAPACITY 128
#define PROMPT "goalgraph> "

/* ------------------------------------------------------------- session -- */

cli_session *cli_session_create(FILE *out, FILE *err)
{
    if (out == NULL || err == NULL) {
        return NULL;
    }
    cli_session *s = calloc(1, sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    s->g = graph_create();
    if (s->g == NULL) {
        free(s);
        return NULL;
    }
    s->out = out;
    s->err = err;
    return s;
}

void cli_session_free(cli_session *s)
{
    if (s == NULL) {
        return;
    }
    graph_free(s->g);
    free(s->path);
    free(s->last_path);
    free(s);
}

bool cli_session_modified(const cli_session *s)
{
    return s != NULL && s->modified;
}

void cli_clear_last_path(cli_session *s)
{
    free(s->last_path);
    s->last_path = NULL;
    s->last_path_length = 0;
}

void cli_mark_modified(cli_session *s)
{
    s->modified = true;
    cli_clear_last_path(s); /* node indices may have changed */
}

void cli_replace_graph(cli_session *s, graph *g)
{
    graph_free(s->g);
    s->g = g;
    s->modified = false;
    cli_clear_last_path(s);
}

bool cli_equal_ci(const char *a, const char *b)
{
    for (;; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        if (*a == '\0') {
            return true;
        }
    }
}

/* -------------------------------------------------------------- output -- */

void cli_print(cli_session *s, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vfprintf(s->out, format, args);
    va_end(args);
    if (written < 0) {
        s->output_failed = true;
    }
}

cli_result cli_error(cli_session *s, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const bool ok = fputs("error: ", s->err) >= 0 && vfprintf(s->err, format, args) >= 0 &&
                    fputc('\n', s->err) != EOF;
    va_end(args);
    if (!ok) {
        s->output_failed = true;
    }
    return CLI_ERROR;
}

void cli_note(cli_session *s, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vfprintf(s->err, format, args);
    va_end(args);
    if (written < 0) {
        s->output_failed = true;
    }
}

/* ------------------------------------------------------------ tokenizer -- */

typedef struct token_list {
    char *buffer; /* decoded arguments, NUL-separated */
    char **argv;  /* argc entries pointing into buffer, then NULL */
    int argc;
} token_list;

static void tokens_free(token_list *t)
{
    free(t->argv);
    free(t->buffer);
    *t = (token_list){0};
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/*
 * Copies one argument from *src to *dst, removing quotes. Returns false on an
 * unterminated quote. The output is never longer than the input consumed.
 */
static bool copy_argument(const char **src, char **dst)
{
    const char *p = *src;
    char *out = *dst;
    while (*p != '\0' && !is_space(*p)) {
        if (*p == '"' || *p == '\'') {
            const char quote = *p++;
            while (*p != quote) {
                if (*p == '\0') {
                    return false;
                }
                if (quote == '"' && *p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                    p++;
                }
                *out++ = *p++;
            }
            p++; /* closing quote */
        } else {
            *out++ = *p++;
        }
    }
    *out++ = '\0';
    *src = p;
    *dst = out;
    return true;
}

static cli_result tokenize(cli_session *s, const char *line, token_list *out)
{
    *out = (token_list){0};
    const size_t len = strlen(line);
    /* Each argument consumes at least one input byte plus a separator. */
    const size_t max_tokens = len / 2 + 2;
    if (max_tokens > SIZE_MAX / sizeof(char *)) {
        return cli_error(s, "line too long");
    }
    out->buffer = malloc(len + 1);
    out->argv = malloc(max_tokens * sizeof(char *));
    if (out->buffer == NULL || out->argv == NULL) {
        tokens_free(out);
        return cli_error(s, "out of memory");
    }

    const char *src = line;
    char *dst = out->buffer;
    for (;;) {
        while (is_space(*src)) {
            src++;
        }
        if (*src == '\0' || *src == '#') {
            break;
        }
        if (out->argc == INT_MAX) {
            tokens_free(out);
            return cli_error(s, "too many arguments");
        }
        out->argv[out->argc++] = dst;
        if (!copy_argument(&src, &dst)) {
            tokens_free(out);
            return cli_error(s, "unterminated quote");
        }
    }
    out->argv[out->argc] = NULL;
    return CLI_OK;
}

/* ------------------------------------------------------------- dispatch -- */

const cli_command *cli_find_command(const char *name)
{
    for (const cli_command *c = cli_commands; c->name != NULL; c++) {
        if (cli_equal_ci(name, c->name)) {
            return c;
        }
    }
    return NULL;
}

static bool is_quit_command(const cli_command *c)
{
    return strcmp(c->name, "quit") == 0 || strcmp(c->name, "exit") == 0;
}

cli_result cli_run_args(cli_session *s, int argc, char *const argv[])
{
    if (s == NULL) {
        return CLI_ERROR;
    }
    if (argc < 1 || argv == NULL || argv[0] == NULL) {
        return cli_error(s, "no command given");
    }

    const cli_command *cmd = cli_find_command(argv[0]);
    if (cmd == NULL) {
        return cli_error(s, "unknown command '%s' (type 'help' for a list)", argv[0]);
    }
    const int nargs = argc - 1;
    if (nargs < cmd->min_args || (cmd->max_args >= 0 && nargs > cmd->max_args)) {
        return cli_error(s, "usage: %s %s", cmd->name, cmd->usage);
    }
    if (!is_quit_command(cmd)) {
        s->quit_warned = false;
    }

    s->output_failed = false;
    cli_result result = cmd->handler(s, argc, argv);
    if (fflush(s->out) != 0) {
        s->output_failed = true;
    }
    if (s->output_failed) {
        s->output_failed = false;
        result = cli_error(s, "writing output failed");
    }
    return result;
}

cli_result cli_run_line(cli_session *s, const char *line)
{
    if (s == NULL) {
        return CLI_ERROR;
    }
    if (line == NULL) {
        return cli_error(s, "no command given");
    }
    token_list tokens = {0};
    if (tokenize(s, line, &tokens) != CLI_OK) {
        return CLI_ERROR;
    }
    const cli_result result = tokens.argc == 0 ? CLI_OK
                                               : cli_run_args(s, tokens.argc, tokens.argv);
    tokens_free(&tokens);
    return result;
}

/* ----------------------------------------------------------------- REPL -- */

typedef struct line_buffer {
    char *data;
    size_t capacity;
} line_buffer;

static bool reserve_line(line_buffer *b, size_t needed)
{
    if (needed <= b->capacity) {
        return true;
    }
    size_t capacity = b->capacity > 0 ? b->capacity : REPL_INITIAL_CAPACITY;
    while (capacity < needed) {
        capacity *= 2; /* bounded by REPL_MAX_LINE + 1, so no overflow */
    }
    char *grown = realloc(b->data, capacity);
    if (grown == NULL) {
        return false;
    }
    b->data = grown;
    b->capacity = capacity;
    return true;
}

typedef enum read_result { READ_LINE, READ_EOF, READ_TOO_LONG, READ_FAILED } read_result;

/* Reads one line (without the newline) into b. Over-long lines are skipped. */
static read_result read_line(FILE *in, line_buffer *b)
{
    size_t len = 0;
    bool any = false;
    bool too_long = false;
    for (int c = fgetc(in); c != EOF; c = fgetc(in)) {
        any = true;
        if (c == '\n') {
            break;
        }
        if (too_long || len >= REPL_MAX_LINE) {
            too_long = true;
            continue;
        }
        if (!reserve_line(b, len + 2)) {
            return READ_FAILED;
        }
        b->data[len++] = (char)c;
    }
    if (ferror(in)) {
        return READ_FAILED;
    }
    if (!any) {
        return READ_EOF;
    }
    if (too_long) {
        return READ_TOO_LONG;
    }
    if (!reserve_line(b, len + 1)) {
        return READ_FAILED;
    }
    b->data[len] = '\0';
    return READ_LINE;
}

static void prompt(cli_session *s)
{
    cli_print(s, "%s", PROMPT);
    if (fflush(s->out) != 0) {
        s->output_failed = true;
    }
}

cli_result cli_repl(cli_session *s, FILE *in, bool interactive)
{
    if (s == NULL) {
        return CLI_ERROR;
    }
    if (in == NULL) {
        return cli_error(s, "no input stream");
    }
    if (interactive) {
        cli_print(s, "GoalGraph interactive shell. Type 'help' for commands, 'quit' to exit.\n");
    }

    line_buffer buffer = {0};
    bool failed = false;
    bool quit = false;
    while (!quit) {
        if (interactive) {
            prompt(s);
        }
        const read_result r = read_line(in, &buffer);
        if (r == READ_EOF) {
            break;
        }
        if (r == READ_FAILED) {
            failed = true;
            cli_error(s, "reading input failed");
            break;
        }
        const cli_result result = r == READ_TOO_LONG
                                      ? cli_error(s, "line longer than %zu bytes", REPL_MAX_LINE)
                                      : cli_run_line(s, buffer.data);
        failed |= result == CLI_ERROR;
        quit = result == CLI_QUIT;
    }
    free(buffer.data);

    if (!quit) {
        if (interactive) {
            cli_print(s, "\n");
        }
        if (s->modified) {
            cli_note(s, "warning: exiting with unsaved changes\n");
        }
    }
    return failed ? CLI_ERROR : CLI_OK;
}
