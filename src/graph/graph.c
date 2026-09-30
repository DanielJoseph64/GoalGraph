#include "graph.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 8

typedef struct node {
    char *id;
    char *category;
    node_status status;
    int effort;
    int enjoyment;
    time_t created_at;
    time_t updated_at;
    graph_edge *out; /* out-adjacency list */
    size_t out_count;
    size_t out_capacity;
} node;

struct graph {
    node *nodes;
    size_t node_count;
    size_t node_capacity;
    size_t edge_count;
};

static char *dup_string(const char *s)
{
    const size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len + 1);
    return copy;
}

/*
 * Makes room for one more element in a dynamic array holding count elements.
 * Returns the (possibly moved) array and updates *capacity, or returns NULL
 * on failure, in which case items and *capacity are left untouched.
 */
static void *reserve_one(void *items, size_t *capacity, size_t count, size_t elem_size)
{
    if (count < *capacity) {
        return items;
    }

    size_t new_capacity = INITIAL_CAPACITY;
    if (*capacity > 0) {
        if (*capacity > SIZE_MAX / 2) {
            return NULL;
        }
        new_capacity = *capacity * 2;
    }
    if (new_capacity > SIZE_MAX / elem_size) {
        return NULL;
    }

    void *grown = realloc(items, new_capacity * elem_size);
    if (grown == NULL) {
        return NULL;
    }
    *capacity = new_capacity;
    return grown;
}

static int score_in_range(int score)
{
    return score >= GRAPH_SCORE_MIN && score <= GRAPH_SCORE_MAX;
}

static int weights_finite(const edge_weights *w)
{
    return isfinite(w->effort) && isfinite(w->enjoyment) && isfinite(w->achievability) &&
           isfinite(w->similarity) && isfinite(w->time_cost);
}

static int find_index(const graph *g, const char *id, size_t *out_index)
{
    for (size_t i = 0; i < g->node_count; i++) {
        if (strcmp(g->nodes[i].id, id) == 0) {
            *out_index = i;
            return 1;
        }
    }
    return 0;
}

graph *graph_create(void)
{
    return calloc(1, sizeof(graph));
}

void graph_free(graph *g)
{
    if (g == NULL) {
        return;
    }
    for (size_t i = 0; i < g->node_count; i++) {
        free(g->nodes[i].id);
        free(g->nodes[i].category);
        free(g->nodes[i].out);
    }
    free(g->nodes);
    free(g);
}

graph_status graph_add_node(graph *g, const graph_node_spec *spec, size_t *out_index)
{
    if (g == NULL || spec == NULL || spec->id == NULL) {
        return GRAPH_ERR_NULL_ARG;
    }
    if (spec->id[0] == '\0' || (int)spec->status < 0 || spec->status >= NODE_STATUS_COUNT ||
        !score_in_range(spec->effort) || !score_in_range(spec->enjoyment)) {
        return GRAPH_ERR_INVALID_ARG;
    }

    size_t existing = 0;
    if (find_index(g, spec->id, &existing)) {
        return GRAPH_ERR_DUPLICATE_ID;
    }

    node *nodes = reserve_one(g->nodes, &g->node_capacity, g->node_count, sizeof(node));
    if (nodes == NULL) {
        return GRAPH_ERR_NO_MEMORY;
    }
    g->nodes = nodes;

    char *id = dup_string(spec->id);
    char *category = dup_string(spec->category != NULL ? spec->category : "");
    if (id == NULL || category == NULL) {
        free(id);
        free(category);
        return GRAPH_ERR_NO_MEMORY;
    }

    const time_t created_at = spec->created_at != 0 ? spec->created_at : time(NULL);
    const size_t index = g->node_count;

    g->nodes[index] = (node){
        .id = id,
        .category = category,
        .status = spec->status,
        .effort = spec->effort,
        .enjoyment = spec->enjoyment,
        .created_at = created_at,
        .updated_at = spec->updated_at != 0 ? spec->updated_at : created_at,
        .out = NULL,
        .out_count = 0,
        .out_capacity = 0,
    };
    g->node_count++;

    if (out_index != NULL) {
        *out_index = index;
    }
    return GRAPH_OK;
}

