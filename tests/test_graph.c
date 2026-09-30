#include "graph.h"

#include <math.h>
#include <stdio.h>
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

static graph_node_spec make_spec(const char *id)
{
    return (graph_node_spec){
        .id = id,
        .category = "fitness",
        .status = NODE_STATUS_NOT_STARTED,
        .effort = 5,
        .enjoyment = 7,
        .created_at = 1700000000,
        .updated_at = 1700000500,
    };
}

static void test_create_and_free(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);
    CHECK(graph_node_count(g) == 0);
    CHECK(graph_edge_count(g) == 0);
    graph_free(g);

    graph_free(NULL); /* must be a no-op */
    CHECK(graph_node_count(NULL) == 0);
    CHECK(graph_edge_count(NULL) == 0);
}

static void test_add_node_and_get(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);

    size_t idx = 99;
    const graph_node_spec spec = make_spec("run-5k");
    CHECK_STATUS(graph_add_node(g, &spec, &idx), GRAPH_OK);
    CHECK(idx == 0);
    CHECK(graph_node_count(g) == 1);

    graph_node_info info;
    CHECK_STATUS(graph_get_node(g, 0, &info), GRAPH_OK);
    CHECK(strcmp(info.id, "run-5k") == 0);
    CHECK(info.index == 0);
    CHECK(strcmp(info.category, "fitness") == 0);
    CHECK(info.status == NODE_STATUS_NOT_STARTED);
    CHECK(info.effort == 5);
    CHECK(info.enjoyment == 7);
    CHECK(info.created_at == 1700000000);
    CHECK(info.updated_at == 1700000500);

    /* Second node gets the next index; out_index may be NULL. */
    graph_node_spec spec2 = make_spec("run-10k");
    spec2.status = NODE_STATUS_IN_PROGRESS;
    CHECK_STATUS(graph_add_node(g, &spec2, NULL), GRAPH_OK);
    CHECK(graph_node_count(g) == 2);
    CHECK_STATUS(graph_get_node(g, 1, &info), GRAPH_OK);
    CHECK(info.index == 1);
    CHECK(info.status == NODE_STATUS_IN_PROGRESS);

    graph_free(g);
}

static void test_add_node_copies_strings_and_defaults(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);

    char id_buf[] = "learn-c";
    graph_node_spec spec = make_spec(id_buf);
    spec.category = NULL;
    spec.created_at = 0;
    spec.updated_at = 0;

    const time_t before = time(NULL);
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_OK);
    const time_t after = time(NULL);

    id_buf[0] = 'X'; /* graph must hold its own copy */

    graph_node_info info;
    CHECK_STATUS(graph_get_node(g, 0, &info), GRAPH_OK);
    CHECK(strcmp(info.id, "learn-c") == 0);
    CHECK(info.category != NULL && info.category[0] == '\0');
    CHECK(info.created_at >= before && info.created_at <= after);
    CHECK(info.updated_at == info.created_at);

    graph_free(g);
}

static void test_add_node_rejects_bad_input(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);

    graph_node_spec spec = make_spec("a");
    CHECK_STATUS(graph_add_node(NULL, &spec, NULL), GRAPH_ERR_NULL_ARG);
    CHECK_STATUS(graph_add_node(g, NULL, NULL), GRAPH_ERR_NULL_ARG);

    spec.id = NULL;
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_ERR_NULL_ARG);
    spec.id = "";
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_ERR_INVALID_ARG);

    spec = make_spec("a");
    spec.effort = GRAPH_SCORE_MIN - 1;
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_ERR_INVALID_ARG);
    spec.effort = GRAPH_SCORE_MAX + 1;
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_ERR_INVALID_ARG);

    spec = make_spec("a");
    spec.enjoyment = 0;
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_ERR_INVALID_ARG);
    spec.enjoyment = 11;
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_ERR_INVALID_ARG);

    spec = make_spec("a");
    spec.status = NODE_STATUS_COUNT;
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_ERR_INVALID_ARG);

    CHECK(graph_node_count(g) == 0); /* nothing was added by failed calls */

    /* Boundary scores are accepted. */
    spec = make_spec("a");
    spec.effort = GRAPH_SCORE_MIN;
    spec.enjoyment = GRAPH_SCORE_MAX;
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_OK);

    /* Duplicate IDs are rejected. */
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_ERR_DUPLICATE_ID);
    CHECK(graph_node_count(g) == 1);

    graph_free(g);
}

