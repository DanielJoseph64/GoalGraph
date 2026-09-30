#ifndef GOALGRAPH_ALGO_H
#define GOALGRAPH_ALGO_H

/*
 * Graph algorithms over a goal graph: shortest paths (Dijkstra / A*),
 * topological sort with cycle detection, strongly connected components, and
 * goal recommendation.
 *
 * Nodes are referred to by their dense graph index (see graph.h); map an index
 * to its string ID with graph_get_node().
 *
 * Edge direction convention: an edge A -> B of type EDGE_PREREQ means
 * "A is a prerequisite of B" (A must be completed before B). Topological
 * order therefore lists A before B.
 *
 * Ownership: every result struct (algo_path, algo_order, algo_scc,
 * algo_rec_list) owns its arrays. The caller must release it with the
 * matching *_free() function. Result structs are always left in a valid state
 * (possibly empty) after a call, even on failure, so calling *_free() is
 * always safe. Results do not borrow from the graph and remain valid after
 * graph_free().
 *
 * On entry each function overwrites *out without freeing it, so free a
 * previous result before reusing its struct.
 *
 * None of these functions modify the graph, and callbacks passed to them must
 * not modify it either.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graph.h"

typedef enum algo_status {
    ALGO_OK = 0,
    ALGO_ERR_NULL_ARG,    /* a required pointer argument was NULL */
    ALGO_ERR_INVALID_ARG, /* an option value was out of range */
    ALGO_ERR_NO_MEMORY,   /* an allocation failed */
    ALGO_ERR_NOT_FOUND,   /* a node index was out of range */
    ALGO_ERR_BAD_WEIGHT,  /* a weight or heuristic was negative or NaN */
    ALGO_ERR_NO_PATH,     /* the target is unreachable from the source */
    ALGO_ERR_CYCLE        /* the graph (restricted to the chosen edges) has a cycle */
} algo_status;

/* Human-readable name. Never returns NULL. */
const char *algo_status_str(algo_status status);

/* ------------------------------------------------------- edge filtering -- */

/*
 * Bit set of edge types an algorithm follows. Build one by OR-ing
 * algo_edge_bit() values. ALGO_EDGES_ALL (0) means "every edge type". Bits
 * at or above EDGE_TYPE_COUNT are rejected with ALGO_ERR_INVALID_ARG.
 */
typedef uint32_t algo_edge_mask;

#define ALGO_EDGES_ALL ((algo_edge_mask)0)

static inline algo_edge_mask algo_edge_bit(edge_type type)
{
    return (unsigned)type < 32u ? (algo_edge_mask)1u << (unsigned)type : 0u;
}

/* ------------------------------------------------------ shortest paths -- */

/* Which edge_weights field gives an edge's cost. */
typedef enum algo_weight_field {
    ALGO_WEIGHT_HOPS = 0, /* every edge costs 1 */
    ALGO_WEIGHT_EFFORT,
    ALGO_WEIGHT_ENJOYMENT,
    ALGO_WEIGHT_ACHIEVABILITY,
    ALGO_WEIGHT_SIMILARITY,
    ALGO_WEIGHT_TIME_COST,
    ALGO_WEIGHT_FIELD_COUNT
} algo_weight_field;

/*
 * Custom edge cost. Must return a value >= 0. Returning +INFINITY makes the
 * edge impassable. A negative or NaN result aborts the search with
 * ALGO_ERR_BAD_WEIGHT. edge is borrowed and valid only during the call.
 */
typedef double (*algo_weight_fn)(const graph_edge *edge, void *ctx);

/*
 * A* heuristic: an estimate of the remaining cost from node to target. It
 * must be finite and >= 0. A negative, NaN, or infinite result aborts the
 * search with ALGO_ERR_BAD_WEIGHT. The returned path is optimal when the
 * heuristic is admissible (it never overestimates the remaining cost).
 */
typedef double (*algo_heuristic_fn)(const graph *g, size_t node, size_t target, void *ctx);

/*
 * Search options. A zero-initialized struct (or passing NULL) means: follow
 * every edge type, each edge costs 1, and use plain Dijkstra.
 */
typedef struct algo_path_options {
    algo_edge_mask edge_types; /* ALGO_EDGES_ALL = every type */
    algo_weight_field field;   /* cost source when weight_fn is NULL */
    algo_weight_fn weight_fn;  /* optional; overrides field */
    void *weight_ctx;          /* passed through to weight_fn */
    algo_heuristic_fn heuristic; /* optional; non-NULL selects A* */
    void *heuristic_ctx;         /* passed through to heuristic */
} algo_path_options;

typedef struct algo_path {
    size_t *nodes; /* owned; nodes[0] is the source, nodes[length - 1] the target */
    size_t length; /* number of nodes (edges + 1); 0 when empty */
    double cost;   /* total cost of the path */
} algo_path;

/*
 * Finds a minimum-cost path from source to target (Dijkstra, or A* when
 * opts->heuristic is set). opts may be NULL.
 *
 * Returns ALGO_OK and fills *out on success. A source equal to target gives a
 * one-node path of cost 0. Returns ALGO_ERR_NO_PATH if target is unreachable.
 * On any failure *out is left empty. Free with algo_path_free().
 */
algo_status algo_shortest_path(const graph *g, size_t source, size_t target,
                               const algo_path_options *opts, algo_path *out);

