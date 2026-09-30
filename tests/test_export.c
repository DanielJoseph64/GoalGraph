#include "export.h"

#include "graph.h"
#include "persist.h"

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

#define SCRATCH_DOT "build/test_export_scratch.dot"

/* ------------------------------------------------------------ helpers -- */

/* Reads the whole stream from the start into a new string (caller frees). */
static char *read_all(FILE *f)
{
    if (fflush(f) != 0 || fseek(f, 0, SEEK_END) != 0) {
        return NULL;
    }
    const long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        return NULL;
    }
    char *text = malloc((size_t)size + 1);
    if (text == NULL) {
        return NULL;
    }
    const size_t got = fread(text, 1, (size_t)size, f);
    text[got] = '\0';
    return text;
}

/* Exports g with opts and returns the DOT text (caller frees), or NULL on error. */
static char *export_text(const graph *g, const export_dot_options *opts, export_status *status)
{
    FILE *f = tmpfile();
    CHECK(f != NULL);
    if (f == NULL) {
        return NULL;
    }
    *status = export_dot(g, f, opts);
    char *text = read_all(f);
    CHECK(fclose(f) == 0);
    return text;
}

static bool contains(const char *text, const char *needle)
{
    return text != NULL && strstr(text, needle) != NULL;
}

static size_t count_of(const char *text, const char *needle)
{
    size_t n = 0;
    const size_t len = strlen(needle);
    for (const char *p = text; p != NULL && (p = strstr(p, needle)) != NULL; p += len) {
        n++;
    }
    return n;
}

static size_t add(graph *g, const char *id, const char *category, node_status status)
{
    const graph_node_spec spec = {
        .id = id, .category = category, .status = status, .effort = 3, .enjoyment = 8,
        .created_at = 1, .updated_at = 1};
    size_t index = SIZE_MAX;
    CHECK_STATUS(graph_add_node(g, &spec, &index), GRAPH_OK);
    return index;
}

static graph *make_graph(void)
{
    graph *g = graph_create();
    if (g == NULL) {
        exit(1);
    }
    add(g, "intro", "year1", NODE_STATUS_COMPLETED);                  /* n0 */
    add(g, "data \"structures\"\\x", "year1", NODE_STATUS_IN_PROGRESS); /* n1 */
    add(g, "algorithms\nline2", "year2", NODE_STATUS_NOT_STARTED);    /* n2 */
    add(g, "dropped", "", NODE_STATUS_ABANDONED);                     /* n3 */
    CHECK_STATUS(graph_add_edge(g, 0, 1, EDGE_PREREQ, NULL), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, 1, 2, EDGE_ENABLES, NULL), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, 2, 1, EDGE_SIMILAR, NULL), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, 3, 0, EDGE_CONFLICTS, NULL), GRAPH_OK);
    CHECK_STATUS(graph_add_edge(g, 0, 2, EDGE_CUSTOM, NULL), GRAPH_OK);
    return g;
}

/* ---------------------------------------------------------------- tests -- */

static void test_basic_output(void)
{
    graph *g = make_graph();
    export_status st = EXPORT_ERR_IO;
    char *dot = export_text(g, NULL, &st);
    CHECK_STATUS(st, EXPORT_OK);
    CHECK(dot != NULL);

    CHECK(strncmp(dot, "digraph goalgraph {\n", 20) == 0);
    CHECK(contains(dot, "}\n"));
    /* Every node is declared once, with escaped labels. */
    CHECK(contains(dot, "n0 [label=\"intro\\ncompleted | effort 3 | enjoy 8\""));
    CHECK(contains(dot, "n1 [label=\"data \\\"structures\\\"\\\\x\\nin_progress"));
    CHECK(contains(dot, "n2 [label=\"algorithms\\nline2\\nnot_started"));
    CHECK(!contains(dot, "algorithms\nline2")); /* raw newline must not leak */
    CHECK(contains(dot, "fillcolor=\"#c8e6c9\"")); /* completed */
    CHECK(contains(dot, "fillcolor=\"#fff3b0\"")); /* in progress */
    CHECK(contains(dot, "style=\"rounded,filled,dashed\"")); /* abandoned */

    /* Every edge appears with its type's style. */
    CHECK(count_of(dot, " -> ") == 5);
    CHECK(contains(dot, "n0 -> n1 [color=\"#37474f\", tooltip=\"PREREQ\"]"));
    CHECK(contains(dot, "n1 -> n2 [color=\"#1e88e5\", tooltip=\"ENABLES\"]"));
    CHECK(contains(dot, "n2 -> n1 [color=\"#9e9e9e\", style=dashed, arrowhead=none"));
    CHECK(contains(dot, "n3 -> n0 [color=\"#e53935\", style=bold, arrowhead=tee"));
    CHECK(contains(dot, "n0 -> n2 [color=\"#8e24aa\", style=dotted"));

    /* No options: no clusters, no highlight, no title. */
    CHECK(!contains(dot, "subgraph"));
    CHECK(!contains(dot, "penwidth"));
    CHECK(!contains(dot, "labelloc"));
    free(dot);
    graph_free(g);
}

