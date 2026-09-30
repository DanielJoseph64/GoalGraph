#include "cli.h"

#include <stdbool.h>
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

#define SAMPLE_PATH "data/sample_goals.dat"
#define SCRATCH_GRAPH "build/test_cli_scratch.dat"
#define SCRATCH_DOT "build/test_cli_scratch.dot"

/* ------------------------------------------------------------ harness -- */

/* A session whose output and errors go to temporary files we can inspect. */
typedef struct harness {
    FILE *out;
    FILE *err;
    cli_session *s;
    char *out_text; /* output of the most recent command */
    char *err_text; /* error output of the most recent command */
} harness;

static harness harness_create(void)
{
    harness h = {.out = tmpfile(), .err = tmpfile()};
    if (h.out == NULL || h.err == NULL) {
        fprintf(stderr, "tmpfile failed\n");
        exit(1);
    }
    h.s = cli_session_create(h.out, h.err);
    if (h.s == NULL) {
        fprintf(stderr, "cli_session_create failed\n");
        exit(1);
    }
    return h;
}

static void harness_free(harness *h)
{
    cli_session_free(h->s);
    CHECK(fclose(h->out) == 0);
    CHECK(fclose(h->err) == 0);
    free(h->out_text);
    free(h->err_text);
}

/* Returns everything written to f since position start (caller frees). */
static char *read_since(FILE *f, long start)
{
    if (fflush(f) != 0) {
        return NULL;
    }
    const long end = ftell(f);
    if (end < start || fseek(f, start, SEEK_SET) != 0) {
        return NULL;
    }
    const size_t size = (size_t)(end - start);
    char *text = malloc(size + 1);
    if (text == NULL) {
        return NULL;
    }
    const size_t got = fread(text, 1, size, f);
    text[got] = '\0';
    if (fseek(f, end, SEEK_SET) != 0) {
        free(text);
        return NULL;
    }
    return text;
}

typedef cli_result (*runner)(harness *h, const void *arg);

static cli_result capture(harness *h, runner fn, const void *arg)
{
    const long out_start = ftell(h->out);
    const long err_start = ftell(h->err);
    const cli_result result = fn(h, arg);
    free(h->out_text);
    free(h->err_text);
    h->out_text = read_since(h->out, out_start);
    h->err_text = read_since(h->err, err_start);
    CHECK(h->out_text != NULL && h->err_text != NULL);
    return result;
}

static cli_result run_line_fn(harness *h, const void *arg)
{
    return cli_run_line(h->s, arg);
}

/* Runs one command line, capturing its output. */
static cli_result run(harness *h, const char *line)
{
    return capture(h, run_line_fn, line);
}

static bool out_has(const harness *h, const char *needle)
{
    return h->out_text != NULL && strstr(h->out_text, needle) != NULL;
}

static bool err_has(const harness *h, const char *needle)
{
    return h->err_text != NULL && strstr(h->err_text, needle) != NULL;
}

/* Position of needle in the last output, or SIZE_MAX if absent. */
static size_t out_pos(const harness *h, const char *needle)
{
    const char *p = h->out_text != NULL ? strstr(h->out_text, needle) : NULL;
    return p != NULL ? (size_t)(p - h->out_text) : (size_t)-1;
}

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

/* ---------------------------------------------------------------- tests -- */