graph_status graph_add_edge(graph *g, size_t source, size_t target, edge_type type,
                            const edge_weights *weights)
{
    if (g == NULL) {
        return GRAPH_ERR_NULL_ARG;
    }
    if (source >= g->node_count || target >= g->node_count) {
        return GRAPH_ERR_NOT_FOUND;
    }

    const edge_weights zero = {0};
    if (weights == NULL) {
        weights = &zero;
    }
    if (source == target || (int)type < 0 || type >= EDGE_TYPE_COUNT ||
        !weights_finite(weights)) {
        return GRAPH_ERR_INVALID_ARG;
    }

    node *src = &g->nodes[source];
    graph_edge *out = reserve_one(src->out, &src->out_capacity, src->out_count, sizeof(graph_edge));
    if (out == NULL) {
        return GRAPH_ERR_NO_MEMORY;
    }
    src->out = out;

    src->out[src->out_count] = (graph_edge){
        .source = source,
        .target = target,
        .type = type,
        .weights = *weights,
    };
    src->out_count++;
    g->edge_count++;
    return GRAPH_OK;
}

/*
 * Sets *out_new to a copy of value if it differs from current (NULL value
 * means "keep"), else to NULL. Returns 0 on allocation failure.
 */
static int replace_string(const char *current, const char *value, char **out_new)
{
    *out_new = NULL;
    if (value == NULL || strcmp(current, value) == 0) {
        return 1;
    }
    *out_new = dup_string(value);
    return *out_new != NULL;
}

graph_status graph_update_node(graph *g, size_t index, const graph_node_spec *spec)
{
    if (g == NULL || spec == NULL) {
        return GRAPH_ERR_NULL_ARG;
    }
    if (index >= g->node_count) {
        return GRAPH_ERR_NOT_FOUND;
    }
    if ((spec->id != NULL && spec->id[0] == '\0') || (int)spec->status < 0 ||
        spec->status >= NODE_STATUS_COUNT || !score_in_range(spec->effort) ||
        !score_in_range(spec->enjoyment)) {
        return GRAPH_ERR_INVALID_ARG;
    }

    size_t existing = 0;
    if (spec->id != NULL && find_index(g, spec->id, &existing) && existing != index) {
        return GRAPH_ERR_DUPLICATE_ID;
    }

    node *n = &g->nodes[index];
    char *new_id = NULL;
    char *new_category = NULL;
    if (!replace_string(n->id, spec->id, &new_id) ||
        !replace_string(n->category, spec->category, &new_category)) {
        free(new_id);
        free(new_category);
        return GRAPH_ERR_NO_MEMORY;
    }

    if (new_id != NULL) {
        free(n->id);
        n->id = new_id;
    }
    if (new_category != NULL) {
        free(n->category);
        n->category = new_category;
    }
    n->status = spec->status;
    n->effort = spec->effort;
    n->enjoyment = spec->enjoyment;
    if (spec->created_at != 0) {
        n->created_at = spec->created_at;
    }
    n->updated_at = spec->updated_at != 0 ? spec->updated_at : time(NULL);
    return GRAPH_OK;
}

/*
 * Drops edges into the removed node and renumbers the rest of n's edges after
 * node `removed` has been deleted and n now sits at index `self`. Returns the
 * number of edges dropped.
 */
static size_t renumber_edges(node *n, size_t self, size_t removed)
{
    size_t kept = 0;
    for (size_t i = 0; i < n->out_count; i++) {
        graph_edge e = n->out[i];
        if (e.target == removed) {
            continue;
        }
        e.source = self;
        if (e.target > removed) {
            e.target--;
        }
        n->out[kept++] = e;
    }
    const size_t dropped = n->out_count - kept;
    n->out_count = kept;
    return dropped;
}

graph_status graph_remove_node(graph *g, size_t index)
{
    if (g == NULL) {
        return GRAPH_ERR_NULL_ARG;
    }
    if (index >= g->node_count) {
        return GRAPH_ERR_NOT_FOUND;
    }

    node *victim = &g->nodes[index];
    size_t removed_edges = victim->out_count;
    free(victim->id);
    free(victim->category);
    free(victim->out);
    memmove(&g->nodes[index], &g->nodes[index + 1],
            (g->node_count - index - 1) * sizeof(node));
    g->node_count--;

    for (size_t i = 0; i < g->node_count; i++) {
        removed_edges += renumber_edges(&g->nodes[i], i, index);
    }
    g->edge_count -= removed_edges;
    return GRAPH_OK;
}

