#include "algo.h"
#include "graph.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_STATUS(expr, expected) CHECK((expr) == (expected))

#define EPSILON 1e-9

/* ------------------------------------------------------------ helpers -- */

static bool near(double a, double b)
{
    return fabs(a - b) < EPSILON;
}

/* Deterministic LCG so random tests are reproducible. */
static uint32_t rng_next(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static size_t rng_below(uint32_t *state, size_t bound)
{
    return (size_t)rng_next(state) % bound;
}

/* Adds a node "n<i>" with the given status, effort and enjoyment. */
static size_t add_goal(graph *g, node_status status, int effort, int enjoyment)
{
    char id[32];
    const int written = snprintf(id, sizeof id, "n%zu", graph_node_count(g));
    CHECK(written > 0 && (size_t)written < sizeof id);

    const graph_node_spec spec = {
        .id = id,
        .category = "test",
        .status = status,
        .effort = effort,
        .enjoyment = enjoyment,
        .created_at = 1700000000,
        .updated_at = 1700000000,
    };
    size_t index = SIZE_MAX;
    CHECK_STATUS(graph_add_node(g, &spec, &index), GRAPH_OK);
    return index;
}

/* Creates a graph with n NOT_STARTED nodes. Aborts the test run on OOM. */
static graph *make_graph(size_t n)
{
    graph *g = graph_create();
    if (g == NULL) {
        fprintf(stderr, "graph_create failed\n");
        exit(1);
    }
    for (size_t i = 0; i < n; i++) {
        add_goal(g, NODE_STATUS_NOT_STARTED, 5, 5);
    }
    return g;
}

static void add_edge(graph *g, size_t u, size_t v, edge_type type)
{
    CHECK_STATUS(graph_add_edge(g, u, v, type, NULL), GRAPH_OK);
}

static void add_timed_edge(graph *g, size_t u, size_t v, float time_cost)
{
    const edge_weights w = {.time_cost = time_cost};
    CHECK_STATUS(graph_add_edge(g, u, v, EDGE_PREREQ, &w), GRAPH_OK);
}

static bool mask_has(algo_edge_mask mask, edge_type type)
{
    return mask == ALGO_EDGES_ALL || (mask & algo_edge_bit(type)) != 0;
}

/* Cheapest time_cost among masked edges u -> v, or INFINITY if none. */
static double min_edge_time(const graph *g, size_t u, size_t v, algo_edge_mask mask)
{
    const graph_edge *edges = NULL;
    size_t count = 0;
    double best = INFINITY;
    CHECK_STATUS(graph_out_edges(g, u, &edges, &count), GRAPH_OK);
    for (size_t i = 0; i < count; i++) {
        if (edges[i].target == v && mask_has(mask, edges[i].type) &&
            edges[i].weights.time_cost < best) {
            best = edges[i].weights.time_cost;
        }
    }
    return best;
}

static bool has_edge(const graph *g, size_t u, size_t v, algo_edge_mask mask)
{
    return min_edge_time(g, u, v, mask) != INFINITY;
}

/* Checks that path is a real source -> target path whose time cost matches. */
static void check_time_path(const graph *g, const algo_path *p, size_t source, size_t target)
{
    CHECK(p->length >= 1 && p->nodes != NULL);
    if (p->length == 0 || p->nodes == NULL) {
        return;
    }
    CHECK(p->nodes[0] == source);
    CHECK(p->nodes[p->length - 1] == target);
    double sum = 0.0;
    for (size_t i = 0; i + 1 < p->length; i++) {
        sum += min_edge_time(g, p->nodes[i], p->nodes[i + 1], ALGO_EDGES_ALL);
    }
    CHECK(near(sum, p->cost));
}

/* Bellman-Ford over time_cost; dist[v] = cost from source (INFINITY if unreachable). */
static void reference_dist_from(const graph *g, size_t source, double *dist)
{
    const size_t n = graph_node_count(g);
    for (size_t i = 0; i < n; i++) {
        dist[i] = INFINITY;
    }
    dist[source] = 0.0;
    for (size_t round = 0; round < n; round++) {
        for (size_t u = 0; u < n; u++) {
            const graph_edge *edges = NULL;
            size_t count = 0;
            CHECK_STATUS(graph_out_edges(g, u, &edges, &count), GRAPH_OK);
            for (size_t i = 0; i < count; i++) {
                const double d = dist[u] + edges[i].weights.time_cost;
                if (d < dist[edges[i].target]) {
                    dist[edges[i].target] = d;
                }
            }
        }
    }
}

/* Random graph with integer-valued time costs (so sums compare exactly). */
static graph *make_random_graph(uint32_t *rng, size_t n, size_t m)
{
    graph *g = make_graph(n);
    for (size_t i = 0; i < m; i++) {
        const size_t u = rng_below(rng, n);
        size_t v = rng_below(rng, n);
        if (u == v) {
            v = (v + 1) % n;
        }
        add_timed_edge(g, u, v, (float)rng_below(rng, 10));
    }
    return g;
}

/* ------------------------------------------------------ shortest paths -- */

static double time_cost_weight(const graph_edge *edge, void *ctx)
{
    (void)ctx;
    return edge->weights.time_cost;
}

/* Blocks edges into the node pointed to by ctx. */
static double avoid_node_weight(const graph_edge *edge, void *ctx)
{
    const size_t *avoid = ctx;
    return edge->target == *avoid ? INFINITY : 1.0;
}

static double negative_weight(const graph_edge *edge, void *ctx)
{
    (void)edge;
    (void)ctx;
    return -1.0;
}

static double nan_weight(const graph_edge *edge, void *ctx)
{
    (void)edge;
    (void)ctx;
    return NAN;
}

static void test_path_bad_input(void)
{
    graph *g = make_graph(2);
    add_edge(g, 0, 1, EDGE_PREREQ);
    algo_path p = {0};

    CHECK_STATUS(algo_shortest_path(NULL, 0, 1, NULL, &p), ALGO_ERR_NULL_ARG);
    CHECK(p.nodes == NULL && p.length == 0);
    CHECK_STATUS(algo_shortest_path(g, 0, 1, NULL, NULL), ALGO_ERR_NULL_ARG);
    CHECK_STATUS(algo_shortest_path(g, 0, 2, NULL, &p), ALGO_ERR_NOT_FOUND);
    CHECK_STATUS(algo_shortest_path(g, 7, 1, NULL, &p), ALGO_ERR_NOT_FOUND);

    algo_path_options opts = {.edge_types = 1u << EDGE_TYPE_COUNT};
    CHECK_STATUS(algo_shortest_path(g, 0, 1, &opts, &p), ALGO_ERR_INVALID_ARG);
    opts = (algo_path_options){.field = ALGO_WEIGHT_FIELD_COUNT};
    CHECK_STATUS(algo_shortest_path(g, 0, 1, &opts, &p), ALGO_ERR_INVALID_ARG);

    opts = (algo_path_options){.weight_fn = negative_weight};
    CHECK_STATUS(algo_shortest_path(g, 0, 1, &opts, &p), ALGO_ERR_BAD_WEIGHT);
    opts = (algo_path_options){.weight_fn = nan_weight};
    CHECK_STATUS(algo_shortest_path(g, 0, 1, &opts, &p), ALGO_ERR_BAD_WEIGHT);
    CHECK(p.nodes == NULL && p.length == 0);

    graph_free(g);

    /* Empty graph: no valid indices at all. */
    graph *empty = make_graph(0);
    CHECK_STATUS(algo_shortest_path(empty, 0, 0, NULL, &p), ALGO_ERR_NOT_FOUND);
    graph_free(empty);

    algo_path_free(NULL);
    algo_path_free(&p);
}

static void test_path_trivial_and_unreachable(void)
{
    /* Two disconnected parts: {0 -> 1} and {2 -> 3}. */
    graph *g = make_graph(4);
    add_edge(g, 0, 1, EDGE_PREREQ);
    add_edge(g, 2, 3, EDGE_PREREQ);
    algo_path p = {0};

    CHECK_STATUS(algo_shortest_path(g, 2, 2, NULL, &p), ALGO_OK);
    CHECK(p.length == 1 && p.nodes[0] == 2 && p.cost == 0.0);
    algo_path_free(&p);
    CHECK(p.nodes == NULL && p.length == 0);

    CHECK_STATUS(algo_shortest_path(g, 0, 3, NULL, &p), ALGO_ERR_NO_PATH);
    CHECK(p.nodes == NULL && p.length == 0);
    CHECK_STATUS(algo_shortest_path(g, 1, 0, NULL, &p), ALGO_ERR_NO_PATH); /* edges are directed */

    graph_free(g);
}

static void test_path_hops_and_weights(void)
{
    /* 0 -> 1 -> 2 -> 3 costs 1 each; shortcut 0 -> 3 costs 10. */
    graph *g = make_graph(4);
    add_timed_edge(g, 0, 1, 1.0f);
    add_timed_edge(g, 1, 2, 1.0f);
    add_timed_edge(g, 2, 3, 1.0f);
    add_timed_edge(g, 0, 3, 10.0f);
    algo_path p = {0};

    /* Default: hop count prefers the shortcut. */
    CHECK_STATUS(algo_shortest_path(g, 0, 3, NULL, &p), ALGO_OK);
    CHECK(p.length == 2 && p.nodes[0] == 0 && p.nodes[1] == 3);
    CHECK(near(p.cost, 1.0));
    algo_path_free(&p);

    /* time_cost field prefers the long chain. */
    const algo_path_options by_time = {.field = ALGO_WEIGHT_TIME_COST};
    CHECK_STATUS(algo_shortest_path(g, 0, 3, &by_time, &p), ALGO_OK);
    CHECK(p.length == 4);
    CHECK(near(p.cost, 3.0));
    check_time_path(g, &p, 0, 3);
    algo_path_free(&p);

    /* Custom weight function gives the same answer. */
    const algo_path_options by_fn = {.weight_fn = time_cost_weight};
    CHECK_STATUS(algo_shortest_path(g, 0, 3, &by_fn, &p), ALGO_OK);
    CHECK(p.length == 4 && near(p.cost, 3.0));
    algo_path_free(&p);

    /* A parallel, cheaper edge is used. */
    add_timed_edge(g, 0, 3, 0.5f);
    CHECK_STATUS(algo_shortest_path(g, 0, 3, &by_time, &p), ALGO_OK);
    CHECK(p.length == 2 && near(p.cost, 0.5));
    algo_path_free(&p);

    graph_free(g);
}

static void test_path_filters(void)
{
    /* 0 -PREREQ-> 1 -PREREQ-> 2 and 0 -SIMILAR-> 2. */
    graph *g = make_graph(3);
    add_edge(g, 0, 1, EDGE_PREREQ);
    add_edge(g, 1, 2, EDGE_PREREQ);
    add_edge(g, 0, 2, EDGE_SIMILAR);
    algo_path p = {0};

    CHECK_STATUS(algo_shortest_path(g, 0, 2, NULL, &p), ALGO_OK);
    CHECK(p.length == 2);
    algo_path_free(&p);

    const algo_path_options prereq_only = {.edge_types = algo_edge_bit(EDGE_PREREQ)};
    CHECK_STATUS(algo_shortest_path(g, 0, 2, &prereq_only, &p), ALGO_OK);
    CHECK(p.length == 3 && p.nodes[1] == 1);
    algo_path_free(&p);

    const algo_path_options conflicts_only = {.edge_types = algo_edge_bit(EDGE_CONFLICTS)};
    CHECK_STATUS(algo_shortest_path(g, 0, 2, &conflicts_only, &p), ALGO_ERR_NO_PATH);

    /* An INFINITY weight makes an edge impassable. */
    size_t avoid = 2;
    const algo_path_options no_entry = {.weight_fn = avoid_node_weight, .weight_ctx = &avoid};
    CHECK_STATUS(algo_shortest_path(g, 0, 2, &no_entry, &p), ALGO_ERR_NO_PATH);
    avoid = 1;
    CHECK_STATUS(algo_shortest_path(g, 0, 2, &no_entry, &p), ALGO_OK);
    CHECK(p.length == 2);
    algo_path_free(&p);

    graph_free(g);
}

static void test_path_with_cycles(void)
{
    /* 0 -> 1 -> 2 -> 0 cycle, plus 2 -> 3, and zero-cost edges. */
    graph *g = make_graph(4);
    add_timed_edge(g, 0, 1, 0.0f);
    add_timed_edge(g, 1, 2, 0.0f);
    add_timed_edge(g, 2, 0, 0.0f);
    add_timed_edge(g, 2, 3, 2.0f);
    algo_path p = {0};

    const algo_path_options by_time = {.field = ALGO_WEIGHT_TIME_COST};
    CHECK_STATUS(algo_shortest_path(g, 1, 3, &by_time, &p), ALGO_OK);
    CHECK(p.length == 3 && near(p.cost, 2.0));
    check_time_path(g, &p, 1, 3);
    algo_path_free(&p);

    graph_free(g);
}

static void test_path_random_vs_reference(void)
{
    uint32_t rng = 12345u;
    for (int trial = 0; trial < 40; trial++) {
        const size_t n = 2 + rng_below(&rng, 25);
        graph *g = make_random_graph(&rng, n, n * 3);
        double *dist = malloc(n * sizeof *dist);
        CHECK(dist != NULL);
        if (dist == NULL) {
            graph_free(g);
            return;
        }

        const size_t source = rng_below(&rng, n);
        reference_dist_from(g, source, dist);
        const algo_path_options by_time = {.field = ALGO_WEIGHT_TIME_COST};
        for (size_t t = 0; t < n; t++) {
            algo_path p = {0};
            const algo_status st = algo_shortest_path(g, source, t, &by_time, &p);
            if (dist[t] == INFINITY) {
                CHECK_STATUS(st, ALGO_ERR_NO_PATH);
            } else {
                CHECK_STATUS(st, ALGO_OK);
                CHECK(p.cost == dist[t]);
                check_time_path(g, &p, source, t);
            }
            algo_path_free(&p);
        }
        free(dist);
        graph_free(g);
    }
}

typedef struct grid_ctx {
    size_t width;
} grid_ctx;

static double manhattan(const graph *g, size_t node, size_t target, void *ctx)
{
    (void)g;
    const grid_ctx *grid = ctx;
    const double dx = fabs((double)(node % grid->width) - (double)(target % grid->width));
    const double dy = fabs((double)(node / grid->width) - (double)(target / grid->width));
    return dx + dy;
}

static double bad_heuristic(const graph *g, size_t node, size_t target, void *ctx)
{
    (void)g;
    (void)node;
    (void)target;
    (void)ctx;
    return -0.5;
}

static void test_astar_grid(void)
{
    /* 12x12 grid, 4-connected, with a wall at x == 6 except at y == 0. */
    const size_t w = 12;
    graph *g = make_graph(w * w);
    for (size_t y = 0; y < w; y++) {
        for (size_t x = 0; x < w; x++) {
            const size_t v = y * w + x;
            const bool wall_edge = (x == 5 && y != 0);
            if (x + 1 < w && !wall_edge) {
                add_edge(g, v, v + 1, EDGE_ENABLES);
                add_edge(g, v + 1, v, EDGE_ENABLES);
            }
            if (y + 1 < w) {
                add_edge(g, v, v + w, EDGE_ENABLES);
                add_edge(g, v + w, v, EDGE_ENABLES);
            }
        }
    }

    grid_ctx ctx = {.width = w};
    const algo_path_options astar = {.heuristic = manhattan, .heuristic_ctx = &ctx};
    const size_t source = (w - 1) * w + 0;  /* bottom-left */
    const size_t target = (w - 1) * w + 11; /* bottom-right */
    algo_path a = {0};
    algo_path d = {0};
    CHECK_STATUS(algo_shortest_path(g, source, target, &astar, &a), ALGO_OK);
    CHECK_STATUS(algo_shortest_path(g, source, target, NULL, &d), ALGO_OK);
    /* Must go up to row 0 and back down: 11 across + 2 * 11 vertical. */
    CHECK(near(a.cost, 33.0));
    CHECK(near(a.cost, d.cost));
    CHECK(a.length == 34);
    for (size_t i = 0; i + 1 < a.length; i++) {
        CHECK(has_edge(g, a.nodes[i], a.nodes[i + 1], ALGO_EDGES_ALL));
    }
    algo_path_free(&a);
    algo_path_free(&d);

    const algo_path_options bad = {.heuristic = bad_heuristic};
    CHECK_STATUS(algo_shortest_path(g, source, target, &bad, &a), ALGO_ERR_BAD_WEIGHT);
    CHECK(a.nodes == NULL);

    graph_free(g);
}

typedef struct scaled_ctx {
    const double *dist_to_target;
    const double *ratio;
} scaled_ctx;

/* Admissible but (generally) inconsistent: a random fraction of the true distance. */
static double scaled_heuristic(const graph *g, size_t node, size_t target, void *ctx)
{
    (void)g;
    (void)target;
    const scaled_ctx *s = ctx;
    const double d = s->dist_to_target[node];
    return d == INFINITY ? 0.0 : d * s->ratio[node];
}

/* dist_to[v] = cost from v to target, via reverse relaxation. */
static void reference_dist_to(const graph *g, size_t target, double *dist_to)
{
    const size_t n = graph_node_count(g);
    for (size_t i = 0; i < n; i++) {
        dist_to[i] = INFINITY;
    }
    dist_to[target] = 0.0;
    for (size_t round = 0; round < n; round++) {
        for (size_t u = 0; u < n; u++) {
            const graph_edge *edges = NULL;
            size_t count = 0;
            CHECK_STATUS(graph_out_edges(g, u, &edges, &count), GRAPH_OK);
            for (size_t i = 0; i < count; i++) {
                const double d = dist_to[edges[i].target] + edges[i].weights.time_cost;
                if (d < dist_to[u]) {
                    dist_to[u] = d;
                }
            }
        }
    }
}

static void test_astar_random_inconsistent(void)
{
    uint32_t rng = 777u;
    for (int trial = 0; trial < 40; trial++) {
        const size_t n = 2 + rng_below(&rng, 25);
        graph *g = make_random_graph(&rng, n, n * 3);
        double *dist_to = malloc(n * sizeof *dist_to);
        double *ratio = malloc(n * sizeof *ratio);
        CHECK(dist_to != NULL && ratio != NULL);
        if (dist_to == NULL || ratio == NULL) {
            free(dist_to);
            free(ratio);
            graph_free(g);
            return;
        }

        const size_t source = rng_below(&rng, n);
        const size_t target = rng_below(&rng, n);
        reference_dist_to(g, target, dist_to);
        for (size_t i = 0; i < n; i++) {
            ratio[i] = (double)rng_below(&rng, 5) / 4.0;
        }

        scaled_ctx ctx = {.dist_to_target = dist_to, .ratio = ratio};
        const algo_path_options astar = {
            .field = ALGO_WEIGHT_TIME_COST,
            .heuristic = scaled_heuristic,
            .heuristic_ctx = &ctx,
        };
        algo_path p = {0};
        const algo_status st = algo_shortest_path(g, source, target, &astar, &p);
        if (dist_to[source] == INFINITY) {
            CHECK_STATUS(st, ALGO_ERR_NO_PATH);
        } else {
            CHECK_STATUS(st, ALGO_OK);
            CHECK(p.cost == dist_to[source]);
            check_time_path(g, &p, source, target);
        }
        algo_path_free(&p);
        free(ratio);
        free(dist_to);
        graph_free(g);
    }
}

/* --------------------------------------------------- topological sort -- */

/* Checks that order is a permutation of 0..n-1 respecting every masked edge. */
static void check_topo_order(const graph *g, const algo_order *o, algo_edge_mask mask)
{
    const size_t n = graph_node_count(g);
    CHECK(o->count == n);
    if (o->count != n || n == 0) {
        return;
    }
    size_t *position = malloc(n * sizeof *position);
    CHECK(position != NULL);
    if (position == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        position[i] = SIZE_MAX;
    }
    for (size_t i = 0; i < n; i++) {
        CHECK(o->nodes[i] < n);
        if (o->nodes[i] < n) {
            CHECK(position[o->nodes[i]] == SIZE_MAX); /* no duplicates */
            position[o->nodes[i]] = i;
        }
    }
    for (size_t u = 0; u < n; u++) {
        const graph_edge *edges = NULL;
        size_t count = 0;
        CHECK_STATUS(graph_out_edges(g, u, &edges, &count), GRAPH_OK);
        for (size_t i = 0; i < count; i++) {
            if (mask_has(mask, edges[i].type)) {
                CHECK(position[u] < position[edges[i].target]);
            }
        }
    }
    free(position);
}

/* Checks that o lists a genuine cycle under mask. */
static void check_cycle(const graph *g, const algo_order *o, algo_edge_mask mask)
{
    CHECK(o->count >= 2 && o->nodes != NULL);
    if (o->count < 2 || o->nodes == NULL) {
        return;
    }
    for (size_t i = 0; i < o->count; i++) {
        const size_t next = o->nodes[(i + 1) % o->count];
        CHECK(has_edge(g, o->nodes[i], next, mask));
    }
}

static void test_topo_bad_input_and_empty(void)
{
    algo_order o = {0};
    CHECK_STATUS(algo_topo_sort(NULL, ALGO_EDGES_ALL, &o), ALGO_ERR_NULL_ARG);

    graph *g = make_graph(0);
    CHECK_STATUS(algo_topo_sort(g, ALGO_EDGES_ALL, NULL), ALGO_ERR_NULL_ARG);
    CHECK_STATUS(algo_topo_sort(g, 0x80000000u, &o), ALGO_ERR_INVALID_ARG);
    CHECK_STATUS(algo_topo_sort(g, ALGO_EDGES_ALL, &o), ALGO_OK);
    CHECK(o.count == 0 && o.nodes == NULL);
    graph_free(g);

    algo_order_free(NULL);
    algo_order_free(&o);
}

static void test_topo_dag(void)
{
    /*
     * 3 -> 1 -> 0, 3 -> 2, 2 -> 0, and isolated node 4 (disconnected).
     * Lexicographically smallest order: 3, 1, 2, 0, 4.
     */
    graph *g = make_graph(5);
    add_edge(g, 3, 1, EDGE_PREREQ);
    add_edge(g, 1, 0, EDGE_PREREQ);
    add_edge(g, 3, 2, EDGE_PREREQ);
    add_edge(g, 2, 0, EDGE_PREREQ);
    add_edge(g, 2, 0, EDGE_PREREQ); /* parallel edge */

    algo_order o = {0};
    CHECK_STATUS(algo_topo_sort(g, ALGO_EDGES_ALL, &o), ALGO_OK);
    check_topo_order(g, &o, ALGO_EDGES_ALL);
    const size_t expected[] = {3, 1, 2, 0, 4};
    CHECK(o.count == 5 && memcmp(o.nodes, expected, sizeof expected) == 0);
    algo_order_free(&o);
    CHECK(o.nodes == NULL && o.count == 0);

    graph_free(g);
}

static void test_topo_cycle(void)
{
    /* 0 -> 1 -> 2 -> 3 -> 1 (cycle 1,2,3), 3 -> 4 downstream, 5 isolated. */
    graph *g = make_graph(6);
    add_edge(g, 0, 1, EDGE_PREREQ);
    add_edge(g, 1, 2, EDGE_PREREQ);
    add_edge(g, 2, 3, EDGE_PREREQ);
    add_edge(g, 3, 1, EDGE_PREREQ);
    add_edge(g, 3, 4, EDGE_PREREQ);

    algo_order o = {0};
    CHECK_STATUS(algo_topo_sort(g, ALGO_EDGES_ALL, &o), ALGO_ERR_CYCLE);
    CHECK(o.count == 3);
    check_cycle(g, &o, ALGO_EDGES_ALL);
    algo_order_free(&o);
    graph_free(g);

    /* Two-node cycle. */
    g = make_graph(2);
    add_edge(g, 0, 1, EDGE_PREREQ);
    add_edge(g, 1, 0, EDGE_PREREQ);
    CHECK_STATUS(algo_topo_sort(g, ALGO_EDGES_ALL, &o), ALGO_ERR_CYCLE);
    CHECK(o.count == 2);
    check_cycle(g, &o, ALGO_EDGES_ALL);
    algo_order_free(&o);
    graph_free(g);
}

static void test_topo_mask(void)
{
    /* PREREQ edges form a DAG; a SIMILAR edge closes a cycle. */
    graph *g = make_graph(3);
    add_edge(g, 0, 1, EDGE_PREREQ);
    add_edge(g, 1, 2, EDGE_PREREQ);
    add_edge(g, 2, 0, EDGE_SIMILAR);

    algo_order o = {0};
    const algo_edge_mask prereq = algo_edge_bit(EDGE_PREREQ);
    CHECK_STATUS(algo_topo_sort(g, prereq, &o), ALGO_OK);
    check_topo_order(g, &o, prereq);
    algo_order_free(&o);

    CHECK_STATUS(algo_topo_sort(g, ALGO_EDGES_ALL, &o), ALGO_ERR_CYCLE);
    check_cycle(g, &o, ALGO_EDGES_ALL);
    algo_order_free(&o);

    const algo_edge_mask two = algo_edge_bit(EDGE_PREREQ) | algo_edge_bit(EDGE_SIMILAR);
    CHECK_STATUS(algo_topo_sort(g, two, &o), ALGO_ERR_CYCLE);
    algo_order_free(&o);

    graph_free(g);
}

static void test_topo_random(void)
{
    uint32_t rng = 4242u;
    for (int trial = 0; trial < 30; trial++) {
        /* Random DAG: edges only go from lower to higher rank. */
        const size_t n = 1 + rng_below(&rng, 40);
        graph *g = make_graph(n);
        for (size_t i = 0; n > 1 && i < n * 2; i++) {
            size_t u = rng_below(&rng, n);
            size_t v = rng_below(&rng, n);
            if (u == v) {
                continue;
            }
            if (u > v) {
                const size_t tmp = u;
                u = v;
                v = tmp;
            }
            add_edge(g, u, v, EDGE_PREREQ);
        }

        algo_order o = {0};
        CHECK_STATUS(algo_topo_sort(g, ALGO_EDGES_ALL, &o), ALGO_OK);
        check_topo_order(g, &o, ALGO_EDGES_ALL);
        algo_order_free(&o);

        /* A 2-cycle between the first and last node must be reported. */
        if (n > 1) {
            add_edge(g, n - 1, 0, EDGE_PREREQ);
            add_edge(g, 0, n - 1, EDGE_PREREQ);
            CHECK_STATUS(algo_topo_sort(g, ALGO_EDGES_ALL, &o), ALGO_ERR_CYCLE);
            check_cycle(g, &o, ALGO_EDGES_ALL);
            algo_order_free(&o);
        }
        graph_free(g);
    }
}

/* ---------------------------------------- strongly connected components -- */

/* Checks the invariants documented in algo.h. */
static void check_scc_consistent(const graph *g, const algo_scc *s, algo_edge_mask mask)
{
    const size_t n = graph_node_count(g);
    CHECK(s->node_count == n);
    CHECK(s->component_count >= 1 && s->component_count <= n);
    CHECK(s->offsets[0] == 0 && s->offsets[s->component_count] == n);
    for (size_t c = 0; c < s->component_count; c++) {
        CHECK(s->offsets[c] < s->offsets[c + 1]); /* no empty components */
        for (size_t k = s->offsets[c]; k < s->offsets[c + 1]; k++) {
            CHECK(s->component[s->members[k]] == c);
            if (k > s->offsets[c]) {
                CHECK(s->members[k - 1] < s->members[k]);
            }
        }
    }
    for (size_t u = 0; u < n; u++) {
        const graph_edge *edges = NULL;
        size_t count = 0;
        CHECK_STATUS(graph_out_edges(g, u, &edges, &count), GRAPH_OK);
        for (size_t i = 0; i < count; i++) {
            if (mask_has(mask, edges[i].type)) {
                CHECK(s->component[u] <= s->component[edges[i].target]);
            }
        }
    }
}

static void test_scc_bad_input_and_empty(void)
{
    algo_scc s = {0};
    CHECK_STATUS(algo_scc_compute(NULL, ALGO_EDGES_ALL, &s), ALGO_ERR_NULL_ARG);

    graph *g = make_graph(0);
    CHECK_STATUS(algo_scc_compute(g, ALGO_EDGES_ALL, NULL), ALGO_ERR_NULL_ARG);
    CHECK_STATUS(algo_scc_compute(g, 1u << 31, &s), ALGO_ERR_INVALID_ARG);
    CHECK_STATUS(algo_scc_compute(g, ALGO_EDGES_ALL, &s), ALGO_OK);
    CHECK(s.component_count == 0 && s.node_count == 0);
    CHECK(s.component == NULL && s.offsets == NULL && s.members == NULL);
    graph_free(g);

    algo_scc_free(NULL);
    algo_scc_free(&s);
}

static void test_scc_classic(void)
{
    /*
     * {0,1,2} cycle -> {3,4} cycle -> {5}; 6 isolated; 7 -> 0.
     * Condensation order: 7 before {0,1,2} before {3,4} before 5.
     */
    graph *g = make_graph(8);
    add_edge(g, 0, 1, EDGE_PREREQ);
    add_edge(g, 1, 2, EDGE_PREREQ);
    add_edge(g, 2, 0, EDGE_PREREQ);
    add_edge(g, 2, 3, EDGE_PREREQ);
    add_edge(g, 3, 4, EDGE_ENABLES);
    add_edge(g, 4, 3, EDGE_ENABLES);
    add_edge(g, 4, 5, EDGE_PREREQ);
    add_edge(g, 7, 0, EDGE_PREREQ);

    algo_scc s = {0};
    CHECK_STATUS(algo_scc_compute(g, ALGO_EDGES_ALL, &s), ALGO_OK);
    check_scc_consistent(g, &s, ALGO_EDGES_ALL);
    CHECK(s.component_count == 5);
    CHECK(s.component[0] == s.component[1] && s.component[1] == s.component[2]);
    CHECK(s.component[3] == s.component[4]);
    CHECK(s.component[0] != s.component[3]);
    CHECK(s.component[7] < s.component[0]);
    CHECK(s.component[0] < s.component[3]);
    CHECK(s.component[3] < s.component[5]);
    const size_t c = s.component[0];
    CHECK(s.offsets[c + 1] - s.offsets[c] == 3);
    algo_scc_free(&s);
    CHECK(s.component == NULL && s.component_count == 0);

    /* Without ENABLES edges, {3,4} splits into singletons. */
    const algo_edge_mask prereq = algo_edge_bit(EDGE_PREREQ);
    CHECK_STATUS(algo_scc_compute(g, prereq, &s), ALGO_OK);
    check_scc_consistent(g, &s, prereq);
    CHECK(s.component_count == 6);
    CHECK(s.component[3] != s.component[4]);
    algo_scc_free(&s);

    graph_free(g);
}

static void test_scc_no_edges_and_big_cycle(void)
{
    graph *g = make_graph(5);
    algo_scc s = {0};
    CHECK_STATUS(algo_scc_compute(g, ALGO_EDGES_ALL, &s), ALGO_OK);
    CHECK(s.component_count == 5);
    check_scc_consistent(g, &s, ALGO_EDGES_ALL);
    algo_scc_free(&s);
    graph_free(g);

    /* A long cycle: deep enough to overflow a recursive implementation. */
    const size_t n = 50000;
    g = make_graph(n);
    for (size_t i = 0; i < n; i++) {
        add_edge(g, i, (i + 1) % n, EDGE_PREREQ);
    }
    CHECK_STATUS(algo_scc_compute(g, ALGO_EDGES_ALL, &s), ALGO_OK);
    CHECK(s.component_count == 1);
    check_scc_consistent(g, &s, ALGO_EDGES_ALL);
    algo_scc_free(&s);

    algo_order o = {0};
    CHECK_STATUS(algo_topo_sort(g, ALGO_EDGES_ALL, &o), ALGO_ERR_CYCLE);
    CHECK(o.count == n);
    algo_order_free(&o);
    graph_free(g);
}

static void test_scc_random_vs_reachability(void)
{
    uint32_t rng = 99u;
    for (int trial = 0; trial < 30; trial++) {
        const size_t n = 2 + rng_below(&rng, 20);
        graph *g = make_random_graph(&rng, n, n + rng_below(&rng, n * 2));

        /* reach[u * n + v]: v reachable from u (Floyd-Warshall closure). */
        bool *reach = calloc(n * n, sizeof *reach);
        CHECK(reach != NULL);
        if (reach == NULL) {
            graph_free(g);
            return;
        }
        for (size_t u = 0; u < n; u++) {
            reach[u * n + u] = true;
            for (size_t v = 0; v < n; v++) {
                if (has_edge(g, u, v, ALGO_EDGES_ALL)) {
                    reach[u * n + v] = true;
                }
            }
        }
        for (size_t k = 0; k < n; k++) {
            for (size_t u = 0; u < n; u++) {
                for (size_t v = 0; v < n; v++) {
                    if (reach[u * n + k] && reach[k * n + v]) {
                        reach[u * n + v] = true;
                    }
                }
            }
        }

        algo_scc s = {0};
        CHECK_STATUS(algo_scc_compute(g, ALGO_EDGES_ALL, &s), ALGO_OK);
        check_scc_consistent(g, &s, ALGO_EDGES_ALL);
        for (size_t u = 0; u < n; u++) {
            for (size_t v = 0; v < n; v++) {
                const bool same = reach[u * n + v] && reach[v * n + u];
                CHECK(same == (s.component[u] == s.component[v]));
            }
        }
        algo_scc_free(&s);
        free(reach);
        graph_free(g);
    }
}

/* ------------------------------------------------------ recommendation -- */

static const algo_recommendation *find_rec(const algo_rec_list *list, size_t node)
{
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].node == node) {
            return &list->items[i];
        }
    }
    return NULL;
}