static void test_help_and_parsing(void)
{
    harness h = harness_create();

    CHECK(run(&h, "help") == CLI_OK);
    CHECK(out_has(&h, "recommend") && out_has(&h, "export") && out_has(&h, "path"));
    CHECK(run(&h, "HELP link") == CLI_OK); /* command names are case-insensitive */
    CHECK(out_has(&h, "usage: link <from> <to>"));
    CHECK(run(&h, "help nosuch") == CLI_ERROR);

    CHECK(run(&h, "") == CLI_OK);
    CHECK(run(&h, "   # just a comment") == CLI_OK);
    CHECK(h.out_text[0] == '\0' && h.err_text[0] == '\0');

    CHECK(run(&h, "frobnicate") == CLI_ERROR);
    CHECK(err_has(&h, "error: unknown command 'frobnicate'"));
    CHECK(run(&h, "show") == CLI_ERROR); /* too few arguments */
    CHECK(err_has(&h, "usage: show <id>"));
    CHECK(run(&h, "done a b") == CLI_ERROR); /* too many */
    CHECK(run(&h, "add \"unterminated") == CLI_ERROR);
    CHECK(err_has(&h, "unterminated quote"));

    /* Quoting: double quotes with escapes, single quotes literal. */
    CHECK(run(&h, "add \"learn \\\"rust\\\"\" category='side project' # trailing") == CLI_OK);
    CHECK(out_has(&h, "Added goal 'learn \"rust\"'."));
    CHECK(run(&h, "list category='side project'") == CLI_OK);
    CHECK(out_has(&h, "learn \"rust\"") && out_has(&h, "(1 of 1 goals)"));

    CHECK(cli_run_line(NULL, "help") == CLI_ERROR);
    CHECK(cli_run_args(h.s, 0, NULL) == CLI_ERROR);
    harness_free(&h);
}

static void test_goal_editing(void)
{
    harness h = harness_create();
    CHECK(run(&h, "list") == CLI_OK);
    CHECK(out_has(&h, "No goals yet"));

    CHECK(run(&h, "add a category=math effort=3 enjoyment=9 status=in-progress") == CLI_OK);
    CHECK(run(&h, "add b") == CLI_OK);
    CHECK(cli_session_modified(h.s));
    CHECK(run(&h, "add a") == CLI_ERROR);
    CHECK(err_has(&h, "a goal named 'a' already exists"));
    CHECK(run(&h, "add c effort=11") == CLI_ERROR);
    CHECK(err_has(&h, "effort must be an integer from 1 to 10"));
    CHECK(run(&h, "add c status=sleeping") == CLI_ERROR);
    CHECK(run(&h, "add c color=blue") == CLI_ERROR);
    CHECK(err_has(&h, "unknown option 'color'"));
    CHECK(run(&h, "add c effort") == CLI_ERROR);
    CHECK(run(&h, "add ''") == CLI_ERROR);

    CHECK(run(&h, "show a") == CLI_OK);
    CHECK(out_has(&h, "category:  math") && out_has(&h, "status:    in_progress"));
    CHECK(out_has(&h, "effort:    3/10") && out_has(&h, "enjoyment: 9/10"));

    CHECK(run(&h, "edit a status=done effort=4") == CLI_OK);
    CHECK(run(&h, "list status=completed") == CLI_OK);
    CHECK(out_has(&h, "(1 of 2 goals)") && out_has(&h, "completed"));
    CHECK(run(&h, "edit a") == CLI_ERROR);
    CHECK(run(&h, "edit a id=b") == CLI_ERROR);
    CHECK(err_has(&h, "already exists"));
    CHECK(run(&h, "edit a id=alpha") == CLI_OK);
    CHECK(run(&h, "show a") == CLI_ERROR);
    CHECK(err_has(&h, "no goal named 'a'"));
    CHECK(run(&h, "done b") == CLI_OK);
    CHECK(out_has(&h, "Updated goal 'b' (completed)"));

    CHECK(run(&h, "link alpha b") == CLI_OK);
    CHECK(run(&h, "remove alpha") == CLI_OK);
    CHECK(out_has(&h, "Removed goal 'alpha' and 1 edge(s)."));
    CHECK(run(&h, "remove alpha") == CLI_ERROR);
    CHECK(run(&h, "list") == CLI_OK);
    CHECK(out_has(&h, "(1 of 1 goals)"));
    harness_free(&h);
}

