#include "persist.h"

#include "algo.h"
#include "graph.h"

#include <float.h>
#include <stdbool.h>
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

#define SAMPLE_PATH "data/sample_goals.dat"
#define SAMPLE_NODES 36
#define SAMPLE_EDGES 55
#define SCRATCH_PATH "build/test_persist_scratch.gg"
#define SCRATCH_TMP_PATH SCRATCH_PATH ".tmp"

/* ------------------------------------------------------------ helpers -- */

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    CHECK(fclose(f) == 0);
    return true;
}

static void remove_if_exists(const char *path)
{
    if (file_exists(path)) {
        CHECK(remove(path) == 0);
    }
}

/* Exact comparison of two float bit patterns (distinguishes -0.0 from 0.0). */
static bool same_float(float a, float b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}

static bool same_weights(const edge_weights *a, const edge_weights *b)
{
    return same_float(a->effort, b->effort) && same_float(a->enjoyment, b->enjoyment) &&
           same_float(a->achievability, b->achievability) &&
           same_float(a->similarity, b->similarity) && same_float(a->time_cost, b->time_cost);
}

/* Checks that a and b have identical nodes (by index) and identical edge lists. */
static void check_graphs_equal(const graph *a, const graph *b)
{
    CHECK(a != NULL && b != NULL);
    if (a == NULL || b == NULL) {
        return;
    }
    CHECK(graph_node_count(a) == graph_node_count(b));
    CHECK(graph_edge_count(a) == graph_edge_count(b));
    if (graph_node_count(a) != graph_node_count(b)) {
        return;
    }

    for (size_t i = 0; i < graph_node_count(a); i++) {
        graph_node_info x = {0};
        graph_node_info y = {0};
        CHECK_STATUS(graph_get_node(a, i, &x), GRAPH_OK);
        CHECK_STATUS(graph_get_node(b, i, &y), GRAPH_OK);
        CHECK(strcmp(x.id, y.id) == 0);
        CHECK(strcmp(x.category, y.category) == 0);
        CHECK(x.status == y.status);
        CHECK(x.effort == y.effort);
        CHECK(x.enjoyment == y.enjoyment);
        CHECK(x.created_at == y.created_at);
        CHECK(x.updated_at == y.updated_at);

        const graph_edge *ea = NULL;
        const graph_edge *eb = NULL;
        size_t na = 0;
        size_t nb = 0;
        CHECK_STATUS(graph_out_edges(a, i, &ea, &na), GRAPH_OK);
        CHECK_STATUS(graph_out_edges(b, i, &eb, &nb), GRAPH_OK);
        CHECK(na == nb);
        for (size_t k = 0; k < na && k < nb; k++) {
            CHECK(ea[k].source == eb[k].source);
            CHECK(ea[k].target == eb[k].target);
            CHECK(ea[k].type == eb[k].type);
            CHECK(same_weights(&ea[k].weights, &eb[k].weights));
        }
    }
}

static size_t add_node(graph *g, const char *id, const char *category, node_status status,
                       int effort, int enjoyment, time_t created, time_t updated)
{
    const graph_node_spec spec = {
        .id = id,
        .category = category,
        .status = status,
        .effort = effort,
        .enjoyment = enjoyment,
        .created_at = created,
        .updated_at = updated,
    };
    size_t index = SIZE_MAX;
    CHECK_STATUS(graph_add_node(g, &spec, &index), GRAPH_OK);
    return index;
}

/* A graph exercising awkward strings, every status/type, and awkward floats. */
static graph *make_tricky_graph(void)
{
    graph *g = graph_create();
    if (g == NULL) {
        fprintf(stderr, "graph_create failed\n");
        exit(1);
    }
    const size_t a = add_node(g, "plain", "cat", NODE_STATUS_NOT_STARTED, 1, 10, 1, 2);
    const size_t b = add_node(g, "with space and \"quotes\" and \\backslash\\", "",
                              NODE_STATUS_IN_PROGRESS, 10, 1, 1700000000, 1700000001);
    const size_t c = add_node(g, "tab\there\nnewline\rcr\x01\x1f\x7f", "multi word category",
                              NODE_STATUS_COMPLETED, 5, 5, -86400, 4102444800);
    const size_t d = add_node(g, "utf8-\xc3\xa9\xe2\x9c\x93", "#not-a-comment",
                              NODE_STATUS_ABANDONED, 7, 3, 1, 1);

    const edge_weights awkward = {
        .effort = 0.1f,
        .enjoyment = -3.40282347e38f,
        .achievability = FLT_MIN,
        .similarity = 1e-45f, /* subnormal */
        .time_cost = -0.0f,
    };
    const edge_weights plain = {.effort = 1.0f, .enjoyment = 123456.789f, .time_cost = 15.0f};

    CHECK_STATUS(graph_add_edge(g, a, b, EDGE_PREREQ, &awkward), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, a, b, EDGE_PREREQ, &plain), GRAPH_OK); /* parallel */
    CHECK_STATUS(graph_add_edge(g, b, c, EDGE_SIMILAR, NULL), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, c, d, EDGE_ENABLES, &plain), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, d, a, EDGE_CONFLICTS, &awkward), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, c, a, EDGE_CUSTOM, &plain), GRAPH_OK);
    return g;
}

