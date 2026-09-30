#include "algo.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static_assert(EDGE_TYPE_COUNT < 32, "edge types must fit in algo_edge_mask");

#define NO_NODE SIZE_MAX
#define HEAP_INITIAL_CAPACITY 16

/* ------------------------------------------------------------ helpers -- */

/* malloc for count elements; count must be > 0. Returns NULL on overflow. */
static void *alloc_array(size_t count, size_t elem_size)
{
    assert(count > 0 && elem_size > 0);
    if (count > SIZE_MAX / elem_size) {
        return NULL;
    }
    return malloc(count * elem_size);
}

/* calloc for count elements; count must be > 0. Returns NULL on overflow. */
static void *alloc_zeroed(size_t count, size_t elem_size)
{
    assert(count > 0 && elem_size > 0);
    if (count > SIZE_MAX / elem_size) {
        return NULL;
    }
    return calloc(count, elem_size);
}

static bool mask_valid(algo_edge_mask mask)
{
    const algo_edge_mask all = (algo_edge_mask)((1u << EDGE_TYPE_COUNT) - 1u);
    return (mask & ~all) == 0;
}

static bool mask_allows(algo_edge_mask mask, edge_type type)
{
    return mask == ALGO_EDGES_ALL || (mask & algo_edge_bit(type)) != 0;
}

/* Out-edges of a node known to exist. */
static void node_edges(const graph *g, size_t index, const graph_edge **edges, size_t *count)
{
    const graph_status status = graph_out_edges(g, index, edges, count);
    assert(status == GRAPH_OK);
    if (status != GRAPH_OK) {
        *edges = NULL;
        *count = 0;
    }
}

/* Info for a node known to exist. */
static graph_node_info node_info(const graph *g, size_t index)
{
    graph_node_info info = {0};
    const graph_status status = graph_get_node(g, index, &info);
    assert(status == GRAPH_OK);
    (void)status;
    return info;
}

/* ------------------------------------------------------- binary heap -- */

/* Min-heap ordered by priority, then by node index (for determinism). */
typedef struct heap_entry {
    double priority;
    double cost;
    size_t node;
} heap_entry;

typedef struct min_heap {
    heap_entry *items;
    size_t count;
    size_t capacity;
} min_heap;

static bool entry_before(const heap_entry *a, const heap_entry *b)
{
    if (a->priority != b->priority) {
        return a->priority < b->priority;
    }
    return a->node < b->node;
}

static bool heap_grow(min_heap *h)
{
    size_t new_capacity = HEAP_INITIAL_CAPACITY;
    if (h->capacity > 0) {
        if (h->capacity > SIZE_MAX / 2) {
            return false;
        }
        new_capacity = h->capacity * 2;
    }
    if (new_capacity > SIZE_MAX / sizeof *h->items) {
        return false;
    }

    heap_entry *const grown = realloc(h->items, new_capacity * sizeof *h->items);
    if (grown == NULL) {
        return false;
    }
    h->items = grown;
    h->capacity = new_capacity;
    return true;
}

/* Returns false on allocation failure; the heap is unchanged in that case. */
static bool heap_push(min_heap *h, heap_entry entry)
{
    if (h->count == h->capacity && !heap_grow(h)) {
        return false;
    }

    size_t i = h->count++;
    while (i > 0) {
        const size_t parent = (i - 1) / 2;
        if (!entry_before(&entry, &h->items[parent])) {
            break;
        }
        h->items[i] = h->items[parent];
        i = parent;
    }
    h->items[i] = entry;
    return true;
}

/* Removes the minimum into *out. Returns false if the heap is empty. */
static bool heap_pop(min_heap *h, heap_entry *out)
{
    if (h->count == 0) {
        return false;
    }
    *out = h->items[0];
    h->count--;
    if (h->count == 0) {
        return true;
    }

    const heap_entry last = h->items[h->count];
    size_t i = 0;
    for (;;) {
        size_t child = 2 * i + 1;
        if (child >= h->count) {
            break;
        }
        if (child + 1 < h->count && entry_before(&h->items[child + 1], &h->items[child])) {
            child++;
        }
        if (!entry_before(&h->items[child], &last)) {
            break;
        }
        h->items[i] = h->items[child];
        i = child;
    }
    h->items[i] = last;
    return true;
}

