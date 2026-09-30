#include "export.h"

#include "graph.h"

#include <stdarg.h>
#include <string.h>

#define HIGHLIGHT_COLOR "#ff6f00"

/* Output sink that remembers whether any write failed. */
typedef struct dot_writer {
    FILE *stream;
    bool ok;
} dot_writer;

static void emit(dot_writer *w, const char *format, ...)
{
    if (!w->ok) {
        return;
    }
    va_list args;
    va_start(args, format);
    const int written = vfprintf(w->stream, format, args);
    va_end(args);
    if (written < 0) {
        w->ok = false;
    }
}

static void emit_char(dot_writer *w, char c)
{
    if (w->ok && fputc(c, w->stream) == EOF) {
        w->ok = false;
    }
}

/* Writes the contents of a DOT double-quoted string (without the quotes). */
static void emit_escaped(dot_writer *w, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            emit_char(w, '\\');
            emit_char(w, (char)*p);
        } else if (*p == '\n') {
            emit(w, "\\n");
        } else if (*p < 0x20 || *p == 0x7f) {
            emit_char(w, ' ');
        } else {
            emit_char(w, (char)*p);
        }
    }
}

static void emit_quoted(dot_writer *w, const char *s)
{
    emit_char(w, '"');
    emit_escaped(w, s);
    emit_char(w, '"');
}

const char *export_status_str(export_status status)
{
    switch (status) {
    case EXPORT_OK:
        return "ok";
    case EXPORT_ERR_NULL_ARG:
        return "null argument";
    case EXPORT_ERR_INVALID_ARG:
        return "invalid argument";
    case EXPORT_ERR_IO:
        return "i/o error";
    }
    return "unknown";
}

/* ------------------------------------------------------------- styling -- */

static const char *status_fill(node_status status)
{
    switch (status) {
    case NODE_STATUS_IN_PROGRESS:
        return "#fff3b0";
    case NODE_STATUS_COMPLETED:
        return "#c8e6c9";
    case NODE_STATUS_ABANDONED:
        return "#e0e0e0";
    default:
        return "#ffffff";
    }
}

static const char *edge_style(edge_type type)
{
    switch (type) {
    case EDGE_PREREQ:
        return "color=\"#37474f\"";
    case EDGE_ENABLES:
        return "color=\"#1e88e5\"";
    case EDGE_SIMILAR:
        return "color=\"#9e9e9e\", style=dashed, arrowhead=none";
    case EDGE_CONFLICTS:
        return "color=\"#e53935\", style=bold, arrowhead=tee";
    default:
        return "color=\"#8e24aa\", style=dotted";
    }
}

static bool on_path(const export_dot_options *o, size_t node)
{
    for (size_t i = 0; i < o->highlight_count; i++) {
        if (o->highlight[i] == node) {
            return true;
        }
    }
    return false;
}