static void test_find_node(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);

    const char *ids[] = {"alpha", "beta", "gamma"};
    for (size_t i = 0; i < 3; i++) {
        const graph_node_spec spec = make_spec(ids[i]);
        CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_OK);
    }

    size_t idx = 0;
    CHECK_STATUS(graph_find_node(g, "gamma", &idx), GRAPH_OK);
    CHECK(idx == 2);
    CHECK_STATUS(graph_find_node(g, "alpha", &idx), GRAPH_OK);
    CHECK(idx == 0);
    CHECK_STATUS(graph_find_node(g, "delta", &idx), GRAPH_ERR_NOT_FOUND);

    CHECK_STATUS(graph_find_node(NULL, "alpha", &idx), GRAPH_ERR_NULL_ARG);
    CHECK_STATUS(graph_find_node(g, NULL, &idx), GRAPH_ERR_NULL_ARG);
    CHECK_STATUS(graph_find_node(g, "alpha", NULL), GRAPH_ERR_NULL_ARG);

    graph_free(g);
}

static void test_get_node_rejects_bad_input(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);
    const graph_node_spec spec = make_spec("only");
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_OK);

    graph_node_info info;
    CHECK_STATUS(graph_get_node(NULL, 0, &info), GRAPH_ERR_NULL_ARG);
    CHECK_STATUS(graph_get_node(g, 0, NULL), GRAPH_ERR_NULL_ARG);
    CHECK_STATUS(graph_get_node(g, 1, &info), GRAPH_ERR_NOT_FOUND);

    graph_free(g);
}

static void test_add_edge_and_out_edges(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);

    size_t a = 0;
    size_t b = 0;
    size_t c = 0;
    graph_node_spec spec = make_spec("a");
    CHECK_STATUS(graph_add_node(g, &spec, &a), GRAPH_OK);
    spec.id = "b";
    CHECK_STATUS(graph_add_node(g, &spec, &b), GRAPH_OK);
    spec.id = "c";
    CHECK_STATUS(graph_add_node(g, &spec, &c), GRAPH_OK);

    const edge_weights w = {
        .effort = 2.5f,
        .enjoyment = 8.0f,
        .achievability = 0.75f,
        .similarity = 0.1f,
        .time_cost = 12.0f,
    };
    CHECK_STATUS(graph_add_edge(g, a, b, EDGE_PREREQ, &w), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, a, c, EDGE_ENABLES, NULL), GRAPH_OK);
    CHECK(graph_edge_count(g) == 2);

    const graph_edge *edges = NULL;
    size_t count = 0;
    CHECK_STATUS(graph_out_edges(g, a, &edges, &count), GRAPH_OK);
    CHECK(count == 2);
    CHECK(edges != NULL);
    if (edges != NULL && count == 2) {
        CHECK(edges[0].source == a);
        CHECK(edges[0].target == b);
        CHECK(edges[0].type == EDGE_PREREQ);
        CHECK(edges[0].weights.effort == 2.5f);
        CHECK(edges[0].weights.enjoyment == 8.0f);
        CHECK(edges[0].weights.achievability == 0.75f);
        CHECK(edges[0].weights.similarity == 0.1f);
        CHECK(edges[0].weights.time_cost == 12.0f);

        CHECK(edges[1].target == c);
        CHECK(edges[1].type == EDGE_ENABLES);
        CHECK(edges[1].weights.effort == 0.0f); /* NULL weights -> zeros */
        CHECK(edges[1].weights.time_cost == 0.0f);
    }

    /* A node with no out-edges reports an empty list. */
    CHECK_STATUS(graph_out_edges(g, b, &edges, &count), GRAPH_OK);
    CHECK(count == 0);
    CHECK(edges == NULL);

    graph_free(g);
}