static void test_clusters_title_highlight(void)
{
    graph *g = make_graph();
    const size_t path[] = {0, 1, 2};
    const export_dot_options opts = {
        .cluster_by_category = true,
        .highlight = path,
        .highlight_count = 3,
        .title = "My \"plan\"",
    };
    export_status st = EXPORT_ERR_IO;
    char *dot = export_text(g, &opts, &st);
    CHECK_STATUS(st, EXPORT_OK);

    /* Two categories -> two clusters; the empty category gets none. */
    CHECK(count_of(dot, "subgraph cluster_") == 2);
    CHECK(contains(dot, "subgraph cluster_0 {\n    label=\"year1\";"));
    CHECK(contains(dot, "    n0;\n    n1;\n  }"));
    CHECK(contains(dot, "subgraph cluster_2 {\n    label=\"year2\";"));
    CHECK(!contains(dot, "    n3;\n"));
    CHECK(contains(dot, "labelloc=t, label=\"My \\\"plan\\\"\""));

    /* Path nodes and path steps are highlighted; others are not. */
    CHECK(count_of(dot, "penwidth=3") == 5); /* 3 nodes + 2 steps */
    CHECK(contains(dot, "n0 -> n1 [color=\"#37474f\", tooltip=\"PREREQ\", color=\"#ff6f00\""));
    CHECK(contains(dot, "n1 -> n2 [color=\"#1e88e5\", tooltip=\"ENABLES\", color=\"#ff6f00\""));
    CHECK(contains(dot, "n2 -> n1 [color=\"#9e9e9e\", style=dashed, arrowhead=none, "
                        "tooltip=\"SIMILAR\"];"));
    free(dot);
    graph_free(g);
}

static void test_empty_and_errors(void)
{
    graph *g = graph_create();
    CHECK(g != NULL);
    export_status st = EXPORT_ERR_IO;
    char *dot = export_text(g, NULL, &st);
    CHECK_STATUS(st, EXPORT_OK);
    CHECK(contains(dot, "digraph goalgraph {") && contains(dot, "}\n"));
    CHECK(!contains(dot, " -> "));
    free(dot);

    CHECK_STATUS(export_dot(NULL, stdout, NULL), EXPORT_ERR_NULL_ARG);
    CHECK_STATUS(export_dot(g, NULL, NULL), EXPORT_ERR_NULL_ARG);
    CHECK_STATUS(export_dot_file(g, NULL, NULL), EXPORT_ERR_NULL_ARG);
    CHECK_STATUS(export_dot_file(NULL, SCRATCH_DOT, NULL), EXPORT_ERR_NULL_ARG);
    graph_free(g);

    /* Out-of-range highlight is rejected before anything is written. */
    g = make_graph();
    const size_t bad[] = {0, 9};
    const export_dot_options bad_opts = {.highlight = bad, .highlight_count = 2};
    dot = export_text(g, &bad_opts, &st);
    CHECK_STATUS(st, EXPORT_ERR_INVALID_ARG);
    CHECK(dot != NULL && dot[0] == '\0');
    free(dot);
    const export_dot_options null_opts = {.highlight = NULL, .highlight_count = 1};
    CHECK_STATUS(export_dot(g, stdout, &null_opts), EXPORT_ERR_NULL_ARG);

    /* Unwritable stream and unwritable path. */
    FILE *read_only = fopen("data/sample_goals.dat", "rb");
    CHECK(read_only != NULL);
    if (read_only != NULL) {
        CHECK_STATUS(export_dot(g, read_only, NULL), EXPORT_ERR_IO);
        CHECK(fclose(read_only) == 0);
    }
    CHECK_STATUS(export_dot_file(g, "build/no-such-dir/out.dot", NULL), EXPORT_ERR_IO);

    CHECK(strcmp(export_status_str(EXPORT_OK), "ok") == 0);
    CHECK(strcmp(export_status_str(EXPORT_ERR_IO), "i/o error") == 0);
    CHECK(strcmp(export_status_str((export_status)77), "unknown") == 0);
    graph_free(g);
}

static void test_file_export_of_sample(void)
{
    graph *g = NULL;
    CHECK_STATUS(persist_load("data/sample_goals.dat", &g, NULL), PERSIST_OK);
    if (g == NULL) {
        return;
    }
    const export_dot_options opts = {.cluster_by_category = true};
    CHECK_STATUS(export_dot_file(g, SCRATCH_DOT, &opts), EXPORT_OK);

    FILE *f = fopen(SCRATCH_DOT, "rb");
    CHECK(f != NULL);
    if (f != NULL) {
        char *dot = read_all(f);
        CHECK(fclose(f) == 0);
        CHECK(count_of(dot, " -> ") == graph_edge_count(g));
        CHECK(count_of(dot, " [label=") == graph_node_count(g));
        CHECK(contains(dot, "label=\"year4-spring\""));
        free(dot);
    }
    CHECK(remove(SCRATCH_DOT) == 0);
    graph_free(g);
}

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } const tests[] = {
        {"basic_output", test_basic_output},
        {"clusters_title_highlight", test_clusters_title_highlight},
        {"empty_and_errors", test_empty_and_errors},
        {"file_export_of_sample", test_file_export_of_sample},
    };

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before = g_failures;
        tests[i].fn();
        printf("  %-40s %s\n", tests[i].name, g_failures == before ? "ok" : "FAILED");
    }

    printf("test_export: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
