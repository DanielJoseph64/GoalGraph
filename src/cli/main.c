/*
 * goalgraph: command-line entry point.
 *
 *   goalgraph [-f FILE] [-q]                  interactive shell (commands from stdin)
 *   goalgraph [-f FILE] COMMAND [ARGS...]     run one command and exit
 *
 * In one-shot mode, a command that changes the graph loaded with -f is saved
 * back to FILE. All command logic lives in the cli module (see cli.h).
 */

#include "cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define GOALGRAPH_VERSION "1.0.0"

enum { EXIT_OK = 0, EXIT_FAILED = 1, EXIT_USAGE = 2 };

static int print_usage(FILE *stream, const char *prog)
{
    const int written = fprintf(
        stream,
        "Usage: %s [-f FILE] [-q] [COMMAND [ARGS...]]\n"
        "\n"
        "Without COMMAND, starts an interactive shell that reads commands from stdin.\n"
        "With COMMAND, runs that single command and exits. If -f was given and the\n"
        "command changed the graph, FILE is saved back atomically.\n"
        "\n"
        "Options:\n"
        "  -f FILE      load FILE first\n"
        "  -q           quiet shell: no banner or prompt (for piped scripts)\n"
        "  -h, --help   show this help\n"
        "  --version    show the version\n"
        "\n"
        "Examples:\n"
        "  %s -f data/sample_goals.dat recommend top=3\n"
        "  %s -f data/sample_goals.dat order remaining\n"
        "  %s -f my_goals.dat add \"learn rust\" category=side-project effort=6\n"
        "  %s -f data/sample_goals.dat export goals.dot cluster\n"
        "\n"
        "Run '%s help' to list the commands.\n",
        prog, prog, prog, prog, prog, prog);
    return written < 0 ? EXIT_FAILED : EXIT_OK;
}

typedef struct cli_args {
    char *file;
    bool quiet;
    int command_index; /* first argument of the command, or argc if none */
} cli_args;

/* Returns -1 to continue, or an exit status to stop with. */
static int parse_args(int argc, char *argv[], cli_args *out)
{
    const char *prog = argc > 0 ? argv[0] : "goalgraph";
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-f") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -f needs a file name\n");
                return EXIT_USAGE;
            }
            out->file = argv[++i];
        } else if (strcmp(a, "-q") == 0) {
            out->quiet = true;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            return print_usage(stdout, prog);
        } else if (strcmp(a, "--version") == 0) {
            return printf("goalgraph %s\n", GOALGRAPH_VERSION) < 0 ? EXIT_FAILED : EXIT_OK;
        } else if (strcmp(a, "--") == 0) {
            i++;
            break;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "error: unknown option '%s' (see %s --help)\n", a, prog);
            return EXIT_USAGE;
        } else {
            break;
        }
    }
    out->command_index = i;
    return -1;
}

static int run_one_shot(cli_session *s, const cli_args *args, int argc, char *argv[])
{
    const cli_result result =
        cli_run_args(s, argc - args->command_index, &argv[args->command_index]);
    if (result == CLI_ERROR) {
        return EXIT_FAILED;
    }
    if (!cli_session_modified(s)) {
        return EXIT_OK;
    }
    if (args->file == NULL) {
        fprintf(stderr, "note: changes were not saved (use -f FILE to save them)\n");
        return EXIT_OK;
    }
    char save_command[] = "save";
    char *save_argv[] = {save_command, NULL};
    return cli_run_args(s, 1, save_argv) == CLI_OK ? EXIT_OK : EXIT_FAILED;
}

int main(int argc, char *argv[])
{
    cli_args args = {0};
    const int early_exit = parse_args(argc, argv, &args);
    if (early_exit >= 0) {
        return early_exit;
    }

    cli_session *s = cli_session_create(stdout, stderr);
    if (s == NULL) {
        fprintf(stderr, "error: out of memory\n");
        return EXIT_FAILED;
    }

    int status = EXIT_OK;
    if (args.file != NULL) {
        char load_command[] = "load";
        char *load_argv[] = {load_command, args.file, NULL};
        if (cli_run_args(s, 2, load_argv) != CLI_OK) {
            status = EXIT_FAILED;
        }
    }
    if (status == EXIT_OK) {
        if (args.command_index < argc) {
            status = run_one_shot(s, &args, argc, argv);
        } else {
            status = cli_repl(s, stdin, !args.quiet) == CLI_OK ? EXIT_OK : EXIT_FAILED;
        }
    }

    cli_session_free(s);
    return status;
}