/* -------------------------------------------------------------- status -- */

const char *algo_status_str(algo_status status)
{
    switch (status) {
    case ALGO_OK:
        return "ok";
    case ALGO_ERR_NULL_ARG:
        return "null argument";
    case ALGO_ERR_INVALID_ARG:
        return "invalid argument";
    case ALGO_ERR_NO_MEMORY:
        return "out of memory";
    case ALGO_ERR_NOT_FOUND:
        return "node not found";
    case ALGO_ERR_BAD_WEIGHT:
        return "negative or NaN weight";
    case ALGO_ERR_NO_PATH:
        return "no path";
    case ALGO_ERR_CYCLE:
        return "cycle detected";
    }
    return "unknown";
}

/* ------------------------------------------------------ shortest paths -- */

static double field_weight(const graph_edge *edge, algo_weight_field field)
{
    switch (field) {
    case ALGO_WEIGHT_HOPS:
        return 1.0;
    case ALGO_WEIGHT_EFFORT:
        return edge->weights.effort;
    case ALGO_WEIGHT_ENJOYMENT:
        return edge->weights.enjoyment;
    case ALGO_WEIGHT_ACHIEVABILITY:
        return edge->weights.achievability;
    case ALGO_WEIGHT_SIMILARITY:
        return edge->weights.similarity;
    case ALGO_WEIGHT_TIME_COST:
        return edge->weights.time_cost;
    case ALGO_WEIGHT_FIELD_COUNT:
        break;
    }
    assert(!"field validated by caller");
    return NAN;
}

static algo_status edge_cost(const graph_edge *edge, const algo_path_options *opts, double *out)
{
    const double w = opts->weight_fn != NULL ? opts->weight_fn(edge, opts->weight_ctx)
                                             : field_weight(edge, opts->field);
    if (isnan(w) || w < 0.0) {
        return ALGO_ERR_BAD_WEIGHT;
    }
    *out = w;
    return ALGO_OK;
}

static algo_status heuristic_at(const graph *g, const algo_path_options *opts, size_t node,
                                size_t target, double *out)
{
    if (opts->heuristic == NULL) {
        *out = 0.0;
        return ALGO_OK;
    }
    const double h = opts->heuristic(g, node, target, opts->heuristic_ctx);
    if (!isfinite(h) || h < 0.0) {
        return ALGO_ERR_BAD_WEIGHT;
    }
    *out = h;
    return ALGO_OK;
}

typedef struct search_state {
    const graph *g;
    const algo_path_options *opts;
    size_t target;
    double *dist;
    size_t *pred;
    min_heap heap;
} search_state;

static algo_status relax_edges(search_state *s, size_t u)
{
    const graph_edge *edges = NULL;
    size_t count = 0;
    node_edges(s->g, u, &edges, &count);

    for (size_t i = 0; i < count; i++) {
        const graph_edge *const e = &edges[i];
        if (!mask_allows(s->opts->edge_types, e->type)) {
            continue;
        }

        double w = 0.0;
        algo_status status = edge_cost(e, s->opts, &w);
        if (status != ALGO_OK) {
            return status;
        }
        const double candidate = s->dist[u] + w;
        if (!(candidate < s->dist[e->target])) {
            continue; /* no improvement, impassable, or overflowed to infinity */
        }

        double h = 0.0;
        status = heuristic_at(s->g, s->opts, e->target, s->target, &h);
        if (status != ALGO_OK) {
            return status;
        }
        s->dist[e->target] = candidate;
        s->pred[e->target] = u;
        const heap_entry entry = {.priority = candidate + h, .cost = candidate, .node = e->target};
        if (!heap_push(&s->heap, entry)) {
            return ALGO_ERR_NO_MEMORY;
        }
    }
    return ALGO_OK;
}

/*
 * Dijkstra / A* with lazy deletion: stale heap entries are skipped when
 * popped. Nodes may be expanded again if a cheaper route is found later, so
 * the result stays optimal with an admissible but inconsistent heuristic.
 */
