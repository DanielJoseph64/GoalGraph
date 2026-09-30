#ifndef GOALGRAPH_CLI_H
#define GOALGRAPH_CLI_H

/*
 * Command interpreter behind the goalgraph executable.
 *
 * A session owns one goal graph (initially empty), the path it was last
 * loaded from or saved to, and a "modified" flag. Commands are plain words
 * followed by positional arguments and key=value options, for example:
 *
 *   add "learn rust" category=side-project effort=6 enjoyment=9
 *   link cs101 cs102 type=prereq time=15
 *   path cs101 cs491 weight=time astar
 *
 * Arguments may be double-quoted (with \" and \\ escapes) or single-quoted
 * (taken literally). A '#' at the start of an argument begins a comment.
 * Run the "help" command for the full list.
 *
 * Normal output goes to the session's out stream. Error messages, prefixed
 * with "error: ", go to its err stream.
 */

#include <stdbool.h>
#include <stdio.h>

typedef struct cli_session cli_session;

typedef enum cli_result {
    CLI_OK = 0, /* the command succeeded (or the line was blank) */
    CLI_ERROR,  /* the command failed; a message was written to err */
    CLI_QUIT    /* the user asked to quit */
} cli_result;

/*
 * Creates a session with an empty graph. out and err are borrowed and must
 * stay open for the session's lifetime. Returns NULL if either stream is
 * NULL or allocation fails. Free with cli_session_free().
 */
cli_session *cli_session_create(FILE *out, FILE *err);

/* Frees the session and its graph. Does nothing if s is NULL. */
void cli_session_free(cli_session *s);

/* True if the graph has changed since it was created, loaded or saved. */
bool cli_session_modified(const cli_session *s);

/* Runs one already-split command. argv[0] is the command name; argc >= 1. */
cli_result cli_run_args(cli_session *s, int argc, char *const argv[]);

/* Splits line into arguments and runs it. Blank and comment lines return CLI_OK. */
cli_result cli_run_line(cli_session *s, const char *line);

/*
 * Reads and runs commands from in until EOF or a quit command. When
 * interactive is true, prints a banner and a prompt before each line.
 * Returns CLI_ERROR if any command failed (or reading failed), else CLI_OK.
 */
cli_result cli_repl(cli_session *s, FILE *in, bool interactive);

#endif /* GOALGRAPH_CLI_H */