static void test_edges(void)
{
    harness h = harness_create();
    CHECK(run(&h, "add a") == CLI_OK);
    CHECK(run(&h, "add b") == CLI_OK);
    CHECK(run(&h, "add c") == CLI_OK);

    CHECK(run(&h, "link a b time=2.5 achievability=0.9") == CLI_OK);
    CHECK(out_has(&h, "Linked a -PREREQ-> b."));
    CHECK(run(&h, "link a c type=similar similarity=0.8") == CLI_OK);
    CHECK(run(&h, "link a b type=Enables") == CLI_OK);
    CHECK(run(&h, "link a a") == CLI_ERROR);
    CHECK(run(&h, "link a zz") == CLI_ERROR);
    CHECK(run(&h, "link a b type=likes") == CLI_ERROR);
    CHECK(run(&h, "link a b time=nan") == CLI_ERROR);
    CHECK(run(&h, "link a b time=1e40") == CLI_ERROR);

    CHECK(run(&h, "edges") == CLI_OK);
    CHECK(out_has(&h, "(3 edge(s))"));
    CHECK(out_has(&h, "-PREREQ-> b") && out_has(&h, "achievability 0.9") &&
          out_has(&h, "time 2.5"));
    CHECK(run(&h, "edges c") == CLI_OK);
    CHECK(out_has(&h, "(1 edge(s))") && out_has(&h, "-SIMILAR-> c"));

    /* A prerequisite cycle is allowed but warned about. */
    CHECK(run(&h, "link b a") == CLI_OK);
    CHECK(err_has(&h, "warning: prerequisites now form a cycle"));

    CHECK(run(&h, "unlink a c") == CLI_ERROR); /* default type prereq: none a->c */
    CHECK(err_has(&h, "no PREREQ edge from 'a' to 'c'"));
    CHECK(run(&h, "unlink a c type=similar") == CLI_OK);
    CHECK(run(&h, "unlink a b type=all") == CLI_OK);
    CHECK(out_has(&h, "Removed 2 edge(s)"));
    CHECK(run(&h, "unlink a b colour=red") == CLI_ERROR);
    CHECK(run(&h, "edges") == CLI_OK);
    CHECK(out_has(&h, "(1 edge(s))"));
    harness_free(&h);
}

static void test_order_and_cycles(void)
{
    harness h = harness_create();
    CHECK(run(&h, "add c status=completed") == CLI_OK);
    CHECK(run(&h, "add b") == CLI_OK);
    CHECK(run(&h, "add a") == CLI_OK);
    CHECK(run(&h, "link c b") == CLI_OK);
    CHECK(run(&h, "link b a") == CLI_OK);
    CHECK(run(&h, "link a c type=similar") == CLI_OK);

    CHECK(run(&h, "order") == CLI_OK);
    CHECK(out_has(&h, "following PREREQ edges"));
    CHECK(out_pos(&h, "1. c") < out_pos(&h, "2. b"));
    CHECK(out_pos(&h, "2. b") < out_pos(&h, "3. a"));
    CHECK(run(&h, "order remaining") == CLI_OK);
    CHECK(out_has(&h, "1. b") && !out_has(&h, ". c "));
    CHECK(run(&h, "order remaining=yes") == CLI_ERROR);

    /* All edge types: the SIMILAR edge closes a cycle. */
    CHECK(run(&h, "order types=all") == CLI_ERROR);
    CHECK(err_has(&h, "contain a cycle") && err_has(&h, " -> "));
    CHECK(run(&h, "order types=prereq,similar") == CLI_ERROR);
    CHECK(run(&h, "order types=prereq,,similar") == CLI_ERROR);

    CHECK(run(&h, "cycles types=prereq") == CLI_OK);
    CHECK(out_has(&h, "No cycles among PREREQ edges."));
    CHECK(run(&h, "cycles") == CLI_OK);
    CHECK(out_has(&h, "Cycle group 1 (3 goals): c b a"));
    harness_free(&h);
}