static algo_status run_search(search_state *s, size_t source)
{
    double h = 0.0;
    algo_status status = heuristic_at(s->g, s->opts, source, s->target, &h);
    if (status != ALGO_OK) {
        return status;
    }

    s->dist[source] = 0.0;
    if (!heap_push(&s->heap, (heap_entry){.priority = h, .cost = 0.0, .node = source})) {
        return ALGO_ERR_NO_MEMORY;
    }

    heap_entry top = {0};
    while (heap_pop(&s->heap, &top)) {
        if (top.cost > s->dist[top.node]) {
            continue;
        }
        if (top.node == s->target) {
            return ALGO_OK;
        }
        status = relax_edges(s, top.node);
        if (status != ALGO_OK) {
            return status;
        }
    }
    return ALGO_ERR_NO_PATH;
}

static algo_status build_path(const search_state *s, size_t source, size_t node_count,
                              algo_path *out)
{
    size_t length = 1;
    for (size_t v = s->target; v != source; v = s->pred[v]) {
        assert(s->pred[v] != NO_NODE);
        assert(length < node_count);
        length++;
    }
    (void)node_count;

    size_t *const nodes = alloc_array(length, sizeof *nodes);
    if (nodes == NULL) {
        return ALGO_ERR_NO_MEMORY;
    }
    size_t v = s->target;
    for (size_t i = length; i-- > 0;) {
        nodes[i] = v;
        v = s->pred[v];
    }

    *out = (algo_path){.nodes = nodes, .length = length, .cost = s->dist[s->target]};
    return ALGO_OK;
}

algo_status algo_shortest_path(const graph *g, size_t source, size_t target,
                               const algo_path_options *opts, algo_path *out)
{
    if (out != NULL) {
        *out = (algo_path){0};
    }
    if (g == NULL || out == NULL) {
        return ALGO_ERR_NULL_ARG;
    }

    const algo_path_options defaults = {0};
    if (opts == NULL) {
        opts = &defaults;
    }
    if (!mask_valid(opts->edge_types) || (unsigned)opts->field >= ALGO_WEIGHT_FIELD_COUNT) {
        return ALGO_ERR_INVALID_ARG;
    }

    const size_t n = graph_node_count(g);
    if (source >= n || target >= n) {
        return ALGO_ERR_NOT_FOUND;
    }

    search_state s = {
        .g = g,
        .opts = opts,
        .target = target,
        .dist = alloc_array(n, sizeof(double)),
        .pred = alloc_array(n, sizeof(size_t)),
        .heap = {0},
    };
    algo_status status = ALGO_OK;
    if (s.dist == NULL || s.pred == NULL) {
        status = ALGO_ERR_NO_MEMORY;
        goto cleanup;
    }
    for (size_t i = 0; i < n; i++) {
        s.dist[i] = INFINITY;
        s.pred[i] = NO_NODE;
    }

    status = run_search(&s, source);
    if (status == ALGO_OK) {
        status = build_path(&s, source, n, out);
    }

cleanup:
    free(s.heap.items);
    free(s.pred);
    free(s.dist);
    return status;
}

void algo_path_free(algo_path *path)
{
    if (path == NULL) {
        return;
    }
    free(path->nodes);
    *path = (algo_path){0};
}

/* --------------------------------------------------- topological sort -- */

typedef struct dfs_frame {
    size_t node;
    size_t next_edge;
} dfs_frame;

enum { COLOR_WHITE = 0, COLOR_GRAY, COLOR_BLACK };

/* Copies the gray path from node v to the top of the DFS stack into *out. */
static algo_status copy_cycle(const dfs_frame *stack, size_t depth, size_t v, algo_order *out)
{
    size_t start = 0;
    while (start < depth && stack[start].node != v) {
        start++;
    }
    assert(start < depth);

    const size_t count = depth - start;
    size_t *const nodes = alloc_array(count, sizeof *nodes);
    if (nodes == NULL) {
        return ALGO_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < count; i++) {
        nodes[i] = stack[start + i].node;
    }
    *out = (algo_order){.nodes = nodes, .count = count};
    return ALGO_OK;
}

/*
 * Finds one cycle among the nodes with in_scope[i] != 0, following only
 * masked edges between in-scope nodes. After Kahn's algorithm stalls, every
 * remaining node has an in-edge from another remaining node, so a cycle
 * exists and an iterative DFS finds it as a back edge.
 */