/* Frees the path's arrays and resets it to empty. Does nothing if path is NULL. */
void algo_path_free(algo_path *path);

/* --------------------------------------------------- topological sort -- */

typedef struct algo_order {
    size_t *nodes; /* owned; NULL when count is 0 */
    size_t count;
} algo_order;

/*
 * Topologically sorts the graph, following only the edges selected by
 * edge_types. Ties are broken in favor of lower node indices, so the result
 * is deterministic.
 *
 * Returns ALGO_OK with every node in *out (an empty graph gives an empty
 * order). If the selected edges contain a cycle, returns ALGO_ERR_CYCLE and
 * fills *out with the nodes of one cycle: an edge runs from each node to the
 * next, and from the last node back to the first.
 * On any other failure *out is left empty. Free with algo_order_free().
 */
algo_status algo_topo_sort(const graph *g, algo_edge_mask edge_types, algo_order *out);

/* Frees the order's array and resets it to empty. Does nothing if order is NULL. */
void algo_order_free(algo_order *order);

/* ---------------------------------------- strongly connected components -- */

typedef struct algo_scc {
    size_t *component;     /* owned; component[i] is node i's component ID */
    size_t node_count;     /* length of component and members */
    size_t component_count;
    size_t *offsets;       /* owned; component_count + 1 entries */
    size_t *members;       /* owned; node_count entries */
} algo_scc;

/*
 * Computes strongly connected components (Tarjan's algorithm, iterative)
 * following only the edges selected by edge_types.
 *
 * Component IDs are 0 .. component_count - 1 and are numbered in topological
 * order of the condensation: every selected edge u -> v with u and v in
 * different components has component[u] < component[v]. Component c's
 * members are members[offsets[c]] .. members[offsets[c + 1] - 1], in
 * ascending index order.
 *
 * For an empty graph, returns ALGO_OK with all counts 0 and all arrays NULL.
 * On failure *out is left empty. Free with algo_scc_free().
 */
algo_status algo_scc_compute(const graph *g, algo_edge_mask edge_types, algo_scc *out);

/* Frees the arrays and resets the result to empty. Does nothing if scc is NULL. */
void algo_scc_free(algo_scc *scc);

/* ------------------------------------------------------ recommendation -- */

/*
 * Scoring model. Only NOT_STARTED goals (and IN_PROGRESS goals, if
 * include_in_progress is set) are candidates. For a candidate goal v:
 *
 *   enjoyment_n  = (enjoyment - GRAPH_SCORE_MIN) / (GRAPH_SCORE_MAX - GRAPH_SCORE_MIN)
 *   ease_n       = (GRAPH_SCORE_MAX - effort)    / (GRAPH_SCORE_MAX - GRAPH_SCORE_MIN)
 *   readiness    = credit / prerequisites (1 when v has no prerequisites), where
 *                  each incoming EDGE_PREREQ gives credit 1 if its source is
 *                  COMPLETED, 0.5 if IN_PROGRESS, and 0 otherwise
 *   achievability = readiness, multiplied by conflict_penalty if v has an
 *                  EDGE_CONFLICTS edge (either direction) with an IN_PROGRESS goal
 *   score = (enjoyment_weight * enjoyment_n + effort_weight * ease_n
 *            + achievability_weight * achievability) / (sum of the three weights)
 *
 * All terms and the score lie in [0, 1]. A goal is blocked if any
 * prerequisite is not COMPLETED. Parallel prerequisite edges count separately.
 */
typedef struct algo_rec_options {
    double enjoyment_weight;     /* >= 0 */
    double effort_weight;        /* >= 0; rewards low effort */
    double achievability_weight; /* >= 0 */
    double conflict_penalty;     /* multiplier in [0, 1] */
    bool include_blocked;        /* also list goals with unmet prerequisites */
    bool include_in_progress;    /* also list goals already in progress */
    size_t max_results;          /* 0 = no limit */
} algo_rec_options;

/*
 * Returns the default options: all three weights 1.0, conflict_penalty 0.5,
 * include_blocked false, include_in_progress true, max_results 0.
 */
algo_rec_options algo_rec_default_options(void);

typedef struct algo_recommendation {
    size_t node;
    double score;
    double achievability;
    double readiness;
    size_t prereq_total; /* incoming EDGE_PREREQ edges */
    size_t prereq_met;   /* of those, edges whose source is COMPLETED */
    bool blocked;        /* prereq_met < prereq_total */
    bool conflicted;     /* conflicts with an IN_PROGRESS goal */
} algo_recommendation;

typedef struct algo_rec_list {
    algo_recommendation *items; /* owned; NULL when count is 0 */
    size_t count;
} algo_rec_list;

/*
 * Scores the candidate goals and returns them sorted by descending score,
 * with ties broken by ascending node index. opts may be NULL (defaults).
 *
 * Returns ALGO_ERR_INVALID_ARG if a weight is negative or not finite, if all
 * three weights are 0, or if conflict_penalty is outside [0, 1].
 * On failure *out is left empty. Free with algo_rec_list_free().
 */
algo_status algo_recommend(const graph *g, const algo_rec_options *opts, algo_rec_list *out);

/* Frees the list's array and resets it to empty. Does nothing if list is NULL. */
void algo_rec_list_free(algo_rec_list *list);

#endif /* GOALGRAPH_ALGO_H */