static void test_paths(void)
{
    harness h = harness_create();
    /* a -> b -> d costs 1 + 1 hops but 10 + 10 time; a -> c -> d costs 1 + 1 time. */
    CHECK(run(&h, "add a") == CLI_OK);
    CHECK(run(&h, "add b") == CLI_OK);
    CHECK(run(&h, "add c") == CLI_OK);
    CHECK(run(&h, "add d") == CLI_OK);
    CHECK(run(&h, "add island") == CLI_OK);
    CHECK(run(&h, "link a b time=10") == CLI_OK);
    CHECK(run(&h, "link b d time=10") == CLI_OK);
    CHECK(run(&h, "link a c type=enables time=1") == CLI_OK);
    CHECK(run(&h, "link c d type=enables time=1") == CLI_OK);
    CHECK(run(&h, "link a d type=similar time=0") == CLI_OK);

    CHECK(run(&h, "path a d") == CLI_OK); /* default: hops over prereq,enables */
    CHECK(out_has(&h, "(Dijkstra, PREREQ,ENABLES edges): 2 step(s), total hops 2"));
    CHECK(run(&h, "path a d weight=time") == CLI_OK);
    CHECK(out_has(&h, "total time 2") && out_has(&h, "2. c"));
    CHECK(run(&h, "path a d weight=time astar") == CLI_OK);
    CHECK(out_has(&h, "(A*,") && out_has(&h, "total time 2") && out_has(&h, "2. c"));
    CHECK(run(&h, "path a d types=prereq weight=time") == CLI_OK);
    CHECK(out_has(&h, "total time 20") && out_has(&h, "2. b"));
    CHECK(run(&h, "path a d types=all weight=time") == CLI_OK);
    CHECK(out_has(&h, "1 step(s), total time 0"));

    CHECK(run(&h, "path a island") == CLI_ERROR);
    CHECK(err_has(&h, "no path from 'a' to 'island'"));
    CHECK(run(&h, "path d a") == CLI_ERROR); /* edges are directed */
    CHECK(run(&h, "path a d weight=speed") == CLI_ERROR);
    CHECK(run(&h, "path a d astar=1") == CLI_ERROR);

    /* Negative weights are rejected by the search. */
    CHECK(run(&h, "link b c effort=-1") == CLI_OK);
    CHECK(run(&h, "path b d weight=effort") == CLI_ERROR);
    CHECK(err_has(&h, "effort weights must be non-negative"));
    harness_free(&h);
}

static void test_recommend(void)
{
    harness h = harness_create();
    CHECK(run(&h, "recommend") == CLI_OK);
    CHECK(out_has(&h, "Nothing to recommend"));

    CHECK(run(&h, "add base status=completed") == CLI_OK);
    CHECK(run(&h, "add fun effort=2 enjoyment=10") == CLI_OK);
    CHECK(run(&h, "add chore effort=9 enjoyment=2") == CLI_OK);
    CHECK(run(&h, "add later") == CLI_OK);
    CHECK(run(&h, "add wip status=in_progress") == CLI_OK);
    CHECK(run(&h, "link base fun") == CLI_OK);
    CHECK(run(&h, "link chore later") == CLI_OK);

    CHECK(run(&h, "recommend") == CLI_OK);
    CHECK(out_has(&h, "1. fun"));
    CHECK(!out_has(&h, "later") && !out_has(&h, "base"));
    CHECK(out_pos(&h, "fun") < out_pos(&h, "chore"));
    CHECK(out_has(&h, "(* already in progress)"));

    CHECK(run(&h, "recommend blocked top=0") == CLI_OK);
    CHECK(out_has(&h, "later") && out_has(&h, "blocked: 0/1 prerequisites done"));
    CHECK(run(&h, "recommend top=1") == CLI_OK);
    CHECK(out_has(&h, "Top 1 recommended") && out_has(&h, "1. fun"));
    CHECK(run(&h, "recommend no-in-progress top=0") == CLI_OK);
    CHECK(!out_has(&h, "wip"));
    CHECK(run(&h, "recommend enjoyment=0 effort=0 achievability=0") == CLI_ERROR);
    CHECK(run(&h, "recommend penalty=2") == CLI_ERROR);
    CHECK(run(&h, "recommend top=-1") == CLI_ERROR);
    CHECK(run(&h, "recommend blocked=yes") == CLI_ERROR);
    harness_free(&h);
}