static algo_status find_cycle(const graph *g, size_t n, algo_edge_mask mask,
                              const size_t *in_scope, algo_order *out)
{
    unsigned char *const color = alloc_zeroed(n, sizeof *color);
    dfs_frame *const stack = alloc_array(n, sizeof *stack);
    algo_status status = ALGO_OK;
    if (color == NULL || stack == NULL) {
        status = ALGO_ERR_NO_MEMORY;
        goto cleanup;
    }

    for (size_t start = 0; start < n; start++) {
        if (in_scope[start] == 0 || color[start] != COLOR_WHITE) {
            continue;
        }
        size_t depth = 0;
        stack[depth++] = (dfs_frame){.node = start, .next_edge = 0};
        color[start] = COLOR_GRAY;

        while (depth > 0) {
            dfs_frame *const top = &stack[depth - 1];
            const graph_edge *edges = NULL;
            size_t count = 0;
            node_edges(g, top->node, &edges, &count);

            if (top->next_edge == count) {
                color[top->node] = COLOR_BLACK;
                depth--;
                continue;
            }
            const graph_edge *const e = &edges[top->next_edge++];
            if (!mask_allows(mask, e->type) || in_scope[e->target] == 0) {
                continue;
            }
            if (color[e->target] == COLOR_GRAY) {
                status = copy_cycle(stack, depth, e->target, out);
                goto cleanup;
            }
            if (color[e->target] == COLOR_WHITE) {
                color[e->target] = COLOR_GRAY;
                stack[depth++] = (dfs_frame){.node = e->target, .next_edge = 0};
            }
        }
    }
    assert(!"remaining nodes must contain a cycle");

cleanup:
    free(stack);
    free(color);
    return status;
}

static void count_indegrees(const graph *g, size_t n, algo_edge_mask mask, size_t *indegree)
{
    for (size_t u = 0; u < n; u++) {
        const graph_edge *edges = NULL;
        size_t count = 0;
        node_edges(g, u, &edges, &count);
        for (size_t i = 0; i < count; i++) {
            if (mask_allows(mask, edges[i].type)) {
                indegree[edges[i].target]++;
            }
        }
    }
}

/*
 * Kahn's algorithm with a min-heap of ready nodes, which yields the
 * lexicographically smallest topological order. Returns the number of nodes
 * placed into order (less than n when a cycle exists), or 0 with *status set
 * on allocation failure.
 */
static size_t kahn_order(const graph *g, size_t n, algo_edge_mask mask, size_t *indegree,
                         size_t *order, algo_status *status)
{
    min_heap ready = {0};
    size_t placed = 0;
    *status = ALGO_OK;

    for (size_t i = 0; i < n; i++) {
        if (indegree[i] == 0 && !heap_push(&ready, (heap_entry){.node = i})) {
            goto no_memory;
        }
    }

    heap_entry top = {0};
    while (heap_pop(&ready, &top)) {
        order[placed++] = top.node;
        const graph_edge *edges = NULL;
        size_t count = 0;
        node_edges(g, top.node, &edges, &count);
        for (size_t i = 0; i < count; i++) {
            if (!mask_allows(mask, edges[i].type)) {
                continue;
            }
            const size_t v = edges[i].target;
            indegree[v]--;
            if (indegree[v] == 0 && !heap_push(&ready, (heap_entry){.node = v})) {
                goto no_memory;
            }
        }
    }
    free(ready.items);
    return placed;

no_memory:
    free(ready.items);
    *status = ALGO_ERR_NO_MEMORY;
    return 0;
}

algo_status algo_topo_sort(const graph *g, algo_edge_mask edge_types, algo_order *out)
{
    if (out != NULL) {
        *out = (algo_order){0};
    }
    if (g == NULL || out == NULL) {
        return ALGO_ERR_NULL_ARG;
    }
    if (!mask_valid(edge_types)) {
        return ALGO_ERR_INVALID_ARG;
    }
    const size_t n = graph_node_count(g);
    if (n == 0) {
        return ALGO_OK;
    }

    size_t *const indegree = alloc_zeroed(n, sizeof *indegree);
    size_t *order = alloc_array(n, sizeof *order);
    algo_status status = ALGO_OK;
    if (indegree == NULL || order == NULL) {
        status = ALGO_ERR_NO_MEMORY;
        goto cleanup;
    }

    count_indegrees(g, n, edge_types, indegree);
    const size_t placed = kahn_order(g, n, edge_types, indegree, order, &status);
    if (status != ALGO_OK) {
        goto cleanup;
    }

    if (placed == n) {
        *out = (algo_order){.nodes = order, .count = n};
        order = NULL;
    } else {
        /* Unplaced nodes are exactly those whose in-degree never reached 0. */
        status = find_cycle(g, n, edge_types, indegree, out);
        if (status == ALGO_OK) {
            status = ALGO_ERR_CYCLE;
        }
    }

cleanup:
    free(order);
    free(indegree);
    return status;
}

