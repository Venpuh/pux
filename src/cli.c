#include "pux/cli.h"

#include <stdio.h>
#include <string.h>

#define PUX_VERSION "0.1.0-dev"

static void print_version(void)
{
    puts("pux " PUX_VERSION);
}

static void print_help(const char *program)
{
    printf(
        "Usage: %s <command> [options]\n\n"
        "Package management:\n"
        "  search      Search configured repositories\n"
        "  info        Show package information\n"
        "  install     Install packages\n"
        "  remove      Remove packages\n"
        "  update      Refresh repository metadata\n"
        "  upgrade     Upgrade installed packages\n"
        "  list        List installed packages\n"
        "  verify      Verify an installed package\n\n"
        "Package creation:\n"
        "  build       Build a .pux package\n"
        "  repo        Repository management\n\n"
        "Other:\n"
        "  help        Show this help\n"
        "  version     Show version information\n",
        program);
}

static int command_not_implemented(const char *command)
{
    fprintf(stderr, "pux: command '%s' is not implemented yet\n", command);
    return 2;
}

int pux_cli_run(int argc, char **argv)
{
    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    const char *command = argv[1];

    if (strcmp(command, "help") == 0 || strcmp(command, "--help") == 0 ||
        strcmp(command, "-h") == 0) {
        print_help(argv[0]);
        return 0;
    }

    if (strcmp(command, "version") == 0 || strcmp(command, "--version") == 0 ||
        strcmp(command, "-V") == 0) {
        print_version();
        return 0;
    }

    if (strcmp(command, "search") == 0 || strcmp(command, "info") == 0 ||
        strcmp(command, "install") == 0 || strcmp(command, "remove") == 0 ||
        strcmp(command, "update") == 0 || strcmp(command, "upgrade") == 0 ||
        strcmp(command, "list") == 0 || strcmp(command, "verify") == 0 ||
        strcmp(command, "build") == 0 || strcmp(command, "repo") == 0) {
        return command_not_implemented(command);
    }

    fprintf(stderr, "pux: unknown command '%s'\n", command);
    fprintf(stderr, "pux: run '%s help' for usage\n", argv[0]);
    return 2;
}