static void test_multigraph_all_edge_types(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);

    graph_node_spec spec = make_spec("x");
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_OK);
    spec.id = "y";
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_OK);

    /* One edge of each type between the same pair, plus an exact duplicate. */
    for (int t = 0; t < EDGE_TYPE_COUNT; t++) {
        CHECK_STATUS(graph_add_edge(g, 0, 1, (edge_type)t, NULL), GRAPH_OK);
    }
    CHECK_STATUS(graph_add_edge(g, 0, 1, EDGE_PREREQ, NULL), GRAPH_OK);
    /* Reverse direction is a separate edge. */
    CHECK_STATUS(graph_add_edge(g, 1, 0, EDGE_CONFLICTS, NULL), GRAPH_OK);

    CHECK(graph_edge_count(g) == (size_t)EDGE_TYPE_COUNT + 2);

    const graph_edge *edges = NULL;
    size_t count = 0;
    CHECK_STATUS(graph_out_edges(g, 0, &edges, &count), GRAPH_OK);
    CHECK(count == (size_t)EDGE_TYPE_COUNT + 1);
    if (edges != NULL && count == (size_t)EDGE_TYPE_COUNT + 1) {
        for (int t = 0; t < EDGE_TYPE_COUNT; t++) {
            CHECK(edges[t].type == (edge_type)t);
        }
        CHECK(edges[EDGE_TYPE_COUNT].type == EDGE_PREREQ);
    }

    CHECK_STATUS(graph_out_edges(g, 1, &edges, &count), GRAPH_OK);
    CHECK(count == 1);

    graph_free(g);
}

static void test_add_edge_rejects_bad_input(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);

    graph_node_spec spec = make_spec("p");
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_OK);
    spec.id = "q";
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_OK);

    CHECK_STATUS(graph_add_edge(NULL, 0, 1, EDGE_PREREQ, NULL), GRAPH_ERR_NULL_ARG);
    CHECK_STATUS(graph_add_edge(g, 0, 2, EDGE_PREREQ, NULL), GRAPH_ERR_NOT_FOUND);
    CHECK_STATUS(graph_add_edge(g, 5, 1, EDGE_PREREQ, NULL), GRAPH_ERR_NOT_FOUND);
    CHECK_STATUS(graph_add_edge(g, 0, 0, EDGE_PREREQ, NULL), GRAPH_ERR_INVALID_ARG);
    CHECK_STATUS(graph_add_edge(g, 0, 1, EDGE_TYPE_COUNT, NULL), GRAPH_ERR_INVALID_ARG);

    edge_weights w = {0};
    w.achievability = NAN;
    CHECK_STATUS(graph_add_edge(g, 0, 1, EDGE_PREREQ, &w), GRAPH_ERR_INVALID_ARG);
    w.achievability = 0.0f;
    w.time_cost = INFINITY;
    CHECK_STATUS(graph_add_edge(g, 0, 1, EDGE_PREREQ, &w), GRAPH_ERR_INVALID_ARG);

    CHECK(graph_edge_count(g) == 0);

    const graph_edge *edges = NULL;
    size_t count = 0;
    CHECK_STATUS(graph_out_edges(NULL, 0, &edges, &count), GRAPH_ERR_NULL_ARG);
    CHECK_STATUS(graph_out_edges(g, 0, NULL, &count), GRAPH_ERR_NULL_ARG);
    CHECK_STATUS(graph_out_edges(g, 0, &edges, NULL), GRAPH_ERR_NULL_ARG);
    CHECK_STATUS(graph_out_edges(g, 2, &edges, &count), GRAPH_ERR_NOT_FOUND);

    graph_free(g);
}