void algo_order_free(algo_order *order)
{
    if (order == NULL) {
        return;
    }
    free(order->nodes);
    *order = (algo_order){0};
}

/* ---------------------------------------- strongly connected components -- */

typedef struct tarjan_state {
    const graph *g;
    algo_edge_mask mask;
    size_t *index;     /* discovery index, NO_NODE if unvisited */
    size_t *low;       /* lowest index reachable via the DFS subtree */
    bool *on_stack;
    size_t *stack;     /* Tarjan's node stack */
    size_t stack_size;
    dfs_frame *frames; /* explicit DFS call stack */
    size_t depth;
    size_t *component; /* component ID in discovery order (sinks first) */
    size_t component_count;
    size_t next_index;
} tarjan_state;

static void tarjan_visit(tarjan_state *t, size_t v)
{
    t->index[v] = t->next_index;
    t->low[v] = t->next_index;
    t->next_index++;
    t->stack[t->stack_size++] = v;
    t->on_stack[v] = true;
    t->frames[t->depth++] = (dfs_frame){.node = v, .next_edge = 0};
}

static void tarjan_pop_component(tarjan_state *t, size_t root)
{
    size_t w = NO_NODE;
    do {
        w = t->stack[--t->stack_size];
        t->on_stack[w] = false;
        t->component[w] = t->component_count;
    } while (w != root);
    t->component_count++;
}

/* Handles the next out-edge of the frame on top of the DFS stack. */
static void tarjan_step(tarjan_state *t, dfs_frame *top, const graph_edge *e)
{
    const size_t u = top->node;
    if (!mask_allows(t->mask, e->type)) {
        return;
    }
    const size_t v = e->target;
    if (t->index[v] == NO_NODE) {
        tarjan_visit(t, v);
    } else if (t->on_stack[v] && t->index[v] < t->low[u]) {
        t->low[u] = t->index[v];
    }
}

static void tarjan_run(tarjan_state *t, size_t start)
{
    tarjan_visit(t, start);
    while (t->depth > 0) {
        dfs_frame *const top = &t->frames[t->depth - 1];
        const size_t u = top->node;
        const graph_edge *edges = NULL;
        size_t count = 0;
        node_edges(t->g, u, &edges, &count);

        if (top->next_edge < count) {
            tarjan_step(t, top, &edges[top->next_edge++]);
            continue;
        }

        if (t->low[u] == t->index[u]) {
            tarjan_pop_component(t, u);
        }
        t->depth--;
        if (t->depth > 0) {
            const size_t parent = t->frames[t->depth - 1].node;
            if (t->low[u] < t->low[parent]) {
                t->low[parent] = t->low[u];
            }
        }
    }
}

/* Fills offsets/members (CSR layout) from component IDs. offsets must be zeroed. */
static void group_members(const size_t *component, size_t n, size_t component_count,
                          size_t *offsets, size_t *members)
{
    for (size_t i = 0; i < n; i++) {
        offsets[component[i] + 1]++;
    }
    for (size_t c = 1; c <= component_count; c++) {
        offsets[c] += offsets[c - 1];
    }
    /* Place members, advancing offsets[c] to the end of component c ... */
    for (size_t i = 0; i < n; i++) {
        members[offsets[component[i]]++] = i;
    }
    /* ... then shift back so offsets[c] is the start again. */
    for (size_t c = component_count; c > 0; c--) {
        offsets[c] = offsets[c - 1];
    }
    offsets[0] = 0;
}

