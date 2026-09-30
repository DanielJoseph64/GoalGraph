#ifndef GOALGRAPH_GRAPH_H
#define GOALGRAPH_GRAPH_H

/*
 * Graph ADT: a directed, weighted multigraph of goals.
 *
 * Nodes are identified by a unique string ID and by a dense integer index
 * (0 .. graph_node_count() - 1) assigned in insertion order. Edges are stored
 * in a dynamic out-adjacency list per source node; parallel edges between the
 * same pair of nodes (of the same or different types) are allowed.
 *
 * Ownership: the graph owns all memory it allocates. Strings passed in are
 * copied. Pointers returned by accessors are borrowed from the graph:
 *   - string pointers in graph_node_info stay valid until that string is
 *     replaced by graph_update_node(), the node is removed, or graph_free();
 *   - an edge array from graph_out_edges() stays valid until edges are added
 *     to or removed from that source node, any node is removed, or graph_free().
 */

#include <stddef.h>
#include <time.h>

typedef enum graph_status {
    GRAPH_OK = 0,
    GRAPH_ERR_NULL_ARG,     /* a required pointer argument was NULL */
    GRAPH_ERR_INVALID_ARG,  /* a value was out of range or malformed */
    GRAPH_ERR_NO_MEMORY,    /* an allocation failed; the graph is unchanged */
    GRAPH_ERR_DUPLICATE_ID, /* a node with that ID already exists */
    GRAPH_ERR_NOT_FOUND     /* no node with that ID or index */
} graph_status;

typedef enum node_status {
    NODE_STATUS_NOT_STARTED = 0,
    NODE_STATUS_IN_PROGRESS,
    NODE_STATUS_COMPLETED,
    NODE_STATUS_ABANDONED,
    NODE_STATUS_COUNT
} node_status;

typedef enum edge_type {
    EDGE_PREREQ = 0,
    EDGE_SIMILAR,
    EDGE_ENABLES,
    EDGE_CONFLICTS,
    EDGE_CUSTOM,
    EDGE_TYPE_COUNT
} edge_type;

#define GRAPH_SCORE_MIN 1
#define GRAPH_SCORE_MAX 10

/* Input for graph_add_node(). All pointers are borrowed and copied. */
typedef struct graph_node_spec {
    const char *id;       /* required, non-empty, unique within the graph */
    const char *category; /* optional; NULL is stored as "" */
    node_status status;
    int effort;           /* GRAPH_SCORE_MIN .. GRAPH_SCORE_MAX */
    int enjoyment;        /* GRAPH_SCORE_MIN .. GRAPH_SCORE_MAX */
    time_t created_at;    /* 0 means "now" */
    time_t updated_at;    /* 0 means "same as created_at" */
} graph_node_spec;

/* Read-only view of a node. String pointers are borrowed from the graph. */
typedef struct graph_node_info {
    const char *id;
    size_t index;
    const char *category;
    node_status status;
    int effort;
    int enjoyment;
    time_t created_at;
    time_t updated_at;
} graph_node_info;

typedef struct edge_weights {
    float effort;
    float enjoyment;
    float achievability;
    float similarity;
    float time_cost;
} edge_weights;

typedef struct graph_edge {
    size_t source; /* node index */
    size_t target; /* node index */
    edge_type type;
    edge_weights weights;
} graph_edge;

typedef struct graph graph;

/* Returns a new empty graph, or NULL if allocation fails. Free with graph_free(). */
graph *graph_create(void);

/* Frees the graph and everything it owns. Does nothing if g is NULL. */
void graph_free(graph *g);

/*
 * Adds a node described by spec. On success, writes the new node's index to
 * *out_index (if out_index is non-NULL). On failure, the graph is unchanged.
 */
graph_status graph_add_node(graph *g, const graph_node_spec *spec, size_t *out_index);

/*
 * Adds a directed edge source -> target. Both indices must refer to existing
 * nodes and must differ (self-loops are rejected). Every weight must be finite.
 * weights may be NULL, meaning all weights are 0. On failure, the graph is
 * unchanged.
 */
graph_status graph_add_edge(graph *g, size_t source, size_t target, edge_type type,
                            const edge_weights *weights);

/*
 * Updates the node at index from spec. The node keeps its index and edges.
 *   - spec->id: NULL keeps the current ID; otherwise renames the node (the
 *     new ID must be non-empty and not used by another node).
 *   - spec->category: NULL keeps the current category.
 *   - status, effort, enjoyment: replaced; validated as in graph_add_node().
 *   - created_at: 0 keeps the current value.
 *   - updated_at: 0 means "now".
 * On failure, the graph is unchanged.
 */
graph_status graph_update_node(graph *g, size_t index, const graph_node_spec *spec);

/*
 * Removes the node at index and every edge into or out of it. Every node
 * after it moves down one index, and edges are renumbered to match, so node
 * indices held by the caller must be looked up again.
 */
graph_status graph_remove_node(graph *g, size_t index);

/*
 * Removes every edge source -> target of the given type, keeping the order
 * of the remaining edges. Writes the number removed to *out_removed (if
 * non-NULL). Returns GRAPH_ERR_NOT_FOUND if an index is out of range or no
 * edge matched, and GRAPH_ERR_INVALID_ARG for an invalid type.
 */
graph_status graph_remove_edges(graph *g, size_t source, size_t target, edge_type type,
                                size_t *out_removed);

/* Returns the node count; 0 if g is NULL. */
size_t graph_node_count(const graph *g);

/* Returns the total edge count; 0 if g is NULL. */
size_t graph_edge_count(const graph *g);

/* Fills *out with a view of the node at index. */
graph_status graph_get_node(const graph *g, size_t index, graph_node_info *out);

/* Looks up a node by ID and writes its index to *out_index. */
graph_status graph_find_node(const graph *g, const char *id, size_t *out_index);

/*
 * Writes the out-edges of the node at index to *out_edges / *out_count.
 * *out_edges is NULL when *out_count is 0.
 */
graph_status graph_out_edges(const graph *g, size_t index, const graph_edge **out_edges,
                             size_t *out_count);

/* Human-readable names. Never return NULL. */
const char *graph_status_str(graph_status status);
const char *edge_type_str(edge_type type);
const char *node_status_str(node_status status);

#endif /* GOALGRAPH_GRAPH_H */