/* Parses text (of length len) through persist_read via a temporary stream. */
static persist_status read_text(const char *text, size_t len, graph **out, persist_error *err)
{
    FILE *f = tmpfile();
    CHECK(f != NULL);
    if (f == NULL) {
        *out = NULL;
        return PERSIST_ERR_IO;
    }
    CHECK(fwrite(text, 1, len, f) == len);
    rewind(f);
    const persist_status status = persist_read(f, out, err);
    CHECK(fclose(f) == 0);
    return status;
}

/* Writes g to a temporary stream and reads it back. */
static graph *roundtrip_stream(const graph *g)
{
    FILE *f = tmpfile();
    CHECK(f != NULL);
    if (f == NULL) {
        return NULL;
    }
    persist_error err = {0};
    CHECK_STATUS(persist_write(g, f, &err), PERSIST_OK);
    rewind(f);
    graph *copy = NULL;
    CHECK_STATUS(persist_read(f, &copy, &err), PERSIST_OK);
    if (err.status != PERSIST_OK) {
        fprintf(stderr, "  line %zu: %s\n", err.line, err.message);
    }
    CHECK(fclose(f) == 0);
    return copy;
}

/* ---------------------------------------------------------------- tests -- */

static void test_roundtrip_stream(void)
{
    graph *g = make_tricky_graph();
    graph *copy = roundtrip_stream(g);
    check_graphs_equal(g, copy);

    /* A second roundtrip is also identical (the format is stable). */
    graph *copy2 = roundtrip_stream(copy);
    check_graphs_equal(g, copy2);

    graph_free(copy2);
    graph_free(copy);
    graph_free(g);
}

static void test_roundtrip_empty(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);
    graph *copy = roundtrip_stream(g);
    CHECK(copy != NULL);
    CHECK(graph_node_count(copy) == 0 && graph_edge_count(copy) == 0);
    graph_free(copy);
    graph_free(g);
}

static void test_save_and_load_file(void)
{
    remove_if_exists(SCRATCH_PATH);
    remove_if_exists(SCRATCH_TMP_PATH);

    graph *g = make_tricky_graph();
    persist_error err = {0};
    CHECK_STATUS(persist_save(g, SCRATCH_PATH, &err), PERSIST_OK);
    CHECK(err.status == PERSIST_OK && err.message[0] == '\0');
    CHECK(file_exists(SCRATCH_PATH));
    CHECK(!file_exists(SCRATCH_TMP_PATH)); /* temp file was renamed away */

    graph *loaded = NULL;
    CHECK_STATUS(persist_load(SCRATCH_PATH, &loaded, &err), PERSIST_OK);
    check_graphs_equal(g, loaded);
    graph_free(loaded);

    /* Saving over an existing file replaces it. */
    graph *empty = graph_create();
    CHECK(empty != NULL);
    CHECK_STATUS(persist_save(empty, SCRATCH_PATH, NULL), PERSIST_OK);
    CHECK_STATUS(persist_load(SCRATCH_PATH, &loaded, NULL), PERSIST_OK);
    CHECK(loaded != NULL && graph_node_count(loaded) == 0);
    graph_free(loaded);

    /* A failed save leaves the existing file untouched and no temp file. */
    CHECK_STATUS(persist_save(NULL, SCRATCH_PATH, &err), PERSIST_ERR_NULL_ARG);
    CHECK_STATUS(persist_load(SCRATCH_PATH, &loaded, NULL), PERSIST_OK);
    CHECK(loaded != NULL && graph_node_count(loaded) == 0);
    graph_free(loaded);
    CHECK(!file_exists(SCRATCH_TMP_PATH));

    graph_free(empty);
    graph_free(g);
    remove_if_exists(SCRATCH_PATH);
}