algo_status algo_scc_compute(const graph *g, algo_edge_mask edge_types, algo_scc *out)
{
    if (out != NULL) {
        *out = (algo_scc){0};
    }
    if (g == NULL || out == NULL) {
        return ALGO_ERR_NULL_ARG;
    }
    if (!mask_valid(edge_types)) {
        return ALGO_ERR_INVALID_ARG;
    }
    const size_t n = graph_node_count(g);
    if (n == 0) {
        return ALGO_OK;
    }

    tarjan_state t = {
        .g = g,
        .mask = edge_types,
        .index = alloc_array(n, sizeof(size_t)),
        .low = alloc_array(n, sizeof(size_t)),
        .on_stack = alloc_zeroed(n, sizeof(bool)),
        .stack = alloc_array(n, sizeof(size_t)),
        .frames = alloc_array(n, sizeof(dfs_frame)),
        .component = alloc_array(n, sizeof(size_t)),
    };
    size_t *offsets = NULL;
    size_t *members = alloc_array(n, sizeof *members);
    algo_status status = ALGO_OK;
    if (t.index == NULL || t.low == NULL || t.on_stack == NULL || t.stack == NULL ||
        t.frames == NULL || t.component == NULL || members == NULL) {
        status = ALGO_ERR_NO_MEMORY;
        goto cleanup;
    }

    for (size_t i = 0; i < n; i++) {
        t.index[i] = NO_NODE;
    }
    for (size_t i = 0; i < n; i++) {
        if (t.index[i] == NO_NODE) {
            tarjan_run(&t, i);
        }
    }

    /* Tarjan emits sinks first; reverse so IDs follow topological order. */
    for (size_t i = 0; i < n; i++) {
        t.component[i] = t.component_count - 1 - t.component[i];
    }

    offsets = alloc_zeroed(t.component_count + 1, sizeof *offsets);
    if (offsets == NULL) {
        status = ALGO_ERR_NO_MEMORY;
        goto cleanup;
    }
    group_members(t.component, n, t.component_count, offsets, members);

    *out = (algo_scc){
        .component = t.component,
        .node_count = n,
        .component_count = t.component_count,
        .offsets = offsets,
        .members = members,
    };
    t.component = NULL;
    offsets = NULL;
    members = NULL;

cleanup:
    free(members);
    free(offsets);
    free(t.component);
    free(t.frames);
    free(t.stack);
    free(t.on_stack);
    free(t.low);
    free(t.index);
    return status;
}

void algo_scc_free(algo_scc *scc)
{
    if (scc == NULL) {
        return;
    }
    free(scc->component);
    free(scc->offsets);
    free(scc->members);
    *scc = (algo_scc){0};
}

/* ------------------------------------------------------ recommendation -- */

#define PREREQ_CREDIT_IN_PROGRESS 0.5

typedef struct goal_stats {
    node_status status;
    int effort;
    int enjoyment;
    size_t prereq_total;
    size_t prereq_met;
    double prereq_credit;
    bool conflicted;
} goal_stats;

algo_rec_options algo_rec_default_options(void)
{
    return (algo_rec_options){
        .enjoyment_weight = 1.0,
        .effort_weight = 1.0,
        .achievability_weight = 1.0,
        .conflict_penalty = 0.5,
        .include_blocked = false,
        .include_in_progress = true,
        .max_results = 0,
    };
}

static bool weight_valid(double w)
{
    return isfinite(w) && w >= 0.0;
}

static bool rec_options_valid(const algo_rec_options *o)
{
    if (!weight_valid(o->enjoyment_weight) || !weight_valid(o->effort_weight) ||
        !weight_valid(o->achievability_weight)) {
        return false;
    }
    const double sum = o->enjoyment_weight + o->effort_weight + o->achievability_weight;
    return isfinite(sum) && sum > 0.0 && isfinite(o->conflict_penalty) &&
           o->conflict_penalty >= 0.0 && o->conflict_penalty <= 1.0;
}

static void apply_edge(goal_stats *stats, const graph_edge *e)
{
    goal_stats *const src = &stats[e->source];
    goal_stats *const dst = &stats[e->target];
    if (e->type == EDGE_PREREQ) {
        dst->prereq_total++;
        if (src->status == NODE_STATUS_COMPLETED) {
            dst->prereq_met++;
            dst->prereq_credit += 1.0;
        } else if (src->status == NODE_STATUS_IN_PROGRESS) {
            dst->prereq_credit += PREREQ_CREDIT_IN_PROGRESS;
        }
    } else if (e->type == EDGE_CONFLICTS) {
        if (src->status == NODE_STATUS_IN_PROGRESS) {
            dst->conflicted = true;
        }
        if (dst->status == NODE_STATUS_IN_PROGRESS) {
            src->conflicted = true;
        }
    }
}