static void test_files(void)
{
    remove_if_exists(SCRATCH_GRAPH);
    harness h = harness_create();

    CHECK(run(&h, "save") == CLI_ERROR);
    CHECK(err_has(&h, "no file name yet"));
    CHECK(run(&h, "load data/does-not-exist.dat") == CLI_ERROR);

    CHECK(run(&h, "load " SAMPLE_PATH) == CLI_OK);
    CHECK(out_has(&h, "Loaded 36 goals and 55 edges"));
    CHECK(!cli_session_modified(h.s));
    CHECK(run(&h, "add my-new-goal") == CLI_OK);

    /* Unsaved changes block load/new unless forced. */
    CHECK(run(&h, "load " SAMPLE_PATH) == CLI_ERROR);
    CHECK(err_has(&h, "unsaved changes"));
    CHECK(run(&h, "new") == CLI_ERROR);
    CHECK(run(&h, "new please") == CLI_ERROR);

    CHECK(run(&h, "save " SCRATCH_GRAPH) == CLI_OK);
    CHECK(out_has(&h, "Saved 37 goals"));
    CHECK(!cli_session_modified(h.s));
    CHECK(!file_exists(SCRATCH_GRAPH ".tmp"));

    CHECK(run(&h, "new") == CLI_OK);
    CHECK(run(&h, "load " SCRATCH_GRAPH) == CLI_OK);
    CHECK(out_has(&h, "Loaded 37 goals"));
    CHECK(run(&h, "edit my-new-goal effort=2") == CLI_OK);
    CHECK(run(&h, "save") == CLI_OK); /* saves back to the loaded file */
    CHECK(out_has(&h, SCRATCH_GRAPH));
    CHECK(run(&h, "add throwaway") == CLI_OK);
    CHECK(run(&h, "load " SCRATCH_GRAPH " force") == CLI_OK);
    CHECK(run(&h, "show throwaway") == CLI_ERROR);
    CHECK(run(&h, "show my-new-goal") == CLI_OK);
    CHECK(out_has(&h, "effort:    2/10"));

    CHECK(run(&h, "save build/no-such-dir/x.dat") == CLI_ERROR);
    CHECK(remove(SCRATCH_GRAPH) == 0);
    harness_free(&h);
}

static void test_export(void)
{
    remove_if_exists(SCRATCH_DOT);
    harness h = harness_create();
    CHECK(run(&h, "load " SAMPLE_PATH) == CLI_OK);

    CHECK(run(&h, "export " SCRATCH_DOT " highlight") == CLI_ERROR);
    CHECK(err_has(&h, "run 'path <from> <to>' first"));
    CHECK(run(&h, "path cs101-intro-programming cs301-algorithms") == CLI_OK);
    CHECK(run(&h, "export " SCRATCH_DOT " cluster highlight title='CS plan'") == CLI_OK);
    CHECK(out_has(&h, "Exported 36 goals and 55 edges"));
    CHECK(file_exists(SCRATCH_DOT));

    /* Editing the graph invalidates the remembered path. */
    CHECK(run(&h, "add extra") == CLI_OK);
    CHECK(run(&h, "export " SCRATCH_DOT " highlight") == CLI_ERROR);
    CHECK(run(&h, "export " SCRATCH_DOT " sparkle") == CLI_ERROR);
    CHECK(run(&h, "export build/no-such-dir/x.dot") == CLI_ERROR);
    CHECK(remove(SCRATCH_DOT) == 0);
    harness_free(&h);
}

