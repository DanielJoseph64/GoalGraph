#ifndef GOALGRAPH_EXPORT_H
#define GOALGRAPH_EXPORT_H

/*
 * Graphviz DOT export of a goal graph.
 *
 * Nodes are named n<index> in the DOT output and labeled with their goal ID,
 * status and scores. Node fill color shows status. Edge color and style
 * show edge type:
 *   PREREQ    dark solid          ENABLES  blue solid
 *   SIMILAR   gray dashed, no arrowhead
 *   CONFLICTS red bold, tee arrowhead
 *   CUSTOM    purple dotted
 *
 * Render with, e.g.:  dot -Tsvg goals.dot -o goals.svg
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct graph graph;

typedef enum export_status {
    EXPORT_OK = 0,
    EXPORT_ERR_NULL_ARG,    /* a required pointer argument was NULL */
    EXPORT_ERR_INVALID_ARG, /* a highlight index was out of range */
    EXPORT_ERR_IO           /* opening, writing or closing the output failed */
} export_status;

typedef struct export_dot_options {
    bool cluster_by_category; /* draw a box around goals sharing a non-empty category */
    const size_t *highlight;  /* optional path of node indices to emphasize (borrowed) */
    size_t highlight_count;   /* number of entries in highlight */
    const char *title;        /* optional graph caption; NULL for none (borrowed) */
} export_dot_options;

/* Human-readable name. Never returns NULL. */
const char *export_status_str(export_status status);

/*
 * Writes g as a DOT digraph to an open stream. The stream is flushed but not
 * closed. opts may be NULL (no clusters, no highlight, no title). Nodes on
 * the highlight path, and edges between consecutive highlight entries, are
 * drawn in bold orange. Highlighting costs O(highlight_count) per node and
 * per edge.
 */
export_status export_dot(const graph *g, FILE *stream, const export_dot_options *opts);

/* Like export_dot(), but creates or truncates the file at path and always closes it. */
export_status export_dot_file(const graph *g, const char *path, const export_dot_options *opts);

#endif /* GOALGRAPH_EXPORT_H */