/* Enough nodes and edges to force several reallocations of every array. */
static void test_growth(void)
{
    enum { NODES = 500, EDGES_PER_NODE = 20 };

    graph *g = graph_create();
    CHECK(g != NULL);

    char id[32];
    for (int i = 0; i < NODES; i++) {
        snprintf(id, sizeof id, "goal-%d", i);
        const graph_node_spec spec = make_spec(id);
        size_t idx = 0;
        CHECK_STATUS(graph_add_node(g, &spec, &idx), GRAPH_OK);
        CHECK(idx == (size_t)i);
    }
    CHECK(graph_node_count(g) == NODES);

    for (size_t i = 0; i < NODES; i++) {
        for (size_t k = 1; k <= EDGES_PER_NODE; k++) {
            const size_t target = (i + k) % NODES;
            const edge_weights w = {.effort = (float)k};
            CHECK_STATUS(graph_add_edge(g, i, target, EDGE_ENABLES, &w), GRAPH_OK);
        }
    }
    CHECK(graph_edge_count(g) == (size_t)NODES * EDGES_PER_NODE);

    /* Spot-check that data survived reallocation. */
    size_t idx = 0;
    CHECK_STATUS(graph_find_node(g, "goal-321", &idx), GRAPH_OK);
    CHECK(idx == 321);

    graph_node_info info;
    CHECK_STATUS(graph_get_node(g, 0, &info), GRAPH_OK);
    CHECK(strcmp(info.id, "goal-0") == 0);

    const graph_edge *edges = NULL;
    size_t count = 0;
    CHECK_STATUS(graph_out_edges(g, NODES - 1, &edges, &count), GRAPH_OK);
    CHECK(count == EDGES_PER_NODE);
    if (edges != NULL && count == EDGES_PER_NODE) {
        CHECK(edges[0].target == 0);
        CHECK(edges[EDGES_PER_NODE - 1].weights.effort == (float)EDGES_PER_NODE);
    }

    graph_free(g);
}

static void test_update_node(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);
    graph_node_spec spec = make_spec("a");
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_OK);
    spec = make_spec("b");
    CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, 0, 1, EDGE_PREREQ, NULL), GRAPH_OK);

    /* NULL id/category keep the current values; created_at 0 keeps it. */
    graph_node_spec upd = {
        .status = NODE_STATUS_COMPLETED, .effort = 2, .enjoyment = 9, .updated_at = 1800000000};
    CHECK_STATUS(graph_update_node(g, 0, &upd), GRAPH_OK);
    graph_node_info info;
    CHECK_STATUS(graph_get_node(g, 0, &info), GRAPH_OK);
    CHECK(strcmp(info.id, "a") == 0 && strcmp(info.category, "fitness") == 0);
    CHECK(info.status == NODE_STATUS_COMPLETED && info.effort == 2 && info.enjoyment == 9);
    CHECK(info.created_at == 1700000000 && info.updated_at == 1800000000);

    /* Rename and recategorize; updated_at 0 means now. Edges are kept. */
    upd.id = "a2";
    upd.category = "health";
    upd.created_at = 1600000000;
    upd.updated_at = 0;
    const time_t before = time(NULL);
    CHECK_STATUS(graph_update_node(g, 0, &upd), GRAPH_OK);
    CHECK_STATUS(graph_get_node(g, 0, &info), GRAPH_OK);
    CHECK(strcmp(info.id, "a2") == 0 && strcmp(info.category, "health") == 0);
    CHECK(info.created_at == 1600000000 && info.updated_at >= before);
    size_t idx = 99;
    CHECK_STATUS(graph_find_node(g, "a2", &idx), GRAPH_OK);
    CHECK(idx == 0);
    CHECK_STATUS(graph_find_node(g, "a", &idx), GRAPH_ERR_NOT_FOUND);
    CHECK(graph_edge_count(g) == 1);

    /* Renaming to its own ID is fine; to another node's ID is not. */
    CHECK_STATUS(graph_update_node(g, 0, &upd), GRAPH_OK);
    upd.id = "b";
    CHECK_STATUS(graph_update_node(g, 0, &upd), GRAPH_ERR_DUPLICATE_ID);

    /* Invalid input leaves the node unchanged. */
    upd.id = "";
    CHECK_STATUS(graph_update_node(g, 0, &upd), GRAPH_ERR_INVALID_ARG);
    upd.id = NULL;
    upd.effort = 11;
    CHECK_STATUS(graph_update_node(g, 0, &upd), GRAPH_ERR_INVALID_ARG);
    upd.effort = 5;
    upd.status = NODE_STATUS_COUNT;
    CHECK_STATUS(graph_update_node(g, 0, &upd), GRAPH_ERR_INVALID_ARG);
    CHECK_STATUS(graph_get_node(g, 0, &info), GRAPH_OK);
    CHECK(strcmp(info.id, "a2") == 0 && info.effort == 2);

    upd.status = NODE_STATUS_NOT_STARTED;
    CHECK_STATUS(graph_update_node(NULL, 0, &upd), GRAPH_ERR_NULL_ARG);
    CHECK_STATUS(graph_update_node(g, 0, NULL), GRAPH_ERR_NULL_ARG);
    CHECK_STATUS(graph_update_node(g, 2, &upd), GRAPH_ERR_NOT_FOUND);

    graph_free(g);
}