graph_status graph_remove_edges(graph *g, size_t source, size_t target, edge_type type,
                                size_t *out_removed)
{
    if (out_removed != NULL) {
        *out_removed = 0;
    }
    if (g == NULL) {
        return GRAPH_ERR_NULL_ARG;
    }
    if (source >= g->node_count || target >= g->node_count) {
        return GRAPH_ERR_NOT_FOUND;
    }
    if ((int)type < 0 || type >= EDGE_TYPE_COUNT) {
        return GRAPH_ERR_INVALID_ARG;
    }

    node *src = &g->nodes[source];
    size_t kept = 0;
    for (size_t i = 0; i < src->out_count; i++) {
        if (src->out[i].target == target && src->out[i].type == type) {
            continue;
        }
        src->out[kept++] = src->out[i];
    }
    const size_t removed = src->out_count - kept;
    src->out_count = kept;
    g->edge_count -= removed;

    if (out_removed != NULL) {
        *out_removed = removed;
    }
    return removed > 0 ? GRAPH_OK : GRAPH_ERR_NOT_FOUND;
}

size_t graph_node_count(const graph *g)
{
    return g != NULL ? g->node_count : 0;
}

size_t graph_edge_count(const graph *g)
{
    return g != NULL ? g->edge_count : 0;
}

graph_status graph_get_node(const graph *g, size_t index, graph_node_info *out)
{
    if (g == NULL || out == NULL) {
        return GRAPH_ERR_NULL_ARG;
    }
    if (index >= g->node_count) {
        return GRAPH_ERR_NOT_FOUND;
    }

    const node *n = &g->nodes[index];
    *out = (graph_node_info){
        .id = n->id,
        .index = index,
        .category = n->category,
        .status = n->status,
        .effort = n->effort,
        .enjoyment = n->enjoyment,
        .created_at = n->created_at,
        .updated_at = n->updated_at,
    };
    return GRAPH_OK;
}

graph_status graph_find_node(const graph *g, const char *id, size_t *out_index)
{
    if (g == NULL || id == NULL || out_index == NULL) {
        return GRAPH_ERR_NULL_ARG;
    }
    return find_index(g, id, out_index) ? GRAPH_OK : GRAPH_ERR_NOT_FOUND;
}

graph_status graph_out_edges(const graph *g, size_t index, const graph_edge **out_edges,
                             size_t *out_count)
{
    if (g == NULL || out_edges == NULL || out_count == NULL) {
        return GRAPH_ERR_NULL_ARG;
    }
    if (index >= g->node_count) {
        return GRAPH_ERR_NOT_FOUND;
    }

    const node *n = &g->nodes[index];
    *out_edges = n->out_count > 0 ? n->out : NULL;
    *out_count = n->out_count;
    return GRAPH_OK;
}

const char *graph_status_str(graph_status status)
{
    switch (status) {
    case GRAPH_OK:
        return "ok";
    case GRAPH_ERR_NULL_ARG:
        return "null argument";
    case GRAPH_ERR_INVALID_ARG:
        return "invalid argument";
    case GRAPH_ERR_NO_MEMORY:
        return "out of memory";
    case GRAPH_ERR_DUPLICATE_ID:
        return "duplicate node id";
    case GRAPH_ERR_NOT_FOUND:
        return "not found";
    default:
        return "unknown status";
    }
}

const char *edge_type_str(edge_type type)
{
    switch (type) {
    case EDGE_PREREQ:
        return "PREREQ";
    case EDGE_SIMILAR:
        return "SIMILAR";
    case EDGE_ENABLES:
        return "ENABLES";
    case EDGE_CONFLICTS:
        return "CONFLICTS";
    case EDGE_CUSTOM:
        return "CUSTOM";
    default:
        return "UNKNOWN";
    }
}

const char *node_status_str(node_status status)
{
    switch (status) {
    case NODE_STATUS_NOT_STARTED:
        return "not_started";
    case NODE_STATUS_IN_PROGRESS:
        return "in_progress";
    case NODE_STATUS_COMPLETED:
        return "completed";
    case NODE_STATUS_ABANDONED:
        return "abandoned";
    default:
        return "unknown";
    }
}
