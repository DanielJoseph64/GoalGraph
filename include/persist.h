#ifndef GOALGRAPH_PERSIST_H
#define GOALGRAPH_PERSIST_H

/*
 * Saving and loading goal graphs as UTF-8 text.
 *
 * File format (version 1), one record per line:
 *
 *   goalgraph 1
 *   node <id> <category> <status> <effort> <enjoyment> <created_at> <updated_at>
 *   edge <source-id> <target-id> <type> <effort> <enjoyment> <achievability> <similarity> <time_cost>
 *
 * - The "goalgraph 1" header must be the first record.
 * - Blank lines and lines whose first non-blank character is '#' are ignored.
 * - Fields are separated by spaces or tabs.
 * - <id> and <category> are double-quoted strings. Inside the quotes,
 *   \" \\ \n \r \t and \xHH (two hex digits) are escapes; other bytes are
 *   literal. IDs must be non-empty; categories may be "".
 * - <status> is a node_status_str() name (not_started, in_progress,
 *   completed, abandoned). <type> is an edge_type_str() name (PREREQ,
 *   SIMILAR, ENABLES, CONFLICTS, CUSTOM).
 * - <effort> and <enjoyment> on a node are integers in
 *   GRAPH_SCORE_MIN .. GRAPH_SCORE_MAX. Timestamps are integer seconds since
 *   the epoch. As with graph_add_node(), 0 means "now".
 * - Edge weights are finite decimal numbers. They are written with enough
 *   digits to round-trip every float exactly.
 * - A node must be declared before any edge that refers to it.
 * - Lines may end in LF or CRLF. A line may be at most PERSIST_MAX_LINE bytes
 *   long and must not contain NUL bytes.
 *
 * Saving writes nodes in index order and then edges grouped by source index,
 * so loading a saved graph reproduces the same node indices and the same
 * out-edge order.
 *
 * Error reporting: every function returns a persist_status. If err is
 * non-NULL, it is filled in on every call: on success err->status is
 * PERSIST_OK, err->line is 0 and err->message is empty. err is optional
 * everywhere.
 */

#include <stddef.h>
#include <stdio.h>

typedef struct graph graph;

#define PERSIST_FORMAT_VERSION 1
#define PERSIST_MAX_LINE ((size_t)1 << 20) /* 1 MiB */
#define PERSIST_ERROR_MESSAGE_SIZE 192

typedef enum persist_status {
    PERSIST_OK = 0,
    PERSIST_ERR_NULL_ARG,  /* a required pointer argument was NULL */
    PERSIST_ERR_IO,        /* opening, reading, writing, closing or renaming failed */
    PERSIST_ERR_NO_MEMORY, /* an allocation failed */
    PERSIST_ERR_FORMAT,    /* the input is malformed; see err->line / err->message */
    PERSIST_ERR_VERSION    /* the header names an unsupported format version */
} persist_status;

typedef struct persist_error {
    persist_status status;
    size_t line;                              /* 1-based input line; 0 if not applicable */
    char message[PERSIST_ERROR_MESSAGE_SIZE]; /* NUL-terminated description */
} persist_error;

/* Human-readable name. Never returns NULL. */
const char *persist_status_str(persist_status status);

/*
 * Writes g to an open stream. Does not close it. The stream is flushed, and
 * any write error on it is reported as PERSIST_ERR_IO.
 */
persist_status persist_write(const graph *g, FILE *stream, persist_error *err);

/*
 * Reads a graph from an open stream (up to EOF). Does not close it.
 * On success *out receives a new graph that the caller owns and must free
 * with graph_free(). On failure *out is set to NULL (when out is non-NULL)
 * and nothing is leaked.
 */
persist_status persist_read(FILE *stream, graph **out, persist_error *err);

/*
 * Saves g to path atomically. The data is written to "<path>.tmp", flushed,
 * closed, and then renamed over path. A failure at any step leaves an
 * existing file at path untouched and removes the temporary file.
 *
 * The rename replaces the target atomically on POSIX systems. Standard C
 * cannot fsync, so this protects against crashes and failed writes in this
 * process, not against sudden power loss. Do not run two saves to the same
 * path at the same time, because they share one temporary file name.
 */
persist_status persist_save(const graph *g, const char *path, persist_error *err);

/*
 * Loads the graph stored at path. Ownership of *out is the same as for
 * persist_read(), and the file is always closed.
 */
persist_status persist_load(const char *path, graph **out, persist_error *err);

#endif /* GOALGRAPH_PERSIST_H */