static void test_remove_node(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);
    const char *ids[] = {"a", "b", "c", "d"};
    for (size_t i = 0; i < 4; i++) {
        const graph_node_spec spec = make_spec(ids[i]);
        CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_OK);
    }
    /* a->b, a->c, b->c, c->d, d->a, b->d */
    const size_t pairs[][2] = {{0, 1}, {0, 2}, {1, 2}, {2, 3}, {3, 0}, {1, 3}};
    for (size_t i = 0; i < 6; i++) {
        const edge_weights w = {.effort = (float)i};
        CHECK_STATUS(graph_add_edge(g, pairs[i][0], pairs[i][1], EDGE_ENABLES, &w), GRAPH_OK);
    }

    /* Removing b drops a->b, b->c, b->d; c and d shift down. */
    CHECK_STATUS(graph_remove_node(g, 1), GRAPH_OK);
    CHECK(graph_node_count(g) == 3);
    CHECK(graph_edge_count(g) == 3);
    size_t idx = 99;
    CHECK_STATUS(graph_find_node(g, "b", &idx), GRAPH_ERR_NOT_FOUND);
    CHECK_STATUS(graph_find_node(g, "c", &idx), GRAPH_OK);
    CHECK(idx == 1);
    CHECK_STATUS(graph_find_node(g, "d", &idx), GRAPH_OK);
    CHECK(idx == 2);

    const graph_edge *edges = NULL;
    size_t count = 0;
    CHECK_STATUS(graph_out_edges(g, 0, &edges, &count), GRAPH_OK); /* a->c */
    CHECK(count == 1 && edges[0].source == 0 && edges[0].target == 1);
    CHECK(edges[0].weights.effort == 1.0f);
    CHECK_STATUS(graph_out_edges(g, 1, &edges, &count), GRAPH_OK); /* c->d */
    CHECK(count == 1 && edges[0].source == 1 && edges[0].target == 2);
    CHECK_STATUS(graph_out_edges(g, 2, &edges, &count), GRAPH_OK); /* d->a */
    CHECK(count == 1 && edges[0].source == 2 && edges[0].target == 0);

    /* Remove the last and the first node, then the only one left. */
    CHECK_STATUS(graph_remove_node(g, 2), GRAPH_OK);
    CHECK(graph_node_count(g) == 2 && graph_edge_count(g) == 1);
    CHECK_STATUS(graph_remove_node(g, 0), GRAPH_OK);
    CHECK(graph_node_count(g) == 1 && graph_edge_count(g) == 0);
    CHECK_STATUS(graph_remove_node(g, 0), GRAPH_OK);
    CHECK(graph_node_count(g) == 0);

    CHECK_STATUS(graph_remove_node(g, 0), GRAPH_ERR_NOT_FOUND);
    CHECK_STATUS(graph_remove_node(NULL, 0), GRAPH_ERR_NULL_ARG);

    /* The graph is still usable afterwards. */
    const graph_node_spec spec = make_spec("again");
    CHECK_STATUS(graph_add_node(g, &spec, &idx), GRAPH_OK);
    CHECK(idx == 0);
    graph_free(g);
}

