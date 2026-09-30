# GoalGraph

**A goal-planning engine and command-line tool written in standard C17.**

GoalGraph models your goals (courses, projects, career milestones) as a directed, weighted
multigraph. It answers the questions that matter when planning:

- *What order should I do things in?*
- *What's the cheapest route from here to there?*
- *Which goals are stuck in a loop?*
- *What should I work on next?*

You can use it as an interactive shell or as one-shot commands. Graphs are saved as readable
text files and can be exported to Graphviz for visualization.

```text
$ goalgraph -f data/sample_goals.dat recommend top=3
Top 3 recommended goal(s):
   1. proj-goalgraph                      score 0.778  (enjoy 10, effort 7, achievability 1.00) *
   2. proj-ray-tracer                     score 0.778  (enjoy 9, effort 6, achievability 1.00)
   3. proj-open-source-contribution       score 0.741  (enjoy 7, effort 5, achievability 1.00)
```

---

## Contents

- [Key Features](#key-features)
- [Architecture Overview](#architecture-overview)
- [Build & Test](#build--test)
- [Quick Start](#quick-start)
- [CLI Reference](#cli-reference)
- [Graphviz Visualization](#graphviz-visualization)
- [Data File Format](#data-file-format)
- [Library API](#library-api)
- [Known Limitations](#known-limitations)

---

## Key Features

| Feature | Details |
|---|---|
| **Dijkstra / A\* shortest paths** | The edge cost can be the hop count, any edge weight (effort, enjoyment, achievability, similarity, time), or your own callback. You can restrict which edge types are followed. A* is optimal with any admissible heuristic, including inconsistent ones. |
| **Kahn's topological sort** | Gives a valid study order. When several goals are ready at once, it takes the lowest node index first, so the order is always the same. If there is a cycle, it returns the actual goals that form the cycle, not just an error flag. |
| **Tarjan's strongly connected components** | Iterative, so deep graphs don't overflow the stack (tested on a 50,000-node cycle). Components are numbered in topological order of the condensed graph. |
| **Goal recommendation engine** | Ranks candidate goals by enjoyment, low effort, and achievability, with adjustable weights. Goals with unfinished prerequisites are held back, and goals that conflict with work already in progress are penalized. |
| **Atomic persistence** | Saves write to a temporary file and then rename it over the target, so a failed save never corrupts the existing file. The format is line-based text, and floats round-trip exactly. Load errors report the line number and the cause. |
| **Graphviz `.dot` export** | Fill color shows status and line style shows edge type. Goals can be grouped into boxes by category, and the last path you searched for can be highlighted. |
| **Interactive shell and one-shot CLI** | 20 commands, quoting for IDs with spaces, protection against losing unsaved changes, and auto-save in one-shot mode. |

---

## Architecture Overview

### Engineering principles

- **Standard C17 only.** No compiler extensions and no third-party dependencies. Only the C
  standard library is used.
- **Warnings are errors.** Every file compiles cleanly with `-std=c17 -Wall -Wextra -Werror`.
- **Opaque types.** Module state (such as `graph`, `cli_session`) is declared in headers and
  defined only in `.c` files. Public symbols carry their module's prefix.
- **Explicit, verified memory management.**
  - Every allocation is checked. Size calculations are checked for overflow before allocating.
  - `realloc` results go into a temporary and are only committed on success.
  - Functions that hold several resources release them through a single `goto cleanup` path.
  - Every `*_create` has a matching `*_free`, and every `*_free` accepts `NULL`.
  - Each header documents who owns every returned pointer.
- **Zero leaks, verified.** The whole test suite runs under Valgrind
  (`--leak-check=full --errors-for-leak-kinds=all`), AddressSanitizer and UBSan, with 0 leaks
  and 0 errors.

### The graph ADT

The graph is a **directed, weighted multigraph** stored as **dynamic out-adjacency lists**:

```text
graph
 └── nodes[]  (dynamic array, grows by doubling)
      ├── id, category          owned string copies
      ├── status, effort, enjoyment, created_at, updated_at
      └── out[]  (dynamic array of graph_edge, grows by doubling)
           └── { source, target, type, weights{effort, enjoyment,
                                               achievability, similarity, time_cost} }
```

- Each node has a unique string ID and a dense index (`0 … n-1`), assigned in insertion order.
- Parallel edges are allowed, of the same or different types. Self-loops are rejected.
- Edge types are `PREREQ`, `SIMILAR`, `ENABLES`, `CONFLICTS` and `CUSTOM`.
- Edge direction: **`A -PREREQ-> B` means "A must be completed before B."**
- Removing a node also removes every edge that touches it, and renumbers the later nodes and
  their edges, so any saved indices must be looked up again.

### Module layout

```mermaid
graph TD
    main["src/cli/main.c<br/>(entry point)"] --> cli
    cli["cli<br/>commands, tokenizer, REPL"] --> algo
    cli --> persist
    cli --> export
    cli --> graph
    algo["algo<br/>Dijkstra/A*, Kahn, Tarjan, recommend"] --> graph
    persist["persist<br/>atomic save / load"] --> graph
    export["export<br/>Graphviz DOT"] --> graph
    graph["graph<br/>multigraph ADT"]
```

```text
include/            Public headers (one per module)
  graph.h           Graph ADT: nodes, edges, add / update / remove, lookups
  algo.h            Shortest paths, topological sort, SCCs, recommendations
  persist.h         Text file format, atomic save, load with diagnostics
  export.h          Graphviz DOT export
  cli.h             Command interpreter used by the executable
src/
  graph/graph.c
  algo/algo.c
  persist/persist.c
  export/export.c
  cli/cli.c         Session, tokenizer, dispatch, REPL
  cli/commands.c    Command handlers and command table
  cli/cli_internal.h  Private header shared by the cli sources
  cli/main.c        Program entry point (argument parsing only)
tests/              One test executable per module, using public APIs only
  test_graph.c  test_algo.c  test_persist.c  test_export.c  test_cli.c
data/
  sample_goals.dat  Sample dataset: a CS student's 4-year plan
```

There are no circular dependencies. `main.c` is left out of the test build, so all command
logic lives in the `cli` module, where `test_cli.c` can test it.

### Algorithms at a glance

| Algorithm | Function | Complexity |
|---|---|---|
| Dijkstra (binary heap, lazy deletion) | `algo_shortest_path` | O((V + E) log V) |
| A* (same engine plus a heuristic callback) | `algo_shortest_path` with `opts.heuristic` | O((V + E) log V) worst case |
| Kahn's topological sort (min-heap of ready nodes) | `algo_topo_sort` | O((V + E) log V) |
| Cycle extraction (iterative DFS, run only if the sort gets stuck) | inside `algo_topo_sort` | O(V + E) |
| Tarjan's SCC (iterative) | `algo_scc_compute` | O(V + E) |
| Recommendation scoring | `algo_recommend` | O(V log V + E) |

---

## Build & Test

**Requirements:** a C17 compiler (`gcc` or `clang`), `make`, and optionally `valgrind` and
`graphviz`. On Ubuntu or WSL:

```sh
sudo apt install build-essential valgrind graphviz
```

| Command | What it does |
|---|---|
| `make` / `make debug` | Debug build (`-g -O0`) into `build/debug/`. Produces `build/debug/goalgraph` |
| `make release` | Optimized build (`-O2 -DNDEBUG`) into `build/release/` |
| `make test` | Builds and runs every `tests/test_*.c` as its own executable (debug by default; `make test BUILD=release` for release) |
| `make memcheck` | Runs every test executable under Valgrind. Fails on any leak or memory error |
| `make test SANITIZE=1` | Runs the tests with AddressSanitizer and UndefinedBehaviorSanitizer |
| `make clean` | Removes `build/` |

Tests open sample data through paths relative to the project root (`data/...`), so run
`make` from the project root.

```text
$ make test
test_algo: 397488 checks, 0 failures
PASS  build/debug/tests/test_algo
test_cli: 393 checks, 0 failures
PASS  build/debug/tests/test_cli
...
[debug] 5 passed, 0 failed

$ make memcheck
CLEAN build/debug/tests/test_algo
CLEAN build/debug/tests/test_cli
CLEAN build/debug/tests/test_export
CLEAN build/debug/tests/test_graph
CLEAN build/debug/tests/test_persist
```

The suite has about 410,000 checks. Beyond hand-written cases, it cross-checks the algorithms
against brute-force references on randomly generated graphs:

- **Dijkstra** against Bellman-Ford.
- **A\*** against true distances, using a heuristic that is admissible but deliberately inconsistent.
- **Tarjan** against the reachability of every pair of nodes.

---

## Quick Start

```sh
make
alias goalgraph=./build/debug/goalgraph      # optional convenience
```

The sample dataset `data/sample_goals.dat` describes a CS student in the fall of their third
year. It has 36 goals and 55 edges:

- 24 courses in a prerequisite tree, from `cs101-intro-programming` through the two-semester
  capstone
- 8 side projects
- 4 career goals

All four statuses and all five edge types appear.

**What should I work on next?**

```text
$ goalgraph -f data/sample_goals.dat recommend top=5
Top 5 recommended goal(s):
   1. proj-goalgraph                      score 0.778  (enjoy 10, effort 7, achievability 1.00) *
   2. proj-ray-tracer                     score 0.778  (enjoy 9, effort 6, achievability 1.00)
   3. proj-open-source-contribution       score 0.741  (enjoy 7, effort 5, achievability 1.00)
   4. cs350-machine-learning              score 0.704  (enjoy 9, effort 8, achievability 1.00)
   5. cs310-operating-systems             score 0.667  (enjoy 9, effort 9, achievability 1.00) *
  (* already in progress)
```

**What's a valid order for everything I haven't finished?**

```text
$ goalgraph -f data/sample_goals.dat order remaining
Valid order following PREREQ edges (unfinished goals only):
    1. cs301-algorithms                    [in_progress]
    2. cs310-operating-systems             [in_progress]
    3. cs320-databases                     [in_progress]
    4. cs330-computer-networks             [not_started]
    ...
```

**What's the fastest route (in weeks) to the capstone?** A* search:

```text
$ goalgraph -f data/sample_goals.dat path cs101-intro-programming cs491-capstone-project-2 weight=time astar
Best path from cs101-intro-programming to cs491-capstone-project-2 (A*, PREREQ,ENABLES edges): 4 step(s), total time 60
    1. cs101-intro-programming [completed]
    2. cs102-object-oriented-programming [completed]
    3. cs230-software-engineering [completed]
    4. cs490-capstone-project-1 [not_started]
    5. cs491-capstone-project-2 [not_started]
```

**Which goals are tangled up with each other?**

```text
$ goalgraph -f data/sample_goals.dat cycles
Cycle group 1 (3 goals): cs350-machine-learning cs450-deep-learning proj-kaggle-competition
Cycle group 2 (2 goals): career-summer-internship-2027 career-undergrad-research-2027
```

**Change the plan.** One-shot commands save back to the `-f` file automatically, so try this
on a copy:

```sh
cp data/sample_goals.dat my_plan.dat
goalgraph -f my_plan.dat done cs301-algorithms
goalgraph -f my_plan.dat add "learn rust" category=side-project effort=6 enjoyment=9
goalgraph -f my_plan.dat link cs220-systems-programming-in-c "learn rust" type=enables time=8
```

**Interactive shell:**

```text
$ goalgraph -f my_plan.dat
GoalGraph interactive shell. Type 'help' for commands, 'quit' to exit.
goalgraph> show cs301-algorithms
cs301-algorithms
  category:  year3-fall
  status:    completed
  ...
goalgraph> recommend blocked top=10
goalgraph> save
goalgraph> quit
```

**Scripting:** pipe commands into the shell. `-q` turns off the banner and prompt:

```sh
printf 'load data/sample_goals.dat\norder\nrecommend\n' | goalgraph -q
```

Exit status is `0` on success, `1` if a command failed, and `2` for invalid command-line usage.

---

## CLI Reference

```text
goalgraph [-f FILE] [-q] [COMMAND [ARGS...]]
  -f FILE      load FILE first (one-shot commands that change the graph save it back)
  -q           quiet shell: no banner or prompt
  -h, --help   usage;  --version  version
```

Commands take positional arguments followed by `key=value` options. Quote arguments that
contain spaces: use `"..."` (which accepts `\"` and `\\`) or `'...'` (taken literally). A `#`
starts a comment. Run `help <command>` for full usage.

| Command | Usage | Description |
|---|---|---|
| `help` | `[command]` | List commands or show one command's usage |
| `load` | `<file> [force]` | Load a graph (refuses to discard unsaved changes without `force`) |
| `save` | `[file]` | Atomic save to `file`, or to the file last loaded or saved |
| `new` | `[force]` | Start an empty graph |
| `list` | `[status=S] [category=C]` | Table of goals, optionally filtered |
| `show` | `<id>` | One goal's details, incoming edges and outgoing edges |
| `add` | `<id> [category=C] [status=S] [effort=1-10] [enjoyment=1-10]` | Add a goal |
| `edit` | `<id> [id=NEW] [category=C] [status=S] [effort=N] [enjoyment=N]` | Change or rename a goal |
| `done` | `<id>` | Mark a goal completed |
| `remove` | `<id>` | Delete a goal and all of its edges |
| `link` | `<from> <to> [type=T] [effort=F] [enjoyment=F] [achievability=F] [similarity=F] [time=F]` | Add an edge (default `type=prereq`). Warns if it creates a prerequisite cycle |
| `unlink` | `<from> <to> [type=T[,T…]\|all]` | Remove edges (default `prereq`) |
| `edges` | `[id]` | List all edges, or those touching one goal |
| `order` | `[types=T[,T…]\|all] [remaining]` | Topological order (default: `prereq` edges). Prints the offending cycle if there is one |
| `path` | `<from> <to> [weight=W] [types=…] [astar]` | Cheapest path. `W` is `hops` (default), `effort`, `enjoyment`, `achievability`, `similarity` or `time`. Default types: `prereq,enables` |
| `recommend` | `[top=N] [blocked] [no-in-progress] [enjoyment=W] [effort=W] [achievability=W] [penalty=P]` | Ranked next goals (default `top=5`; `top=0` shows all) |
| `cycles` | `[types=…]` | Strongly connected components with more than one goal |
| `export` | `<file.dot> [cluster] [highlight] [title=TEXT]` | Graphviz export |
| `quit` / `exit` | | Leave the shell. With unsaved changes it warns once; a second `quit` discards them |

**Statuses:** `not_started`, `in_progress`, `completed`, `abandoned`. The aliases `todo`,
`active`, `doing` and `done` also work.
**Edge types:** `prereq`, `similar`, `enables`, `conflicts`, `custom`.
Names are case-insensitive, and `-` and `_` are interchangeable.

### How recommendations are scored

Candidates are goals that are `not_started`, plus `in_progress` goals unless you pass
`no-in-progress`. For each candidate:

```text
enjoyment_n   = (enjoyment - 1) / 9
ease_n        = (10 - effort) / 9
readiness     = prerequisite credit / number of prerequisites   (1 if there are none)
                where a completed prerequisite gives credit 1, an in-progress one 0.5,
                and anything else 0
achievability = readiness × penalty    if the goal CONFLICTS with an in-progress goal
              = readiness              otherwise
score         = (wE·enjoyment_n + wF·ease_n + wA·achievability) / (wE + wF + wA)
```

The weights `wE`, `wF` and `wA` default to 1 and `penalty` defaults to 0.5. Goals with any
unfinished prerequisite are *blocked* and left out unless you pass `blocked`. Ties are broken
by node index, so rankings are the same on every run.

### The A\* heuristic

`path … astar` estimates the remaining cost from any node other than the target as the cheapest
selected edge that leads *into* the target. Every path has to end with one of those edges, so
the estimate never exceeds the real cost, and it never drops by more than one edge's cost from a
node to the next. That means A* always returns the same optimal cost as Dijkstra.

---

## Graphviz Visualization

Export the graph, then render it with Graphviz's `dot`:

```sh
goalgraph -f data/sample_goals.dat export goals.dot cluster title="CS degree plan"
dot -Tsvg goals.dot -o goals.svg        # or -Tpng goals.dot -o goals.png
```

To highlight a route, run `path` and then `export … highlight` in the same session:

```sh
printf '%s\n' \
  'load data/sample_goals.dat' \
  'path cs101-intro-programming cs450-deep-learning weight=time' \
  'export route.dot cluster highlight title="Road to deep learning"' \
  | goalgraph -q
dot -Tsvg route.dot -o route.svg
```

How the output looks:

| Element | Style |
|---|---|
| `completed` goal | green fill |
| `in_progress` goal | yellow fill |
| `not_started` goal | white fill |
| `abandoned` goal | gray fill, dashed border, gray text |
| `PREREQ` edge | dark solid arrow |
| `ENABLES` edge | blue solid arrow |
| `SIMILAR` edge | gray dashed line, no arrowhead |
| `CONFLICTS` edge | red bold line, tee (⊣) arrowhead |
| `CUSTOM` edge | purple dotted arrow |
| Highlighted path | bold orange nodes and edges |
| `cluster` option | a dashed box around each category (for example `year1-fall`) |

Layouts run left to right (`rankdir=LR`). Hovering over a goal in an SVG viewer shows its
category.

---

## Data File Format

Graphs are stored as UTF-8 text, one record per line (format version 1):

```text
goalgraph 1
# node <id> <category> <status> <effort> <enjoyment> <created_at> <updated_at>
node "cs201-data-structures" "year2-fall" completed 7 8 1724112000 1765843200
node "cs301-algorithms"      "year3-fall" in_progress 9 8 1724112000 1788998400
# edge <source> <target> <type> <effort> <enjoyment> <achievability> <similarity> <time_cost>
edge "cs201-data-structures" "cs301-algorithms" PREREQ 9 8 0.75 0.8 15
```

- The `goalgraph 1` header comes first. Blank lines and `#` comments are ignored. LF and CRLF
  line endings both work.
- IDs and categories are quoted strings with the escapes `\" \\ \n \r \t \xHH`.
- A node must be declared before any edge that refers to it.
- Weights are written with `%.9g`, so every `float` value round-trips bit for bit.
- Saving preserves node indices and edge order, so a graph reloads exactly as it was saved.
- Malformed input is rejected with a line number and a reason. For example:
  `data/x.dat:12: unknown node id "cs999"`. The limit is 1 MiB per line, and NUL bytes are
  rejected.

**Atomic saves:** `persist_save()` writes `<file>.tmp`, flushes and closes it (checking for
errors), and then `rename()`s it over `<file>`. If any step fails, the original file is left
untouched and the temporary file is deleted.

---

## Library API

Every module can be used on its own. Here is a minimal program that loads the sample data and
prints a study order:

```c
#include "algo.h"
#include "graph.h"
#include "persist.h"
#include <stdio.h>

int main(void)
{
    graph *g = NULL;
    persist_error err;
    if (persist_load("data/sample_goals.dat", &g, &err) != PERSIST_OK) {
        fprintf(stderr, "line %zu: %s\n", err.line, err.message);
        return 1;
    }

    algo_order order = {0};
    if (algo_topo_sort(g, algo_edge_bit(EDGE_PREREQ), &order) == ALGO_OK) {
        for (size_t i = 0; i < order.count; i++) {
            graph_node_info info;
            if (graph_get_node(g, order.nodes[i], &info) == GRAPH_OK) {
                printf("%zu. %s\n", i + 1, info.id);
            }
        }
    }

    algo_order_free(&order);   /* result structs own their arrays */
    graph_free(g);
    return 0;
}
```

API conventions:

- Fallible functions return a status enum (`graph_status`, `algo_status`, `persist_status`,
  `export_status`) or `NULL`.
- Pointer arguments are validated at every public entry point.
- Result structs are always left valid, and empty on failure, so calling their `*_free()` is
  always safe.
- Each header documents the lifetime of every borrowed pointer.

---

## Known Limitations

- **No protection against power loss.** Standard C has no `fsync`, so a save is atomic if the
  program crashes but might not survive a sudden power cut. Replacing the file is atomic on
  POSIX, including WSL. A native Windows build would fail to replace an existing file.
- **Looking up a goal by ID is linear.** Loading a file therefore takes O(E·V) time. That's
  instant at planning scale (hundreds of goals); a hash index would be the next step for very
  large graphs.
- **Don't run concurrent saves to the same file.** Two simultaneous saves to one path share the
  same `.tmp` name.