static void test_recommend_bad_input_and_empty(void)
{
    algo_rec_list r = {0};
    CHECK_STATUS(algo_recommend(NULL, NULL, &r), ALGO_ERR_NULL_ARG);

    graph *g = make_graph(0);
    CHECK_STATUS(algo_recommend(g, NULL, NULL), ALGO_ERR_NULL_ARG);
    CHECK_STATUS(algo_recommend(g, NULL, &r), ALGO_OK);
    CHECK(r.count == 0 && r.items == NULL);

    algo_rec_options o = algo_rec_default_options();
    o.effort_weight = -1.0;
    CHECK_STATUS(algo_recommend(g, &o, &r), ALGO_ERR_INVALID_ARG);
    o = algo_rec_default_options();
    o.enjoyment_weight = NAN;
    CHECK_STATUS(algo_recommend(g, &o, &r), ALGO_ERR_INVALID_ARG);
    o = algo_rec_default_options();
    o.achievability_weight = INFINITY;
    CHECK_STATUS(algo_recommend(g, &o, &r), ALGO_ERR_INVALID_ARG);
    o = (algo_rec_options){.conflict_penalty = 0.5};
    CHECK_STATUS(algo_recommend(g, &o, &r), ALGO_ERR_INVALID_ARG); /* all weights 0 */
    o = algo_rec_default_options();
    o.conflict_penalty = 1.5;
    CHECK_STATUS(algo_recommend(g, &o, &r), ALGO_ERR_INVALID_ARG);
    o.conflict_penalty = -0.1;
    CHECK_STATUS(algo_recommend(g, &o, &r), ALGO_ERR_INVALID_ARG);
    graph_free(g);

    algo_rec_list_free(NULL);
    algo_rec_list_free(&r);
}