static void test_file_errors(void)
{
    graph *g = make_tricky_graph();
    persist_error err = {0};

    CHECK_STATUS(persist_save(g, "build/no-such-directory/goals.gg", &err), PERSIST_ERR_IO);
    CHECK(err.status == PERSIST_ERR_IO && err.message[0] != '\0');
    CHECK(!file_exists("build/no-such-directory/goals.gg.tmp"));

    graph *loaded = g; /* must be overwritten with NULL */
    CHECK_STATUS(persist_load("data/does-not-exist.dat", &loaded, &err), PERSIST_ERR_IO);
    CHECK(loaded == NULL);
    CHECK(err.status == PERSIST_ERR_IO && err.message[0] != '\0');

    /* Write errors on a stream are reported (this stream is read-only). */
    FILE *read_only = fopen(SAMPLE_PATH, "rb");
    CHECK(read_only != NULL);
    if (read_only != NULL) {
        CHECK_STATUS(persist_write(g, read_only, &err), PERSIST_ERR_IO);
        CHECK(fclose(read_only) == 0);
    }

    graph_free(g);
}

static void test_null_args(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);
    graph *out = NULL;
    persist_error err = {0};

    CHECK_STATUS(persist_write(NULL, stdout, &err), PERSIST_ERR_NULL_ARG);
    CHECK(err.status == PERSIST_ERR_NULL_ARG);
    CHECK_STATUS(persist_write(g, NULL, NULL), PERSIST_ERR_NULL_ARG);
    CHECK_STATUS(persist_read(NULL, &out, NULL), PERSIST_ERR_NULL_ARG);
    CHECK(out == NULL);
    CHECK_STATUS(persist_read(stdin, NULL, NULL), PERSIST_ERR_NULL_ARG);
    CHECK_STATUS(persist_save(g, NULL, NULL), PERSIST_ERR_NULL_ARG);
    CHECK_STATUS(persist_load(NULL, &out, NULL), PERSIST_ERR_NULL_ARG);
    CHECK_STATUS(persist_load(SAMPLE_PATH, NULL, NULL), PERSIST_ERR_NULL_ARG);

    CHECK(strcmp(persist_status_str(PERSIST_OK), "ok") == 0);
    CHECK(strcmp(persist_status_str(PERSIST_ERR_FORMAT), "malformed input") == 0);
    CHECK(strcmp(persist_status_str((persist_status)42), "unknown") == 0);
    graph_free(g);
}

static void test_lenient_syntax(void)
{
    /* CRLF endings, comments, blank lines, tabs, indentation, no final newline. */
    const char text[] = "\r\n# comment before header\r\n"
                        "  goalgraph\t1  \r\n"
                        "\n"
                        "\tnode \"a\" \"\" not_started 1 10 5 6\r\n"
                        "   # indented comment\n"
                        "node\t\"b\"\t\"x y\"\tcompleted\t10\t1\t-5\t0\n"
                        "edge \"a\" \"b\" CUSTOM 1 -2.5 1e3 0x1p-2 7";
    graph *g = NULL;
    persist_error err = {0};
    CHECK_STATUS(read_text(text, sizeof text - 1, &g, &err), PERSIST_OK);
    CHECK(g != NULL);
    if (g == NULL) {
        fprintf(stderr, "  line %zu: %s\n", err.line, err.message);
        return;
    }
    CHECK(graph_node_count(g) == 2 && graph_edge_count(g) == 1);

    graph_node_info info = {0};
    CHECK_STATUS(graph_get_node(g, 1, &info), GRAPH_OK);
    CHECK(strcmp(info.id, "b") == 0 && strcmp(info.category, "x y") == 0);
    CHECK(info.created_at == -5);
    CHECK(info.updated_at == -5); /* 0 means "same as created_at" */

    const graph_edge *edges = NULL;
    size_t count = 0;
    CHECK_STATUS(graph_out_edges(g, 0, &edges, &count), GRAPH_OK);
    CHECK(count == 1 && edges[0].type == EDGE_CUSTOM);
    CHECK(edges[0].weights.enjoyment == -2.5f && edges[0].weights.achievability == 1000.0f);
    CHECK(edges[0].weights.similarity == 0.25f && edges[0].weights.time_cost == 7.0f);
    graph_free(g);
}

typedef struct bad_case {
    const char *text;
    persist_status status;
    size_t line;
} bad_case;

#define H "goalgraph 1\n"
#define NODE_A "node \"a\" \"c\" not_started 5 5 1 1\n"
#define NODE_B "node \"b\" \"c\" not_started 5 5 1 1\n"