static void test_remove_edges(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);
    for (size_t i = 0; i < 3; i++) {
        const char *ids[] = {"a", "b", "c"};
        const graph_node_spec spec = make_spec(ids[i]);
        CHECK_STATUS(graph_add_node(g, &spec, NULL), GRAPH_OK);
    }
    CHECK_STATUS(graph_add_edge(g, 0, 1, EDGE_PREREQ, NULL), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, 0, 1, EDGE_SIMILAR, NULL), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, 0, 2, EDGE_PREREQ, NULL), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, 0, 1, EDGE_PREREQ, NULL), GRAPH_OK);

    size_t removed = 99;
    CHECK_STATUS(graph_remove_edges(g, 0, 1, EDGE_PREREQ, &removed), GRAPH_OK);
    CHECK(removed == 2);
    CHECK(graph_edge_count(g) == 2);
    const graph_edge *edges = NULL;
    size_t count = 0;
    CHECK_STATUS(graph_out_edges(g, 0, &edges, &count), GRAPH_OK);
    CHECK(count == 2 && edges[0].type == EDGE_SIMILAR && edges[1].target == 2); /* order kept */

    CHECK_STATUS(graph_remove_edges(g, 0, 1, EDGE_PREREQ, &removed), GRAPH_ERR_NOT_FOUND);
    CHECK(removed == 0);
    CHECK_STATUS(graph_remove_edges(g, 1, 0, EDGE_SIMILAR, NULL), GRAPH_ERR_NOT_FOUND);
    CHECK_STATUS(graph_remove_edges(g, 0, 3, EDGE_SIMILAR, NULL), GRAPH_ERR_NOT_FOUND);
    CHECK_STATUS(graph_remove_edges(g, 0, 1, EDGE_TYPE_COUNT, NULL), GRAPH_ERR_INVALID_ARG);
    CHECK_STATUS(graph_remove_edges(NULL, 0, 1, EDGE_SIMILAR, NULL), GRAPH_ERR_NULL_ARG);

    CHECK_STATUS(graph_remove_edges(g, 0, 1, EDGE_SIMILAR, NULL), GRAPH_OK);
    CHECK_STATUS(graph_remove_edges(g, 0, 2, EDGE_PREREQ, NULL), GRAPH_OK);
    CHECK(graph_edge_count(g) == 0);
    CHECK_STATUS(graph_out_edges(g, 0, &edges, &count), GRAPH_OK);
    CHECK(count == 0 && edges == NULL);

    /* Edges can be added again after the list was emptied. */
    CHECK_STATUS(graph_add_edge(g, 0, 1, EDGE_CUSTOM, NULL), GRAPH_OK);
    CHECK(graph_edge_count(g) == 1);
    graph_free(g);
}

static void test_names(void)
{
    CHECK(strcmp(edge_type_str(EDGE_PREREQ), "PREREQ") == 0);
    CHECK(strcmp(edge_type_str(EDGE_SIMILAR), "SIMILAR") == 0);
    CHECK(strcmp(edge_type_str(EDGE_ENABLES), "ENABLES") == 0);
    CHECK(strcmp(edge_type_str(EDGE_CONFLICTS), "CONFLICTS") == 0);
    CHECK(strcmp(edge_type_str(EDGE_CUSTOM), "CUSTOM") == 0);
    CHECK(strcmp(edge_type_str(EDGE_TYPE_COUNT), "UNKNOWN") == 0);

    CHECK(strcmp(node_status_str(NODE_STATUS_COMPLETED), "completed") == 0);
    CHECK(strcmp(node_status_str(NODE_STATUS_COUNT), "unknown") == 0);

    CHECK(strcmp(graph_status_str(GRAPH_OK), "ok") == 0);
    CHECK(strcmp(graph_status_str(GRAPH_ERR_DUPLICATE_ID), "duplicate node id") == 0);
}

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } const tests[] = {
        {"create_and_free", test_create_and_free},
        {"add_node_and_get", test_add_node_and_get},
        {"add_node_copies_strings_and_defaults", test_add_node_copies_strings_and_defaults},
        {"add_node_rejects_bad_input", test_add_node_rejects_bad_input},
        {"find_node", test_find_node},
        {"get_node_rejects_bad_input", test_get_node_rejects_bad_input},
        {"add_edge_and_out_edges", test_add_edge_and_out_edges},
        {"multigraph_all_edge_types", test_multigraph_all_edge_types},
        {"add_edge_rejects_bad_input", test_add_edge_rejects_bad_input},
        {"growth", test_growth},
        {"update_node", test_update_node},
        {"remove_node", test_remove_node},
        {"remove_edges", test_remove_edges},
        {"names", test_names},
    };

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before = g_failures;
        tests[i].fn();
        printf("  %-40s %s\n", tests[i].name, g_failures == before ? "ok" : "FAILED");
    }

    printf("test_graph: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