static void test_recommend_defaults(void)
{
    const algo_rec_options d = algo_rec_default_options();
    CHECK(d.enjoyment_weight == 1.0 && d.effort_weight == 1.0 && d.achievability_weight == 1.0);
    CHECK(d.conflict_penalty == 0.5);
    CHECK(!d.include_blocked && d.include_in_progress && d.max_results == 0);
}

static void test_recommend_scoring(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);
    const size_t done = add_goal(g, NODE_STATUS_COMPLETED, 5, 5);     /* 0 */
    const size_t active = add_goal(g, NODE_STATUS_IN_PROGRESS, 5, 5); /* 1 */
    const size_t ready = add_goal(g, NODE_STATUS_NOT_STARTED, 1, 10); /* 2: easy, fun */
    const size_t half = add_goal(g, NODE_STATUS_NOT_STARTED, 10, 1);  /* 3: blocked */
    const size_t dropped = add_goal(g, NODE_STATUS_ABANDONED, 1, 10); /* 4 */
    const size_t clash = add_goal(g, NODE_STATUS_NOT_STARTED, 4, 7);  /* 5 */
    add_edge(g, done, ready, EDGE_PREREQ);
    add_edge(g, done, half, EDGE_PREREQ);
    add_edge(g, active, half, EDGE_PREREQ);
    add_edge(g, clash, active, EDGE_CONFLICTS);

    algo_rec_list r = {0};
    CHECK_STATUS(algo_recommend(g, NULL, &r), ALGO_OK);
    /* Candidates: active, ready, clash (half is blocked; done/dropped excluded). */
    CHECK(r.count == 3);
    CHECK(find_rec(&r, done) == NULL && find_rec(&r, dropped) == NULL);
    CHECK(find_rec(&r, half) == NULL);

    const algo_recommendation *rr = find_rec(&r, ready);
    CHECK(rr != NULL);
    if (rr != NULL) {
        CHECK(near(rr->readiness, 1.0) && near(rr->achievability, 1.0));
        CHECK(near(rr->score, 1.0)); /* max enjoyment, min effort, fully ready */
        CHECK(rr->prereq_total == 1 && rr->prereq_met == 1 && !rr->blocked);
    }

    /* clash conflicts with an in-progress goal: achievability halved. */
    const algo_recommendation *rc = find_rec(&r, clash);
    CHECK(rc != NULL);
    if (rc != NULL) {
        CHECK(rc->conflicted);
        CHECK(near(rc->achievability, 0.5));
        CHECK(near(rc->score, (6.0 / 9.0 + 6.0 / 9.0 + 0.5) / 3.0));
    }
    /* active's only conflict is with a NOT_STARTED goal, so it is not penalized. */
    const algo_recommendation *ra = find_rec(&r, active);
    CHECK(ra != NULL && !ra->conflicted);
    if (ra != NULL) {
        CHECK(near(ra->achievability, 1.0));
        CHECK(near(ra->score, (4.0 / 9.0 + 5.0 / 9.0 + 1.0) / 3.0));
    }

    /* Sorted by descending score. */
    for (size_t i = 0; i + 1 < r.count; i++) {
        CHECK(r.items[i].score >= r.items[i + 1].score);
    }
    CHECK(r.items[0].node == ready);
    algo_rec_list_free(&r);

    /* include_blocked: half gets credit 1 (done) + 0.5 (in progress) of 2. */
    algo_rec_options o = algo_rec_default_options();
    o.include_blocked = true;
    CHECK_STATUS(algo_recommend(g, &o, &r), ALGO_OK);
    CHECK(r.count == 4);
    const algo_recommendation *rh = find_rec(&r, half);
    CHECK(rh != NULL);
    if (rh != NULL) {
        CHECK(rh->blocked && rh->prereq_total == 2 && rh->prereq_met == 1);
        CHECK(near(rh->readiness, 0.75));
        CHECK(near(rh->score, (0.0 + 0.0 + 0.75) / 3.0));
    }
    algo_rec_list_free(&r);

    /* Excluding in-progress goals, and limiting results. */
    o = algo_rec_default_options();
    o.include_in_progress = false;
    CHECK_STATUS(algo_recommend(g, &o, &r), ALGO_OK);
    CHECK(r.count == 2 && find_rec(&r, active) == NULL);
    algo_rec_list_free(&r);

    o.max_results = 1;
    CHECK_STATUS(algo_recommend(g, &o, &r), ALGO_OK);
    CHECK(r.count == 1 && r.items[0].node == ready);
    algo_rec_list_free(&r);

    /* Weights change the ranking: caring only about achievability, the
     * unconflicted goals tie at 1.0 and are ordered by index. */
    o = algo_rec_default_options();
    o.enjoyment_weight = 0.0;
    o.effort_weight = 0.0;
    o.conflict_penalty = 0.0;
    CHECK_STATUS(algo_recommend(g, &o, &r), ALGO_OK);
    CHECK(r.count == 3);
    if (r.count == 3) {
        CHECK(near(r.items[0].score, 1.0) && near(r.items[1].score, 1.0));
        CHECK(r.items[0].node == active && r.items[1].node == ready); /* tie -> index */
        CHECK(r.items[2].node == clash && near(r.items[2].score, 0.0));
    }
    algo_rec_list_free(&r);

    graph_free(g);
}