static void test_malformed_input(void)
{
    static const bad_case cases[] = {
        {"", PERSIST_ERR_FORMAT, 0},
        {"# only a comment\n\n", PERSIST_ERR_FORMAT, 2},
        {"goalgraph 2\n", PERSIST_ERR_VERSION, 1},
        {"goalgraph 0\n", PERSIST_ERR_VERSION, 1},
        {"goalgraph\n", PERSIST_ERR_FORMAT, 1},
        {"goalgraph one\n", PERSIST_ERR_FORMAT, 1},
        {"goalgraph 1 extra\n", PERSIST_ERR_FORMAT, 1},
        {"graph 1\n", PERSIST_ERR_FORMAT, 1},
        {NODE_A H, PERSIST_ERR_FORMAT, 1},
        {H "vertex \"a\"\n", PERSIST_ERR_FORMAT, 2},
        /* node records */
        {H "node\n", PERSIST_ERR_FORMAT, 2},
        {H "node a \"c\" not_started 5 5 1 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\"\"c\" not_started 5 5 1 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"\" \"c\" not_started 5 5 1 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\\q\" \"c\" not_started 5 5 1 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\\x4\" \"c\" not_started 5 5 1 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\\x00\" \"c\" not_started 5 5 1 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\" \"c\" finished 5 5 1 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\" \"c\" not_started 0 5 1 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\" \"c\" not_started 5 11 1 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\" \"c\" not_started 5x 5 1 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\" \"c\" not_started 5 5 1.5 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\" \"c\" not_started 5 5 99999999999999999999 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\" \"c\" not_started 5 5 1\n", PERSIST_ERR_FORMAT, 2},
        {H "node \"a\" \"c\" not_started 5 5 1 1 extra\n", PERSIST_ERR_FORMAT, 2},
        {H NODE_A "\n" NODE_A, PERSIST_ERR_FORMAT, 4},
        /* edge records */
        {H NODE_A NODE_B "edge \"a\" \"z\" PREREQ 0 0 0 0 0\n", PERSIST_ERR_FORMAT, 4},
        {H NODE_A NODE_B "edge \"z\" \"b\" PREREQ 0 0 0 0 0\n", PERSIST_ERR_FORMAT, 4},
        {H NODE_A NODE_B "edge \"a\" \"a\" PREREQ 0 0 0 0 0\n", PERSIST_ERR_FORMAT, 4},
        {H NODE_A NODE_B "edge \"a\" \"b\" REQUIRES 0 0 0 0 0\n", PERSIST_ERR_FORMAT, 4},
        {H NODE_A NODE_B "edge \"a\" \"b\" PREREQ 0 0 0 0\n", PERSIST_ERR_FORMAT, 4},
        {H NODE_A NODE_B "edge \"a\" \"b\" PREREQ 0 0 0 0 0 0\n", PERSIST_ERR_FORMAT, 4},
        {H NODE_A NODE_B "edge \"a\" \"b\" PREREQ nan 0 0 0 0\n", PERSIST_ERR_FORMAT, 4},
        {H NODE_A NODE_B "edge \"a\" \"b\" PREREQ 0 inf 0 0 0\n", PERSIST_ERR_FORMAT, 4},
        {H NODE_A NODE_B "edge \"a\" \"b\" PREREQ 0 0 1e999 0 0\n", PERSIST_ERR_FORMAT, 4},
        {H NODE_A NODE_B "edge \"a\" \"b\" PREREQ 0 0 0 1,5 0\n", PERSIST_ERR_FORMAT, 4},
        {H "edge \"a\" \"b\" PREREQ 0 0 0 0 0\n" NODE_A NODE_B, PERSIST_ERR_FORMAT, 2},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        graph *g = NULL;
        persist_error err = {0};
        const persist_status status =
            read_text(cases[i].text, strlen(cases[i].text), &g, &err);
        CHECK(g == NULL);
        CHECK(err.status == status);
        if (status != cases[i].status || err.line != cases[i].line) {
            fprintf(stderr, "  case %zu: got %s at line %zu (%s)\n", i,
                    persist_status_str(status), err.line, err.message);
        }
        CHECK(status == cases[i].status);
        CHECK(err.line == cases[i].line);
        CHECK(err.message[0] != '\0');
        graph_free(g);
    }
}

