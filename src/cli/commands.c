#include "cli_internal.h"

#include "algo.h"
#include "export.h"
#include "graph.h"
#include "persist.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_SCORE 5
#define DEFAULT_TOP 5
#define ID_COLUMN_MIN 4
#define ID_COLUMN_MAX 40
#define OPTION_KEY_MAX 32
#define TYPE_NAME_MAX 16
#define MASK_TEXT_SIZE 64
#define DATE_TEXT_SIZE 32

/* ------------------------------------------------------------- helpers -- */

static char *dup_string(const char *s)
{
    const size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

/* Case-insensitive match that also treats '-' and '_' as equal. */
static bool name_matches(const char *text, const char *canonical)
{
    for (;; text++, canonical++) {
        int a = tolower((unsigned char)*text);
        int b = tolower((unsigned char)*canonical);
        a = a == '-' ? '_' : a;
        b = b == '-' ? '_' : b;
        if (a != b) {
            return false;
        }
        if (a == '\0') {
            return true;
        }
    }
}

static graph_node_info goal_info(const cli_session *s, size_t index)
{
    graph_node_info info = {.id = "?", .category = ""};
    if (graph_get_node(s->g, index, &info) != GRAPH_OK) {
        info = (graph_node_info){.id = "?", .category = ""};
    }
    return info;
}

static cli_result find_goal(cli_session *s, const char *id, size_t *out)
{
    if (graph_find_node(s->g, id, out) != GRAPH_OK) {
        return cli_error(s, "no goal named '%s'", id);
    }
    return CLI_OK;
}

static void format_date(time_t t, char *buf, size_t size)
{
    const struct tm *tm = gmtime(&t);
    if (tm == NULL || strftime(buf, size, "%Y-%m-%d %H:%M UTC", tm) == 0) {
        if (snprintf(buf, size, "%lld", (long long)t) < 0) {
            buf[0] = '\0';
        }
    }
}

/* Width for an ID column: the longest ID, clamped to a sensible range. */
static int id_column_width(const cli_session *s)
{
    size_t width = ID_COLUMN_MIN;
    for (size_t i = 0; i < graph_node_count(s->g); i++) {
        const size_t len = strlen(goal_info(s, i).id);
        width = len > width ? len : width;
    }
    return (int)(width < ID_COLUMN_MAX ? width : ID_COLUMN_MAX);
}

/* ------------------------------------------------------------- options -- */

/* A "key=value" argument, or a bare "flag" (value NULL). */
typedef struct option_arg {
    char key[OPTION_KEY_MAX];
    const char *value;
} option_arg;

static cli_result parse_option(cli_session *s, const char *arg, option_arg *out)
{
    const char *eq = strchr(arg, '=');
    const size_t key_len = eq != NULL ? (size_t)(eq - arg) : strlen(arg);
    if (key_len == 0 || key_len >= OPTION_KEY_MAX) {
        return cli_error(s, "invalid option '%s'", arg);
    }
    memcpy(out->key, arg, key_len);
    out->key[key_len] = '\0';
    out->value = eq != NULL ? eq + 1 : NULL;
    return CLI_OK;
}

static cli_result unknown_option(cli_session *s, const option_arg *o)
{
    return cli_error(s, "unknown option '%s'", o->key);
}

static cli_result need_value(cli_session *s, const option_arg *o)
{
    if (o->value == NULL) {
        return cli_error(s, "option '%s' needs a value (%s=...)", o->key, o->key);
    }
    return CLI_OK;
}

static cli_result need_flag(cli_session *s, const option_arg *o)
{
    if (o->value != NULL) {
        return cli_error(s, "option '%s' does not take a value", o->key);
    }
    return CLI_OK;
}

static cli_result parse_score(cli_session *s, const option_arg *o, int *out)
{
    if (need_value(s, o) != CLI_OK) {
        return CLI_ERROR;
    }
    char *end = NULL;
    errno = 0;
    const long value = strtol(o->value, &end, 10);
    if (end == o->value || *end != '\0' || errno == ERANGE || value < GRAPH_SCORE_MIN ||
        value > GRAPH_SCORE_MAX) {
        return cli_error(s, "%s must be an integer from %d to %d", o->key, GRAPH_SCORE_MIN,
                         GRAPH_SCORE_MAX);
    }
    *out = (int)value;
    return CLI_OK;
}

static cli_result parse_number(cli_session *s, const option_arg *o, double *out)
{
    if (need_value(s, o) != CLI_OK) {
        return CLI_ERROR;
    }
    char *end = NULL;
    const double value = strtod(o->value, &end);
    if (end == o->value || *end != '\0' || !isfinite(value)) {
        return cli_error(s, "%s must be a finite number", o->key);
    }
    *out = value;
    return CLI_OK;
}

static cli_result parse_weight(cli_session *s, const option_arg *o, float *out)
{
    double value = 0.0;
    if (parse_number(s, o, &value) != CLI_OK) {
        return CLI_ERROR;
    }
    if (fabs(value) > FLT_MAX) {
        return cli_error(s, "%s is out of range", o->key);
    }
    *out = (float)value;
    return CLI_OK;
}

static cli_result parse_count(cli_session *s, const option_arg *o, size_t *out)
{
    if (need_value(s, o) != CLI_OK) {
        return CLI_ERROR;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(o->value, &end, 10);
    if (end == o->value || *end != '\0' || errno == ERANGE || o->value[0] == '-' ||
        value > SIZE_MAX) {
        return cli_error(s, "%s must be a non-negative integer", o->key);
    }
    *out = (size_t)value;
    return CLI_OK;
}

static cli_result parse_status(cli_session *s, const char *text, node_status *out)
{
    static const struct {
        const char *alias;
        node_status status;
    } aliases[] = {
        {"todo", NODE_STATUS_NOT_STARTED},
        {"active", NODE_STATUS_IN_PROGRESS},
        {"doing", NODE_STATUS_IN_PROGRESS},
        {"done", NODE_STATUS_COMPLETED},
    };
    for (int st = 0; st < NODE_STATUS_COUNT; st++) {
        if (name_matches(text, node_status_str((node_status)st))) {
            *out = (node_status)st;
            return CLI_OK;
        }
    }
    for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; i++) {
        if (name_matches(text, aliases[i].alias)) {
            *out = aliases[i].status;
            return CLI_OK;
        }
    }
    return cli_error(s, "unknown status '%s' (use not_started, in_progress, completed or "
                        "abandoned)",
                     text);
}

static bool lookup_edge_type(const char *text, edge_type *out)
{
    for (int t = 0; t < EDGE_TYPE_COUNT; t++) {
        if (name_matches(text, edge_type_str((edge_type)t))) {
            *out = (edge_type)t;
            return true;
        }
    }
    return false;
}

static cli_result parse_edge_type(cli_session *s, const char *text, edge_type *out)
{
    if (!lookup_edge_type(text, out)) {
        return cli_error(s, "unknown edge type '%s' (use prereq, similar, enables, conflicts "
                            "or custom)",
                         text);
    }
    return CLI_OK;
}

/* Parses "all" or a comma-separated list of edge type names. */
static cli_result parse_type_mask(cli_session *s, const char *text, algo_edge_mask *out)
{
    if (name_matches(text, "all")) {
        *out = ALGO_EDGES_ALL;
        return CLI_OK;
    }
    algo_edge_mask mask = 0;
    const char *p = text;
    for (;;) {
        const char *comma = strchr(p, ',');
        const size_t len = comma != NULL ? (size_t)(comma - p) : strlen(p);
        char name[TYPE_NAME_MAX];
        edge_type type = EDGE_PREREQ;
        if (len == 0 || len >= sizeof name) {
            return cli_error(s, "invalid edge type list '%s'", text);
        }
        memcpy(name, p, len);
        name[len] = '\0';
        if (parse_edge_type(s, name, &type) != CLI_OK) {
            return CLI_ERROR;
        }
        mask |= algo_edge_bit(type);
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }
    *out = mask;
    return CLI_OK;
}

/* Writes a mask as "all" or "PREREQ,ENABLES". */
static void describe_mask(algo_edge_mask mask, char *buf, size_t size)
{
    if (mask == ALGO_EDGES_ALL) {
        if (snprintf(buf, size, "all") < 0) {
            buf[0] = '\0';
        }
        return;
    }
    size_t used = 0;
    buf[0] = '\0';
    for (int t = 0; t < EDGE_TYPE_COUNT; t++) {
        if ((mask & algo_edge_bit((edge_type)t)) == 0) {
            continue;
        }
        const int n = snprintf(buf + used, size - used, "%s%s", used > 0 ? "," : "",
                               edge_type_str((edge_type)t));
        if (n < 0 || (size_t)n >= size - used) {
            return; /* truncated: good enough for a description */
        }
        used += (size_t)n;
    }
}

static const char *const weight_field_names[ALGO_WEIGHT_FIELD_COUNT] = {
    "hops", "effort", "enjoyment", "achievability", "similarity", "time",
};

static cli_result parse_weight_field(cli_session *s, const char *text, algo_weight_field *out)
{
    for (int f = 0; f < ALGO_WEIGHT_FIELD_COUNT; f++) {
        if (name_matches(text, weight_field_names[f])) {
            *out = (algo_weight_field)f;
            return CLI_OK;
        }
    }
    return cli_error(s, "unknown weight '%s' (use hops, effort, enjoyment, achievability, "
                        "similarity or time)",
                     text);
}

/* Accepts an optional single "force" argument at argv[index]. */
static cli_result parse_force(cli_session *s, int argc, char *const argv[], int index,
                              bool *force)
{
    *force = false;
    if (argc > index) {
        if (!cli_equal_ci(argv[index], "force")) {
            return cli_error(s, "unexpected argument '%s'", argv[index]);
        }
        *force = true;
    }
    return CLI_OK;
}

/* ---------------------------------------------------------- help/files -- */

static cli_result cmd_help(cli_session *s, int argc, char *const argv[])
{
    if (argc == 2) {
        const cli_command *c = cli_find_command(argv[1]);
        if (c == NULL) {
            return cli_error(s, "unknown command '%s'", argv[1]);
        }
        cli_print(s, "usage: %s %s\n  %s\n", c->name, c->usage, c->summary);
        return CLI_OK;
    }
    cli_print(s, "Commands:\n");
    for (const cli_command *c = cli_commands; c->name != NULL; c++) {
        cli_print(s, "  %-9s %s\n", c->name, c->summary);
    }
    cli_print(s, "\nType 'help <command>' for its arguments. Options are key=value pairs;\n"
                 "quote arguments that contain spaces: add \"learn rust\" effort=6\n"
                 "Statuses: not_started, in_progress, completed, abandoned.\n"
                 "Edge types: prereq, similar, enables, conflicts, custom.\n");
    return CLI_OK;
}

static cli_result cmd_new(cli_session *s, int argc, char *const argv[])
{
    bool force = false;
    if (parse_force(s, argc, argv, 1, &force) != CLI_OK) {
        return CLI_ERROR;
    }
    if (s->modified && !force) {
        return cli_error(s, "unsaved changes; 'save' first or use 'new force'");
    }
    graph *g = graph_create();
    if (g == NULL) {
        return cli_error(s, "out of memory");
    }
    cli_replace_graph(s, g);
    free(s->path);
    s->path = NULL;
    cli_print(s, "Started a new, empty goal graph.\n");
    return CLI_OK;
}

static cli_result cmd_load(cli_session *s, int argc, char *const argv[])
{
    bool force = false;
    if (parse_force(s, argc, argv, 2, &force) != CLI_OK) {
        return CLI_ERROR;
    }
    if (s->modified && !force) {
        return cli_error(s, "unsaved changes; 'save' first or use 'load <file> force'");
    }

    graph *g = NULL;
    persist_error perr = {0};
    if (persist_load(argv[1], &g, &perr) != PERSIST_OK) {
        if (perr.line > 0) {
            return cli_error(s, "%s:%zu: %s", argv[1], perr.line, perr.message);
        }
        return cli_error(s, "%s: %s", argv[1], perr.message);
    }
    char *path = dup_string(argv[1]);
    if (path == NULL) {
        graph_free(g);
        return cli_error(s, "out of memory");
    }
    cli_replace_graph(s, g);
    free(s->path);
    s->path = path;
    cli_print(s, "Loaded %zu goals and %zu edges from %s\n", graph_node_count(g),
              graph_edge_count(g), path);
    return CLI_OK;
}

static cli_result cmd_save(cli_session *s, int argc, char *const argv[])
{
    const char *path = argc == 2 ? argv[1] : s->path;
    if (path == NULL) {
        return cli_error(s, "no file name yet; use 'save <file>'");
    }
    char *new_path = NULL;
    if (path != s->path) {
        new_path = dup_string(path);
        if (new_path == NULL) {
            return cli_error(s, "out of memory");
        }
    }

    persist_error perr = {0};
    if (persist_save(s->g, path, &perr) != PERSIST_OK) {
        free(new_path);
        return cli_error(s, "%s: %s", path, perr.message);
    }
    if (new_path != NULL) {
        free(s->path);
        s->path = new_path;
    }
    s->modified = false;
    cli_print(s, "Saved %zu goals and %zu edges to %s\n", graph_node_count(s->g),
              graph_edge_count(s->g), s->path);
    return CLI_OK;
}

/* --------------------------------------------------------------- goals -- */

static cli_result cmd_list(cli_session *s, int argc, char *const argv[])
{
    bool filter_status = false;
    node_status status = NODE_STATUS_NOT_STARTED;
    const char *category = NULL;
    for (int i = 1; i < argc; i++) {
        option_arg o = {0};
        if (parse_option(s, argv[i], &o) != CLI_OK || need_value(s, &o) != CLI_OK) {
            return CLI_ERROR;
        }
        if (strcmp(o.key, "status") == 0) {
            if (parse_status(s, o.value, &status) != CLI_OK) {
                return CLI_ERROR;
            }
            filter_status = true;
        } else if (strcmp(o.key, "category") == 0) {
            category = o.value;
        } else {
            return unknown_option(s, &o);
        }
    }

    const size_t n = graph_node_count(s->g);
    if (n == 0) {
        cli_print(s, "No goals yet. Add one with: add <id>\n");
        return CLI_OK;
    }
    const int width = id_column_width(s);
    cli_print(s, "%-*s  %-11s  %6s  %5s  %s\n", width, "GOAL", "STATUS", "EFFORT", "ENJOY",
              "CATEGORY");
    size_t shown = 0;
    for (size_t i = 0; i < n; i++) {
        const graph_node_info info = goal_info(s, i);
        if ((filter_status && info.status != status) ||
            (category != NULL && strcmp(info.category, category) != 0)) {
            continue;
        }
        cli_print(s, "%-*s  %-11s  %6d  %5d  %s\n", width, info.id,
                  node_status_str(info.status), info.effort, info.enjoyment, info.category);
        shown++;
    }
    cli_print(s, "(%zu of %zu goals)\n", shown, n);
    return CLI_OK;
}

static void print_edge_weights(cli_session *s, const edge_weights *w)
{
    cli_print(s, "  [effort %g, enjoyment %g, achievability %g, similarity %g, time %g]",
              (double)w->effort, (double)w->enjoyment, (double)w->achievability,
              (double)w->similarity, (double)w->time_cost);
}

static void print_edge(cli_session *s, const graph_edge *e, int width)
{
    cli_print(s, "  %-*s -%s-> %s", width, goal_info(s, e->source).id, edge_type_str(e->type),
              goal_info(s, e->target).id);
    print_edge_weights(s, &e->weights);
    cli_print(s, "\n");
}

/* Prints the edges into (incoming) or out of node. Returns how many. */
static size_t print_neighbors(cli_session *s, size_t node, bool incoming)
{
    size_t shown = 0;
    for (size_t u = 0; u < graph_node_count(s->g); u++) {
        const graph_edge *edges = NULL;
        size_t count = 0;
        if (graph_out_edges(s->g, u, &edges, &count) != GRAPH_OK) {
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            const graph_edge *e = &edges[i];
            if ((incoming ? e->target : e->source) != node) {
                continue;
            }
            const graph_node_info other = goal_info(s, incoming ? e->source : e->target);
            cli_print(s, "    %-9s %s %s (%s)\n", edge_type_str(e->type), incoming ? "<-" : "->",
                      other.id, node_status_str(other.status));
            shown++;
        }
    }
    return shown;
}

static cli_result cmd_show(cli_session *s, int argc, char *const argv[])
{
    (void)argc;
    size_t index = 0;
    if (find_goal(s, argv[1], &index) != CLI_OK) {
        return CLI_ERROR;
    }
    const graph_node_info info = goal_info(s, index);
    char created[DATE_TEXT_SIZE];
    char updated[DATE_TEXT_SIZE];
    format_date(info.created_at, created, sizeof created);
    format_date(info.updated_at, updated, sizeof updated);

    cli_print(s, "%s\n", info.id);
    cli_print(s, "  category:  %s\n", info.category[0] != '\0' ? info.category : "(none)");
    cli_print(s, "  status:    %s\n", node_status_str(info.status));
    cli_print(s, "  effort:    %d/%d\n", info.effort, GRAPH_SCORE_MAX);
    cli_print(s, "  enjoyment: %d/%d\n", info.enjoyment, GRAPH_SCORE_MAX);
    cli_print(s, "  created:   %s\n", created);
    cli_print(s, "  updated:   %s\n", updated);
    cli_print(s, "  incoming edges:\n");
    if (print_neighbors(s, index, true) == 0) {
        cli_print(s, "    (none)\n");
    }
    cli_print(s, "  outgoing edges:\n");
    if (print_neighbors(s, index, false) == 0) {
        cli_print(s, "    (none)\n");
    }
    return CLI_OK;
}

/* Applies category/status/effort/enjoyment (and id, if allowed) options. */
static cli_result parse_goal_options(cli_session *s, int first, int argc, char *const argv[],
                                     graph_node_spec *spec, bool allow_rename)
{
    for (int i = first; i < argc; i++) {
        option_arg o = {0};
        if (parse_option(s, argv[i], &o) != CLI_OK || need_value(s, &o) != CLI_OK) {
            return CLI_ERROR;
        }
        cli_result r = CLI_OK;
        if (strcmp(o.key, "category") == 0) {
            spec->category = o.value;
        } else if (strcmp(o.key, "status") == 0) {
            r = parse_status(s, o.value, &spec->status);
        } else if (strcmp(o.key, "effort") == 0) {
            r = parse_score(s, &o, &spec->effort);
        } else if (strcmp(o.key, "enjoyment") == 0) {
            r = parse_score(s, &o, &spec->enjoyment);
        } else if (allow_rename && strcmp(o.key, "id") == 0) {
            if (o.value[0] == '\0') {
                return cli_error(s, "id must not be empty");
            }
            spec->id = o.value;
        } else {
            r = unknown_option(s, &o);
        }
        if (r != CLI_OK) {
            return r;
        }
    }
    return CLI_OK;
}

static cli_result cmd_add(cli_session *s, int argc, char *const argv[])
{
    graph_node_spec spec = {
        .id = argv[1],
        .category = "",
        .status = NODE_STATUS_NOT_STARTED,
        .effort = DEFAULT_SCORE,
        .enjoyment = DEFAULT_SCORE,
    };
    if (argv[1][0] == '\0') {
        return cli_error(s, "goal id must not be empty");
    }
    if (parse_goal_options(s, 2, argc, argv, &spec, false) != CLI_OK) {
        return CLI_ERROR;
    }
    const graph_status st = graph_add_node(s->g, &spec, NULL);
    if (st == GRAPH_ERR_DUPLICATE_ID) {
        return cli_error(s, "a goal named '%s' already exists", argv[1]);
    }
    if (st != GRAPH_OK) {
        return cli_error(s, "cannot add goal: %s", graph_status_str(st));
    }
    cli_mark_modified(s);
    cli_print(s, "Added goal '%s'.\n", argv[1]);
    return CLI_OK;
}

static cli_result update_goal(cli_session *s, const char *id, int first, int argc,
                              char *const argv[], node_status *force_status)
{
    size_t index = 0;
    if (find_goal(s, id, &index) != CLI_OK) {
        return CLI_ERROR;
    }
    const graph_node_info info = goal_info(s, index);
    graph_node_spec spec = {
        .status = info.status,
        .effort = info.effort,
        .enjoyment = info.enjoyment,
    };
    if (force_status != NULL) {
        spec.status = *force_status;
    }
    if (parse_goal_options(s, first, argc, argv, &spec, true) != CLI_OK) {
        return CLI_ERROR;
    }
    const graph_status st = graph_update_node(s->g, index, &spec);
    if (st == GRAPH_ERR_DUPLICATE_ID) {
        return cli_error(s, "a goal named '%s' already exists", spec.id);
    }
    if (st != GRAPH_OK) {
        return cli_error(s, "cannot update goal: %s", graph_status_str(st));
    }
    cli_mark_modified(s);
    cli_print(s, "Updated goal '%s' (%s).\n", spec.id != NULL ? spec.id : id,
              node_status_str(spec.status));
    return CLI_OK;
}

static cli_result cmd_edit(cli_session *s, int argc, char *const argv[])
{
    if (argc < 3) {
        return cli_error(s, "nothing to change; e.g. edit %s status=completed", argv[1]);
    }
    return update_goal(s, argv[1], 2, argc, argv, NULL);
}

static cli_result cmd_done(cli_session *s, int argc, char *const argv[])
{
    node_status completed = NODE_STATUS_COMPLETED;
    return update_goal(s, argv[1], 2, argc, argv, &completed);
}

static cli_result cmd_remove(cli_session *s, int argc, char *const argv[])
{
    (void)argc;
    size_t index = 0;
    if (find_goal(s, argv[1], &index) != CLI_OK) {
        return CLI_ERROR;
    }
    const size_t edges_before = graph_edge_count(s->g);
    const graph_status st = graph_remove_node(s->g, index);
    if (st != GRAPH_OK) {
        return cli_error(s, "cannot remove goal: %s", graph_status_str(st));
    }
    cli_mark_modified(s);
    cli_print(s, "Removed goal '%s' and %zu edge(s).\n", argv[1],
              edges_before - graph_edge_count(s->g));
    return CLI_OK;
}

/* --------------------------------------------------------------- edges -- */

static cli_result parse_link_options(cli_session *s, int argc, char *const argv[],
                                     edge_type *type, edge_weights *w)
{
    for (int i = 3; i < argc; i++) {
        option_arg o = {0};
        if (parse_option(s, argv[i], &o) != CLI_OK || need_value(s, &o) != CLI_OK) {
            return CLI_ERROR;
        }
        cli_result r = CLI_OK;
        if (strcmp(o.key, "type") == 0) {
            r = parse_edge_type(s, o.value, type);
        } else if (strcmp(o.key, "effort") == 0) {
            r = parse_weight(s, &o, &w->effort);
        } else if (strcmp(o.key, "enjoyment") == 0) {
            r = parse_weight(s, &o, &w->enjoyment);
        } else if (strcmp(o.key, "achievability") == 0) {
            r = parse_weight(s, &o, &w->achievability);
        } else if (strcmp(o.key, "similarity") == 0) {
            r = parse_weight(s, &o, &w->similarity);
        } else if (strcmp(o.key, "time") == 0) {
            r = parse_weight(s, &o, &w->time_cost);
        } else {
            r = unknown_option(s, &o);
        }
        if (r != CLI_OK) {
            return r;
        }
    }
    return CLI_OK;
}

static cli_result find_pair(cli_session *s, char *const argv[], size_t *from, size_t *to)
{
    if (find_goal(s, argv[1], from) != CLI_OK || find_goal(s, argv[2], to) != CLI_OK) {
        return CLI_ERROR;
    }
    if (*from == *to) {
        return cli_error(s, "a goal cannot be linked to itself");
    }
    return CLI_OK;
}

static void warn_if_prereq_cycle(cli_session *s)
{
    algo_order order = {0};
    if (algo_topo_sort(s->g, algo_edge_bit(EDGE_PREREQ), &order) == ALGO_ERR_CYCLE) {
        cli_note(s, "warning: prerequisites now form a cycle (see 'cycles types=prereq')\n");
    }
    algo_order_free(&order);
}

static cli_result cmd_link(cli_session *s, int argc, char *const argv[])
{
    size_t from = 0;
    size_t to = 0;
    edge_type type = EDGE_PREREQ;
    edge_weights w = {0};
    if (find_pair(s, argv, &from, &to) != CLI_OK ||
        parse_link_options(s, argc, argv, &type, &w) != CLI_OK) {
        return CLI_ERROR;
    }
    const graph_status st = graph_add_edge(s->g, from, to, type, &w);
    if (st != GRAPH_OK) {
        return cli_error(s, "cannot link: %s", graph_status_str(st));
    }
    cli_mark_modified(s);
    cli_print(s, "Linked %s -%s-> %s.\n", argv[1], edge_type_str(type), argv[2]);
    if (type == EDGE_PREREQ) {
        warn_if_prereq_cycle(s);
    }
    return CLI_OK;
}

static cli_result cmd_unlink(cli_session *s, int argc, char *const argv[])
{
    size_t from = 0;
    size_t to = 0;
    if (find_pair(s, argv, &from, &to) != CLI_OK) {
        return CLI_ERROR;
    }
    algo_edge_mask mask = algo_edge_bit(EDGE_PREREQ);
    if (argc == 4) {
        option_arg o = {0};
        if (parse_option(s, argv[3], &o) != CLI_OK || need_value(s, &o) != CLI_OK) {
            return CLI_ERROR;
        }
        if (strcmp(o.key, "type") != 0) {
            return unknown_option(s, &o);
        }
        if (parse_type_mask(s, o.value, &mask) != CLI_OK) {
            return CLI_ERROR;
        }
    }

    size_t total = 0;
    for (int t = 0; t < EDGE_TYPE_COUNT; t++) {
        size_t removed = 0;
        if ((mask == ALGO_EDGES_ALL || (mask & algo_edge_bit((edge_type)t)) != 0) &&
            graph_remove_edges(s->g, from, to, (edge_type)t, &removed) == GRAPH_OK) {
            total += removed;
        }
    }
    char types[MASK_TEXT_SIZE];
    describe_mask(mask, types, sizeof types);
    if (total == 0) {
        return cli_error(s, "no %s edge from '%s' to '%s'", types, argv[1], argv[2]);
    }
    cli_mark_modified(s);
    cli_print(s, "Removed %zu edge(s) from '%s' to '%s'.\n", total, argv[1], argv[2]);
    return CLI_OK;
}

static cli_result cmd_edges(cli_session *s, int argc, char *const argv[])
{
    size_t only = SIZE_MAX;
    if (argc == 2 && find_goal(s, argv[1], &only) != CLI_OK) {
        return CLI_ERROR;
    }
    const int width = id_column_width(s);
    size_t shown = 0;
    for (size_t u = 0; u < graph_node_count(s->g); u++) {
        const graph_edge *edges = NULL;
        size_t count = 0;
        if (graph_out_edges(s->g, u, &edges, &count) != GRAPH_OK) {
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            if (only == SIZE_MAX || edges[i].source == only || edges[i].target == only) {
                print_edge(s, &edges[i], width);
                shown++;
            }
        }
    }
    cli_print(s, "(%zu edge(s))\n", shown);
    return CLI_OK;
}

/* ----------------------------------------------------------- algorithms -- */

static cli_result report_cycle(cli_session *s, const algo_order *cycle, const char *types)
{
    cli_error(s, "the %s edges contain a cycle, so no valid order exists:", types);
    cli_note(s, "  ");
    for (size_t i = 0; i < cycle->count; i++) {
        cli_note(s, "%s -> ", goal_info(s, cycle->nodes[i]).id);
    }
    if (cycle->count > 0) {
        cli_note(s, "%s\n", goal_info(s, cycle->nodes[0]).id);
    }
    return CLI_ERROR;
}

static cli_result cmd_order(cli_session *s, int argc, char *const argv[])
{
    algo_edge_mask mask = algo_edge_bit(EDGE_PREREQ);
    bool remaining = false;
    for (int i = 1; i < argc; i++) {
        option_arg o = {0};
        if (parse_option(s, argv[i], &o) != CLI_OK) {
            return CLI_ERROR;
        }
        cli_result r = CLI_OK;
        if (strcmp(o.key, "types") == 0) {
            r = need_value(s, &o) == CLI_OK ? parse_type_mask(s, o.value, &mask) : CLI_ERROR;
        } else if (strcmp(o.key, "remaining") == 0) {
            r = need_flag(s, &o);
            remaining = true;
        } else {
            r = unknown_option(s, &o);
        }
        if (r != CLI_OK) {
            return r;
        }
    }

    char types[MASK_TEXT_SIZE];
    describe_mask(mask, types, sizeof types);
    algo_order order = {0};
    const algo_status st = algo_topo_sort(s->g, mask, &order);
    if (st == ALGO_ERR_CYCLE) {
        const cli_result r = report_cycle(s, &order, types);
        algo_order_free(&order);
        return r;
    }
    if (st != ALGO_OK) {
        return cli_error(s, "cannot sort: %s", algo_status_str(st));
    }

    const int width = id_column_width(s);
    cli_print(s, "Valid order following %s edges%s:\n", types,
              remaining ? " (unfinished goals only)" : "");
    size_t step = 0;
    for (size_t i = 0; i < order.count; i++) {
        const graph_node_info info = goal_info(s, order.nodes[i]);
        if (remaining && (info.status == NODE_STATUS_COMPLETED ||
                          info.status == NODE_STATUS_ABANDONED)) {
            continue;
        }
        cli_print(s, "  %3zu. %-*s  [%s]\n", ++step, width, info.id,
                  node_status_str(info.status));
    }
    if (step == 0) {
        cli_print(s, "  (nothing left to do)\n");
    }
    algo_order_free(&order);
    return CLI_OK;
}

/* A* heuristic: any path into target must use one of its incoming edges. */
typedef struct entry_bound {
    size_t target;
    double min_incoming; /* cheapest masked edge into target, >= 0 and finite */
} entry_bound;

static double edge_field_value(const graph_edge *e, algo_weight_field field)
{
    switch (field) {
    case ALGO_WEIGHT_EFFORT:
        return e->weights.effort;
    case ALGO_WEIGHT_ENJOYMENT:
        return e->weights.enjoyment;
    case ALGO_WEIGHT_ACHIEVABILITY:
        return e->weights.achievability;
    case ALGO_WEIGHT_SIMILARITY:
        return e->weights.similarity;
    case ALGO_WEIGHT_TIME_COST:
        return e->weights.time_cost;
    default:
        return 1.0;
    }
}

/*
 * The cheapest selected edge into target is a lower bound on the remaining
 * cost from any other node, so this heuristic is admissible and consistent.
 */
static entry_bound compute_entry_bound(const cli_session *s, size_t target,
                                       const algo_path_options *opts)
{
    entry_bound b = {.target = target, .min_incoming = INFINITY};
    for (size_t u = 0; u < graph_node_count(s->g); u++) {
        const graph_edge *edges = NULL;
        size_t count = 0;
        if (graph_out_edges(s->g, u, &edges, &count) != GRAPH_OK) {
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            const bool selected = opts->edge_types == ALGO_EDGES_ALL ||
                                  (opts->edge_types & algo_edge_bit(edges[i].type)) != 0;
            const double w = edge_field_value(&edges[i], opts->field);
            if (edges[i].target == target && selected && w < b.min_incoming) {
                b.min_incoming = w;
            }
        }
    }
    if (!(b.min_incoming >= 0.0) || !isfinite(b.min_incoming)) {
        b.min_incoming = 0.0;
    }
    return b;
}

static double entry_heuristic(const graph *g, size_t node, size_t target, void *ctx)
{
    (void)g;
    (void)target;
    const entry_bound *b = ctx;
    return node == b->target ? 0.0 : b->min_incoming;
}

static cli_result parse_path_options(cli_session *s, int argc, char *const argv[],
                                     algo_path_options *opts, bool *astar)
{
    for (int i = 3; i < argc; i++) {
        option_arg o = {0};
        if (parse_option(s, argv[i], &o) != CLI_OK) {
            return CLI_ERROR;
        }
        cli_result r = CLI_OK;
        if (strcmp(o.key, "weight") == 0) {
            r = need_value(s, &o) == CLI_OK ? parse_weight_field(s, o.value, &opts->field)
                                            : CLI_ERROR;
        } else if (strcmp(o.key, "types") == 0) {
            r = need_value(s, &o) == CLI_OK ? parse_type_mask(s, o.value, &opts->edge_types)
                                            : CLI_ERROR;
        } else if (strcmp(o.key, "astar") == 0) {
            r = need_flag(s, &o);
            *astar = true;
        } else {
            r = unknown_option(s, &o);
        }
        if (r != CLI_OK) {
            return r;
        }
    }
    return CLI_OK;
}

static cli_result cmd_path(cli_session *s, int argc, char *const argv[])
{
    size_t from = 0;
    size_t to = 0;
    if (find_goal(s, argv[1], &from) != CLI_OK || find_goal(s, argv[2], &to) != CLI_OK) {
        return CLI_ERROR;
    }
    algo_path_options opts = {
        .edge_types = algo_edge_bit(EDGE_PREREQ) | algo_edge_bit(EDGE_ENABLES),
        .field = ALGO_WEIGHT_HOPS,
    };
    bool astar = false;
    if (parse_path_options(s, argc, argv, &opts, &astar) != CLI_OK) {
        return CLI_ERROR;
    }
    entry_bound bound = compute_entry_bound(s, to, &opts);
    if (astar) {
        opts.heuristic = entry_heuristic;
        opts.heuristic_ctx = &bound;
    }

    char types[MASK_TEXT_SIZE];
    describe_mask(opts.edge_types, types, sizeof types);
    algo_path path = {0};
    const algo_status st = algo_shortest_path(s->g, from, to, &opts, &path);
    if (st == ALGO_ERR_NO_PATH) {
        return cli_error(s, "no path from '%s' to '%s' along %s edges", argv[1], argv[2], types);
    }
    if (st == ALGO_ERR_BAD_WEIGHT) {
        return cli_error(s, "%s weights must be non-negative", weight_field_names[opts.field]);
    }
    if (st != ALGO_OK) {
        return cli_error(s, "cannot find path: %s", algo_status_str(st));
    }

    cli_print(s, "Best path from %s to %s (%s, %s edges): %zu step(s), total %s %g\n", argv[1],
              argv[2], astar ? "A*" : "Dijkstra", types, path.length - 1,
              weight_field_names[opts.field], path.cost);
    for (size_t i = 0; i < path.length; i++) {
        const graph_node_info info = goal_info(s, path.nodes[i]);
        cli_print(s, "  %3zu. %s [%s]\n", i + 1, info.id, node_status_str(info.status));
    }
    cli_clear_last_path(s);
    s->last_path = path.nodes; /* kept for 'export ... highlight' */
    s->last_path_length = path.length;
    return CLI_OK;
}

static cli_result parse_recommend_options(cli_session *s, int argc, char *const argv[],
                                          algo_rec_options *o)
{
    for (int i = 1; i < argc; i++) {
        option_arg a = {0};
        if (parse_option(s, argv[i], &a) != CLI_OK) {
            return CLI_ERROR;
        }
        cli_result r = CLI_OK;
        if (strcmp(a.key, "top") == 0) {
            r = parse_count(s, &a, &o->max_results);
        } else if (strcmp(a.key, "blocked") == 0) {
            r = need_flag(s, &a);
            o->include_blocked = true;
        } else if (strcmp(a.key, "no-in-progress") == 0) {
            r = need_flag(s, &a);
            o->include_in_progress = false;
        } else if (strcmp(a.key, "enjoyment") == 0) {
            r = parse_number(s, &a, &o->enjoyment_weight);
        } else if (strcmp(a.key, "effort") == 0) {
            r = parse_number(s, &a, &o->effort_weight);
        } else if (strcmp(a.key, "achievability") == 0) {
            r = parse_number(s, &a, &o->achievability_weight);
        } else if (strcmp(a.key, "penalty") == 0) {
            r = parse_number(s, &a, &o->conflict_penalty);
        } else {
            r = unknown_option(s, &a);
        }
        if (r != CLI_OK) {
            return r;
        }
    }
    return CLI_OK;
}

static void print_recommendation(cli_session *s, size_t rank, const algo_recommendation *r,
                                 int width)
{
    const graph_node_info info = goal_info(s, r->node);
    cli_print(s, "  %2zu. %-*s  score %.3f  (enjoy %d, effort %d, achievability %.2f", rank,
              width, info.id, r->score, info.enjoyment, info.effort, r->achievability);
    if (r->blocked) {
        cli_print(s, ", blocked: %zu/%zu prerequisites done", r->prereq_met, r->prereq_total);
    }
    if (r->conflicted) {
        cli_print(s, ", conflicts with an active goal");
    }
    cli_print(s, ")%s\n", info.status == NODE_STATUS_IN_PROGRESS ? " *" : "");
}

static cli_result cmd_recommend(cli_session *s, int argc, char *const argv[])
{
    algo_rec_options o = algo_rec_default_options();
    o.max_results = DEFAULT_TOP;
    if (parse_recommend_options(s, argc, argv, &o) != CLI_OK) {
        return CLI_ERROR;
    }
    algo_rec_list list = {0};
    const algo_status st = algo_recommend(s->g, &o, &list);
    if (st == ALGO_ERR_INVALID_ARG) {
        return cli_error(s, "weights must be >= 0 (not all 0) and penalty must be in [0, 1]");
    }
    if (st != ALGO_OK) {
        return cli_error(s, "cannot recommend: %s", algo_status_str(st));
    }
    if (list.count == 0) {
        cli_print(s, "Nothing to recommend: every goal is finished or waiting on "
                     "prerequisites (try 'recommend blocked').\n");
        return CLI_OK;
    }

    const int width = id_column_width(s);
    cli_print(s, "Top %zu recommended goal(s):\n", list.count);
    bool any_in_progress = false;
    for (size_t i = 0; i < list.count; i++) {
        print_recommendation(s, i + 1, &list.items[i], width);
        any_in_progress |= goal_info(s, list.items[i].node).status == NODE_STATUS_IN_PROGRESS;
    }
    if (any_in_progress) {
        cli_print(s, "  (* already in progress)\n");
    }
    algo_rec_list_free(&list);
    return CLI_OK;
}

static cli_result cmd_cycles(cli_session *s, int argc, char *const argv[])
{
    algo_edge_mask mask = ALGO_EDGES_ALL;
    if (argc == 2) {
        option_arg o = {0};
        if (parse_option(s, argv[1], &o) != CLI_OK || need_value(s, &o) != CLI_OK) {
            return CLI_ERROR;
        }
        if (strcmp(o.key, "types") != 0) {
            return unknown_option(s, &o);
        }
        if (parse_type_mask(s, o.value, &mask) != CLI_OK) {
            return CLI_ERROR;
        }
    }

    char types[MASK_TEXT_SIZE];
    describe_mask(mask, types, sizeof types);
    algo_scc scc = {0};
    const algo_status st = algo_scc_compute(s->g, mask, &scc);
    if (st != ALGO_OK) {
        return cli_error(s, "cannot compute cycles: %s", algo_status_str(st));
    }
    size_t groups = 0;
    for (size_t c = 0; c < scc.component_count; c++) {
        const size_t size = scc.offsets[c + 1] - scc.offsets[c];
        if (size < 2) {
            continue;
        }
        cli_print(s, "Cycle group %zu (%zu goals):", ++groups, size);
        for (size_t k = scc.offsets[c]; k < scc.offsets[c + 1]; k++) {
            cli_print(s, " %s", goal_info(s, scc.members[k]).id);
        }
        cli_print(s, "\n");
    }
    if (groups == 0) {
        cli_print(s, "No cycles among %s edges.\n", types);
    }
    algo_scc_free(&scc);
    return CLI_OK;
}

/* -------------------------------------------------------------- export -- */

static cli_result cmd_export(cli_session *s, int argc, char *const argv[])
{
    export_dot_options opts = {0};
    for (int i = 2; i < argc; i++) {
        option_arg o = {0};
        if (parse_option(s, argv[i], &o) != CLI_OK) {
            return CLI_ERROR;
        }
        cli_result r = CLI_OK;
        if (strcmp(o.key, "cluster") == 0) {
            r = need_flag(s, &o);
            opts.cluster_by_category = true;
        } else if (strcmp(o.key, "highlight") == 0) {
            r = need_flag(s, &o);
            if (r == CLI_OK && s->last_path == NULL) {
                r = cli_error(s, "no path to highlight; run 'path <from> <to>' first");
            }
            opts.highlight = s->last_path;
            opts.highlight_count = s->last_path_length;
        } else if (strcmp(o.key, "title") == 0) {
            r = need_value(s, &o);
            opts.title = o.value;
        } else {
            r = unknown_option(s, &o);
        }
        if (r != CLI_OK) {
            return r;
        }
    }

    const export_status st = export_dot_file(s->g, argv[1], &opts);
    if (st != EXPORT_OK) {
        return cli_error(s, "cannot export to %s: %s", argv[1], export_status_str(st));
    }
    cli_print(s, "Exported %zu goals and %zu edges to %s\n", graph_node_count(s->g),
              graph_edge_count(s->g), argv[1]);
    cli_print(s, "Render it with: dot -Tsvg %s -o goals.svg\n", argv[1]);
    return CLI_OK;
}

static cli_result cmd_quit(cli_session *s, int argc, char *const argv[])
{
    (void)argc;
    (void)argv;
    if (s->modified && !s->quit_warned) {
        s->quit_warned = true;
        cli_note(s, "warning: you have unsaved changes. Use 'save' to keep them, or "
                    "'quit' again to discard them.\n");
        return CLI_OK;
    }
    return CLI_QUIT;
}

/* --------------------------------------------------------------- table -- */

const cli_command cli_commands[] = {
    {"help", "[command]", "Show commands, or details for one command", 0, 1, cmd_help},
    {"load", "<file> [force]", "Load a goal graph (force discards unsaved changes)", 1, 2,
     cmd_load},
    {"save", "[file]", "Save the graph (atomically) to file or to the loaded file", 0, 1,
     cmd_save},
    {"new", "[force]", "Start an empty graph", 0, 1, cmd_new},
    {"list", "[status=S] [category=C]", "List goals, optionally filtered", 0, 2, cmd_list},
    {"show", "<id>", "Show one goal with its incoming and outgoing edges", 1, 1, cmd_show},
    {"add", "<id> [category=C] [status=S] [effort=1-10] [enjoyment=1-10]",
     "Add a goal (defaults: not_started, effort 5, enjoyment 5)", 1, -1, cmd_add},
    {"edit", "<id> [id=NEW] [category=C] [status=S] [effort=N] [enjoyment=N]",
     "Change a goal's attributes or rename it", 1, -1, cmd_edit},
    {"done", "<id>", "Mark a goal completed", 1, 1, cmd_done},
    {"remove", "<id>", "Remove a goal and all of its edges", 1, 1, cmd_remove},
    {"link",
     "<from> <to> [type=prereq|similar|enables|conflicts|custom] [effort=F] [enjoyment=F] "
     "[achievability=F] [similarity=F] [time=F]",
     "Add an edge (default type prereq: <from> must come before <to>)", 2, -1, cmd_link},
    {"unlink", "<from> <to> [type=T[,T...]|all]", "Remove edges (default type prereq)", 2, 3,
     cmd_unlink},
    {"edges", "[id]", "List all edges, or the edges touching one goal", 0, 1, cmd_edges},
    {"order", "[types=T[,T...]|all] [remaining]",
     "Topological order of goals (default: prereq edges)", 0, 2, cmd_order},
    {"path",
     "<from> <to> [weight=hops|effort|enjoyment|achievability|similarity|time] "
     "[types=T[,T...]|all] [astar]",
     "Cheapest path via Dijkstra or A* (default: hops over prereq,enables)", 2, 5, cmd_path},
    {"recommend",
     "[top=N] [blocked] [no-in-progress] [enjoyment=W] [effort=W] [achievability=W] "
     "[penalty=P]",
     "Rank the best next goals (top=0 shows all)", 0, -1, cmd_recommend},
    {"cycles", "[types=T[,T...]|all]", "Show groups of goals that form cycles", 0, 1,
     cmd_cycles},
    {"export", "<file.dot> [cluster] [highlight] [title=TEXT]",
     "Write a Graphviz DOT file (highlight = last path)", 1, 4, cmd_export},
    {"quit", "", "Leave the shell (warns once about unsaved changes)", 0, 0, cmd_quit},
    {"exit", "", "Same as quit", 0, 0, cmd_quit},
    {NULL, NULL, NULL, 0, 0, NULL},
};
