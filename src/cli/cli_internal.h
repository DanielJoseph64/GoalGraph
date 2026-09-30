#ifndef GOALGRAPH_CLI_INTERNAL_H
#define GOALGRAPH_CLI_INTERNAL_H

/* Private to the cli module: shared by cli.c and commands.c. */

#include "cli.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct graph graph;

struct cli_session {
    FILE *out;
    FILE *err;
    graph *g;          /* owned; never NULL */
    char *path;        /* owned; file last loaded or saved, or NULL */
    bool modified;     /* unsaved changes exist */
    bool quit_warned;  /* "quit" already warned about unsaved changes */
    bool output_failed;
    size_t *last_path; /* owned; node indices from the last "path" command */
    size_t last_path_length;
};

typedef cli_result (*cli_handler)(cli_session *s, int argc, char *const argv[]);

typedef struct cli_command {
    const char *name;
    const char *usage;   /* arguments, shown after the name */
    const char *summary; /* one-line description */
    int min_args;        /* required positional arguments (excluding the name) */
    int max_args;        /* maximum arguments, or -1 for no limit */
    cli_handler handler;
} cli_command;

/* The command table (commands.c), terminated by an entry with a NULL name. */
extern const cli_command cli_commands[];

/* Finds a command by name (case-insensitive), or returns NULL. */
const cli_command *cli_find_command(const char *name);

/* Writes to the out stream; records failures in s->output_failed. */
void cli_print(cli_session *s, const char *format, ...);

/* Writes "error: <message>\n" to the err stream and returns CLI_ERROR. */
cli_result cli_error(cli_session *s, const char *format, ...);

/* Writes text to the err stream as-is (warnings and error details). */
void cli_note(cli_session *s, const char *format, ...);

/* Marks the graph modified and drops state derived from the old graph. */
void cli_mark_modified(cli_session *s);

/* Drops the remembered "path" result. */
void cli_clear_last_path(cli_session *s);

/* Replaces the session graph (taking ownership) and resets derived state. */
void cli_replace_graph(cli_session *s, graph *g);

/* Case-insensitive ASCII string equality. */
bool cli_equal_ci(const char *a, const char *b);

#endif /* GOALGRAPH_CLI_INTERNAL_H */