typedef struct repl_input {
    const char *script;
    bool interactive;
} repl_input;

static cli_result run_repl_fn(harness *h, const void *arg)
{
    const repl_input *input = arg;
    FILE *in = tmpfile();
    CHECK(in != NULL);
    if (in == NULL) {
        return CLI_ERROR;
    }
    const size_t len = strlen(input->script);
    CHECK(fwrite(input->script, 1, len, in) == len);
    rewind(in);
    const cli_result result = cli_repl(h->s, in, input->interactive);
    CHECK(fclose(in) == 0);
    return result;
}

static cli_result repl(harness *h, const char *script, bool interactive)
{
    const repl_input input = {.script = script, .interactive = interactive};
    return capture(h, run_repl_fn, &input);
}

static void test_repl(void)
{
    harness h = harness_create();

    /* Script mode: no prompt; runs to EOF; warns about unsaved changes. */
    CHECK(repl(&h, "add a\r\nadd b\nlink a b\norder\n", false) == CLI_OK);
    CHECK(!out_has(&h, "goalgraph>"));
    CHECK(out_has(&h, "1. a") && out_has(&h, "2. b"));
    CHECK(err_has(&h, "exiting with unsaved changes"));

    /* Errors are reported but do not stop the script. */
    CHECK(repl(&h, "bogus\nadd c\n", false) == CLI_ERROR);
    CHECK(err_has(&h, "unknown command 'bogus'") && out_has(&h, "Added goal 'c'"));

    /* quit warns once with unsaved changes, then quits; later lines are not run. */
    CHECK(repl(&h, "quit\nquit\nadd never\n", true) == CLI_OK);
    CHECK(out_has(&h, "GoalGraph interactive shell") && out_has(&h, "goalgraph> "));
    CHECK(err_has(&h, "unsaved changes"));
    CHECK(run(&h, "show never") == CLI_ERROR);

    /* Another command between the two quits resets the warning. */
    CHECK(repl(&h, "quit\nlist\nquit\n", false) == CLI_OK);
    CHECK(out_has(&h, "(3 of 3 goals)"));
    CHECK(repl(&h, "exit\nexit\n", false) == CLI_OK);

    /* No unsaved changes: quit immediately; last line may lack a newline. */
    harness clean = harness_create();
    CHECK(repl(&clean, "help\nquit", true) == CLI_OK);
    CHECK(clean.err_text[0] == '\0');
    CHECK(cli_repl(clean.s, NULL, false) == CLI_ERROR);
    CHECK(cli_repl(NULL, stdin, false) == CLI_ERROR);
    harness_free(&clean);
    harness_free(&h);
}

static void test_session_api(void)
{
    CHECK(cli_session_create(NULL, stderr) == NULL);
    CHECK(cli_session_create(stdout, NULL) == NULL);
    cli_session_free(NULL);
    CHECK(!cli_session_modified(NULL));
    CHECK(cli_run_args(NULL, 1, NULL) == CLI_ERROR);

    harness h = harness_create();
    char cmd[] = "add";
    char id[] = "via args";
    char *argv[] = {cmd, id, NULL};
    CHECK(cli_run_args(h.s, 2, argv) == CLI_OK);
    CHECK(cli_run_line(h.s, NULL) == CLI_ERROR);
    harness_free(&h);
}

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } const tests[] = {
        {"help_and_parsing", test_help_and_parsing},
        {"goal_editing", test_goal_editing},
        {"edges", test_edges},
        {"order_and_cycles", test_order_and_cycles},
        {"paths", test_paths},
        {"recommend", test_recommend},
        {"files", test_files},
        {"export", test_export},
        {"repl", test_repl},
        {"session_api", test_session_api},
    };

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before = g_failures;
        tests[i].fn();
        printf("  %-40s %s\n", tests[i].name, g_failures == before ? "ok" : "FAILED");
    }

    printf("test_cli: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