static void test_recommend_no_candidates_and_cycles(void)
{
    /* Every goal completed: empty list, not an error. */
    graph *g = graph_create();
    CHECK(g != NULL);
    add_goal(g, NODE_STATUS_COMPLETED, 3, 3);
    add_goal(g, NODE_STATUS_ABANDONED, 3, 3);
    algo_rec_list r = {0};
    CHECK_STATUS(algo_recommend(g, NULL, &r), ALGO_OK);
    CHECK(r.count == 0 && r.items == NULL);
    graph_free(g);

    /* Prerequisite cycle: both goals are blocked forever. */
    g = make_graph(3);
    add_edge(g, 0, 1, EDGE_PREREQ);
    add_edge(g, 1, 0, EDGE_PREREQ);
    CHECK_STATUS(algo_recommend(g, NULL, &r), ALGO_OK);
    CHECK(r.count == 1 && r.items[0].node == 2); /* only the free-standing goal */
    algo_rec_list_free(&r);
    graph_free(g);
}

static void test_status_names(void)
{
    CHECK(strcmp(algo_status_str(ALGO_OK), "ok") == 0);
    CHECK(strcmp(algo_status_str(ALGO_ERR_CYCLE), "cycle detected") == 0);
    CHECK(strcmp(algo_status_str(ALGO_ERR_NO_PATH), "no path") == 0);
    CHECK(strcmp(algo_status_str((algo_status)99), "unknown") == 0);
    CHECK(algo_edge_bit(EDGE_PREREQ) == 1u);
    CHECK(algo_edge_bit(EDGE_CONFLICTS) == 8u);
}

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } const tests[] = {
        {"path_bad_input", test_path_bad_input},
        {"path_trivial_and_unreachable", test_path_trivial_and_unreachable},
        {"path_hops_and_weights", test_path_hops_and_weights},
        {"path_filters", test_path_filters},
        {"path_with_cycles", test_path_with_cycles},
        {"path_random_vs_reference", test_path_random_vs_reference},
        {"astar_grid", test_astar_grid},
        {"astar_random_inconsistent", test_astar_random_inconsistent},
        {"topo_bad_input_and_empty", test_topo_bad_input_and_empty},
        {"topo_dag", test_topo_dag},
        {"topo_cycle", test_topo_cycle},
        {"topo_mask", test_topo_mask},
        {"topo_random", test_topo_random},
        {"scc_bad_input_and_empty", test_scc_bad_input_and_empty},
        {"scc_classic", test_scc_classic},
        {"scc_no_edges_and_big_cycle", test_scc_no_edges_and_big_cycle},
        {"scc_random_vs_reachability", test_scc_random_vs_reachability},
        {"recommend_bad_input_and_empty", test_recommend_bad_input_and_empty},
        {"recommend_defaults", test_recommend_defaults},
        {"recommend_scoring", test_recommend_scoring},
        {"recommend_no_candidates_and_cycles", test_recommend_no_candidates_and_cycles},
        {"status_names", test_status_names},
    };

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before = g_failures;
        tests[i].fn();
        printf("  %-40s %s\n", tests[i].name, g_failures == before ? "ok" : "FAILED");
    }

    printf("test_algo: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