static void test_nul_and_long_lines(void)
{
    const char with_nul[] = H "node \"a\0\" \"c\" not_started 5 5 1 1\n";
    graph *g = NULL;
    persist_error err = {0};
    CHECK_STATUS(read_text(with_nul, sizeof with_nul - 1, &g, &err), PERSIST_ERR_FORMAT);
    CHECK(g == NULL && err.line == 2);

    /* A comment line one byte over the limit. */
    const size_t len = strlen(H) + 1 + PERSIST_MAX_LINE + 1;
    char *long_text = malloc(len + 1);
    CHECK(long_text != NULL);
    if (long_text == NULL) {
        return;
    }
    strcpy(long_text, H);
    memset(long_text + strlen(H), '#', len - strlen(H) - 1);
    long_text[len - 1] = '\n';
    long_text[len] = '\0';
    CHECK_STATUS(read_text(long_text, len, &g, &err), PERSIST_ERR_FORMAT);
    CHECK(g == NULL && err.line == 2);

    /* Exactly at the limit is fine. */
    long_text[len - 2] = '\n';
    CHECK_STATUS(read_text(long_text, len - 1, &g, &err), PERSIST_OK);
    CHECK(g != NULL && graph_node_count(g) == 0);
    graph_free(g);
    free(long_text);
}

static size_t find(const graph *g, const char *id)
{
    size_t index = SIZE_MAX;
    CHECK_STATUS(graph_find_node(g, id, &index), GRAPH_OK);
    return index;
}

static void test_sample_data(void)
{
    graph *g = NULL;
    persist_error err = {0};
    CHECK_STATUS(persist_load(SAMPLE_PATH, &g, &err), PERSIST_OK);
    if (g == NULL) {
        fprintf(stderr, "  %s:%zu: %s\n", SAMPLE_PATH, err.line, err.message);
        return;
    }
    CHECK(graph_node_count(g) == SAMPLE_NODES);
    CHECK(graph_edge_count(g) == SAMPLE_EDGES);

    graph_node_info info = {0};
    CHECK_STATUS(graph_get_node(g, find(g, "cs301-algorithms"), &info), GRAPH_OK);
    CHECK(info.status == NODE_STATUS_IN_PROGRESS && strcmp(info.category, "year3-fall") == 0);
    CHECK(info.effort == 9 && info.enjoyment == 8);
    CHECK_STATUS(graph_get_node(g, find(g, "proj-weekend-hackathon"), &info), GRAPH_OK);
    CHECK(info.status == NODE_STATUS_ABANDONED);

    /* The prerequisite tree is a DAG and respects course order. */
    algo_order order = {0};
    CHECK_STATUS(algo_topo_sort(g, algo_edge_bit(EDGE_PREREQ), &order), ALGO_OK);
    CHECK(order.count == SAMPLE_NODES);
    size_t pos_101 = SIZE_MAX;
    size_t pos_491 = SIZE_MAX;
    for (size_t i = 0; i < order.count; i++) {
        if (order.nodes[i] == find(g, "cs101-intro-programming")) {
            pos_101 = i;
        }
        if (order.nodes[i] == find(g, "cs491-capstone-project-2")) {
            pos_491 = i;
        }
    }
    CHECK(pos_101 < pos_491);
    algo_order_free(&order);

    /* SIMILAR / CONFLICTS pairs point both ways, so all edges together cycle. */
    CHECK_STATUS(algo_topo_sort(g, ALGO_EDGES_ALL, &order), ALGO_ERR_CYCLE);
    algo_order_free(&order);

    /* Recommendations: ready goals appear, blocked and finished goals do not. */
    algo_rec_list recs = {0};
    CHECK_STATUS(algo_recommend(g, NULL, &recs), ALGO_OK);
    bool has_networks = false;
    bool has_internship = false;
    bool has_completed = false;
    for (size_t i = 0; i < recs.count; i++) {
        const size_t n = recs.items[i].node;
        has_networks |= n == find(g, "cs330-computer-networks");
        has_internship |= n == find(g, "career-summer-internship-2027");
        CHECK_STATUS(graph_get_node(g, n, &info), GRAPH_OK);
        has_completed |= info.status == NODE_STATUS_COMPLETED;
    }
    CHECK(has_networks && !has_internship && !has_completed);
    algo_rec_list_free(&recs);

    /* The sample survives a save/load roundtrip. */
    graph *copy = roundtrip_stream(g);
    check_graphs_equal(g, copy);
    graph_free(copy);
    graph_free(g);
}

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } const tests[] = {
        {"roundtrip_stream", test_roundtrip_stream},
        {"roundtrip_empty", test_roundtrip_empty},
        {"save_and_load_file", test_save_and_load_file},
        {"file_errors", test_file_errors},
        {"null_args", test_null_args},
        {"lenient_syntax", test_lenient_syntax},
        {"malformed_input", test_malformed_input},
        {"nul_and_long_lines", test_nul_and_long_lines},
        {"sample_data", test_sample_data},
    };

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before = g_failures;
        tests[i].fn();
        printf("  %-40s %s\n", tests[i].name, g_failures == before ? "ok" : "FAILED");
    }

    printf("test_persist: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