static bool path_step(const export_dot_options *o, size_t from, size_t to)
{
    for (size_t i = 0; i + 1 < o->highlight_count; i++) {
        if (o->highlight[i] == from && o->highlight[i + 1] == to) {
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------------- output -- */

static void emit_node(dot_writer *w, const graph_node_info *n, bool highlighted)
{
    emit(w, "  n%zu [label=\"", n->index);
    emit_escaped(w, n->id);
    emit(w, "\\n%s | effort %d | enjoy %d\", fillcolor=\"%s\"", node_status_str(n->status),
         n->effort, n->enjoyment, status_fill(n->status));
    if (n->status == NODE_STATUS_ABANDONED) {
        emit(w, ", fontcolor=\"#757575\", style=\"rounded,filled,dashed\"");
    }
    if (highlighted) {
        emit(w, ", color=\"" HIGHLIGHT_COLOR "\", penwidth=3");
    }
    emit(w, ", tooltip=");
    emit_quoted(w, n->category);
    emit(w, "];\n");
}

static void emit_nodes(dot_writer *w, const graph *g, const export_dot_options *o)
{
    const size_t n = graph_node_count(g);
    for (size_t i = 0; i < n && w->ok; i++) {
        graph_node_info info = {0};
        if (graph_get_node(g, i, &info) != GRAPH_OK) {
            w->ok = false;
            return;
        }
        emit_node(w, &info, on_path(o, i));
    }
}

static bool category_seen_before(const graph *g, size_t index, const char *category)
{
    for (size_t j = 0; j < index; j++) {
        graph_node_info other = {0};
        if (graph_get_node(g, j, &other) == GRAPH_OK && strcmp(other.category, category) == 0) {
            return true;
        }
    }
    return false;
}

/* One cluster per distinct non-empty category, in order of first appearance. */
static void emit_clusters(dot_writer *w, const graph *g)
{
    const size_t n = graph_node_count(g);
    for (size_t i = 0; i < n && w->ok; i++) {
        graph_node_info info = {0};
        if (graph_get_node(g, i, &info) != GRAPH_OK) {
            w->ok = false;
            return;
        }
        if (info.category[0] == '\0' || category_seen_before(g, i, info.category)) {
            continue;
        }
        emit(w, "  subgraph cluster_%zu {\n    label=", i);
        emit_quoted(w, info.category);
        emit(w, ";\n    style=\"rounded,dashed\";\n    color=\"#90a4ae\";\n");
        for (size_t k = i; k < n; k++) {
            graph_node_info member = {0};
            if (graph_get_node(g, k, &member) == GRAPH_OK &&
                strcmp(member.category, info.category) == 0) {
                emit(w, "    n%zu;\n", k);
            }
        }
        emit(w, "  }\n");
    }
}

static void emit_edges(dot_writer *w, const graph *g, const export_dot_options *o)
{
    const size_t n = graph_node_count(g);
    for (size_t u = 0; u < n && w->ok; u++) {
        const graph_edge *edges = NULL;
        size_t count = 0;
        if (graph_out_edges(g, u, &edges, &count) != GRAPH_OK) {
            w->ok = false;
            return;
        }
        for (size_t i = 0; i < count; i++) {
            const graph_edge *e = &edges[i];
            emit(w, "  n%zu -> n%zu [%s, tooltip=\"%s\"", e->source, e->target,
                 edge_style(e->type), edge_type_str(e->type));
            if (path_step(o, e->source, e->target)) {
                emit(w, ", color=\"" HIGHLIGHT_COLOR "\", penwidth=3");
            }
            emit(w, "];\n");
        }
    }
}

export_status export_dot(const graph *g, FILE *stream, const export_dot_options *opts)
{
    if (g == NULL || stream == NULL) {
        return EXPORT_ERR_NULL_ARG;
    }
    const export_dot_options defaults = {0};
    if (opts == NULL) {
        opts = &defaults;
    }
    if (opts->highlight_count > 0 && opts->highlight == NULL) {
        return EXPORT_ERR_NULL_ARG;
    }
    for (size_t i = 0; i < opts->highlight_count; i++) {
        if (opts->highlight[i] >= graph_node_count(g)) {
            return EXPORT_ERR_INVALID_ARG;
        }
    }

    dot_writer w = {.stream = stream, .ok = true};
    emit(&w, "digraph goalgraph {\n");
    emit(&w, "  graph [rankdir=LR, fontname=\"Helvetica\", nodesep=0.3, ranksep=0.6");
    if (opts->title != NULL) {
        emit(&w, ", labelloc=t, label=");
        emit_quoted(&w, opts->title);
    }
    emit(&w, "];\n");
    emit(&w, "  node [shape=box, style=\"rounded,filled\", fontname=\"Helvetica\", "
             "fontsize=10];\n");
    emit(&w, "  edge [fontname=\"Helvetica\", fontsize=9];\n");
    emit_nodes(&w, g, opts);
    if (opts->cluster_by_category) {
        emit_clusters(&w, g);
    }
    emit_edges(&w, g, opts);
    emit(&w, "}\n");

    if (!w.ok || fflush(stream) != 0 || ferror(stream)) {
        return EXPORT_ERR_IO;
    }
    return EXPORT_OK;
}

export_status export_dot_file(const graph *g, const char *path, const export_dot_options *opts)
{
    if (g == NULL || path == NULL) {
        return EXPORT_ERR_NULL_ARG;
    }
    FILE *stream = fopen(path, "wb");
    if (stream == NULL) {
        return EXPORT_ERR_IO;
    }
    export_status status = export_dot(g, stream, opts);
    if (fclose(stream) != 0 && status == EXPORT_OK) {
        status = EXPORT_ERR_IO;
    }
    return status;
}