static void collect_stats(const graph *g, size_t n, goal_stats *stats)
{
    for (size_t i = 0; i < n; i++) {
        const graph_node_info info = node_info(g, i);
        stats[i] = (goal_stats){
            .status = info.status,
            .effort = info.effort,
            .enjoyment = info.enjoyment,
        };
    }
    for (size_t u = 0; u < n; u++) {
        const graph_edge *edges = NULL;
        size_t count = 0;
        node_edges(g, u, &edges, &count);
        for (size_t i = 0; i < count; i++) {
            apply_edge(stats, &edges[i]);
        }
    }
}

static bool is_candidate(const goal_stats *s, const algo_rec_options *o)
{
    const bool status_ok = s->status == NODE_STATUS_NOT_STARTED ||
                           (o->include_in_progress && s->status == NODE_STATUS_IN_PROGRESS);
    const bool blocked = s->prereq_met < s->prereq_total;
    return status_ok && (o->include_blocked || !blocked);
}

static algo_recommendation score_goal(const goal_stats *s, size_t node, const algo_rec_options *o)
{
    const double range = (double)(GRAPH_SCORE_MAX - GRAPH_SCORE_MIN);
    const double enjoyment_n = (double)(s->enjoyment - GRAPH_SCORE_MIN) / range;
    const double ease_n = (double)(GRAPH_SCORE_MAX - s->effort) / range;
    const double readiness =
        s->prereq_total > 0 ? s->prereq_credit / (double)s->prereq_total : 1.0;
    const double achievability = readiness * (s->conflicted ? o->conflict_penalty : 1.0);
    const double weight_sum = o->enjoyment_weight + o->effort_weight + o->achievability_weight;
    const double score = (o->enjoyment_weight * enjoyment_n + o->effort_weight * ease_n +
                          o->achievability_weight * achievability) /
                         weight_sum;

    return (algo_recommendation){
        .node = node,
        .score = score,
        .achievability = achievability,
        .readiness = readiness,
        .prereq_total = s->prereq_total,
        .prereq_met = s->prereq_met,
        .blocked = s->prereq_met < s->prereq_total,
        .conflicted = s->conflicted,
    };
}

/* Descending score, then ascending node index. */
static int compare_recommendations(const void *a, const void *b)
{
    const algo_recommendation *const x = a;
    const algo_recommendation *const y = b;
    if (x->score > y->score) {
        return -1;
    }
    if (x->score < y->score) {
        return 1;
    }
    return (x->node > y->node) - (x->node < y->node);
}

algo_status algo_recommend(const graph *g, const algo_rec_options *opts, algo_rec_list *out)
{
    if (out != NULL) {
        *out = (algo_rec_list){0};
    }
    if (g == NULL || out == NULL) {
        return ALGO_ERR_NULL_ARG;
    }

    const algo_rec_options defaults = algo_rec_default_options();
    if (opts == NULL) {
        opts = &defaults;
    }
    if (!rec_options_valid(opts)) {
        return ALGO_ERR_INVALID_ARG;
    }
    const size_t n = graph_node_count(g);
    if (n == 0) {
        return ALGO_OK;
    }

    goal_stats *const stats = alloc_array(n, sizeof *stats);
    algo_recommendation *items = alloc_array(n, sizeof *items);
    algo_status status = ALGO_OK;
    if (stats == NULL || items == NULL) {
        status = ALGO_ERR_NO_MEMORY;
        goto cleanup;
    }

    collect_stats(g, n, stats);
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (is_candidate(&stats[i], opts)) {
            items[count++] = score_goal(&stats[i], i, opts);
        }
    }

    qsort(items, count, sizeof *items, compare_recommendations);
    if (opts->max_results > 0 && count > opts->max_results) {
        count = opts->max_results;
    }
    if (count > 0) {
        *out = (algo_rec_list){.items = items, .count = count};
        items = NULL;
    }

cleanup:
    free(items);
    free(stats);
    return status;
}

void algo_rec_list_free(algo_rec_list *list)
{
    if (list == NULL) {
        return;
    }
    free(list->items);
    *list = (algo_rec_list){0};
}
