#include "pux/cli.h"
#include "pux/package.h"
#include "pux/resolver.h"
#include "pux/db.h"
#include "pux/container.h"
#include "pux/builder.h"
#include "pux/extract.h"
#include "pux/transaction.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PUX_VERSION "0.10.1-dev"

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
        "  install     Install a package or resolve one from a repository\n"
        "  remove      Remove packages\n"
        "  update      Refresh repository metadata\n"
        "  upgrade     Upgrade installed packages\n"
        "  list        List installed packages\n"
        "  verify      Verify an installed package\n"
        "  resolve     Resolve package dependencies (no changes made)\n\n"
        "Package operations:\n"
        "  package     Inspect, validate, and extract packages\n  db          Inspect and maintain the local package database\n\n"
        "Package creation:\n"
        "  build       Build a .pux package\n"
        "  repo        Repository management\n\n"
        "Other:\n"
        "  help        Show this help\n"
        "  version     Show version information\n",
        program);
}

static int has_suffix(const char *value, const char *suffix)
{
    const size_t value_length = strlen(value);
    const size_t suffix_length = strlen(suffix);
    return value_length >= suffix_length &&
           strcmp(value + value_length - suffix_length, suffix) == 0;
}

static int command_not_implemented(const char *command)
{
    fprintf(stderr, "pux: command '%s' is not implemented yet\n", command);
    return 2;
}

static int build_command(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "Usage: %s build <manifest> <payload-dir> <output.pux>\n", argv[0]);
        return 2;
    }

    const char *manifest = argv[2];
    const char *payload = argv[3];
    const char *output = argv[4];
    char error[512] = {0};

    if (pux_package_build(manifest, payload, output, error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: build failed: %s\n", error);
        return 1;
    }

    printf("package: %s\n", output);
    return 0;
}

static int package_command(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s package <validate|info|extract> ...\n", argv[0]);
        return 2;
    }

    const char *operation = argv[2];
    if (strcmp(operation, "extract") == 0) {
        if (argc != 5) {
            fprintf(stderr, "Usage: %s package extract <package.pux> <destination>\n", argv[0]);
            return 2;
        }

        char error[512] = {0};
        if (pux_package_archive_extract(argv[3], argv[4], error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: extraction failed: %s\n", error);
            return 1;
        }
        printf("extracted: %s\n", argv[4]);
        return 0;
    }

    if (strcmp(operation, "validate") != 0 && strcmp(operation, "info") != 0) {
        fprintf(stderr, "pux: unknown package operation '%s'\n", operation);
        return 2;
    }

    if (argc != 4) {
        fprintf(stderr, "Usage: %s package %s <manifest-or-package>\n", argv[0], operation);
        return 2;
    }

    struct pux_package_manifest manifest;
    char error[512] = {0};

    const char *path = argv[3];
    int read_result;
    if (path[0] != '\0' && has_suffix(path, ".pux")) {
        read_result = pux_package_archive_validate(path, &manifest, error, sizeof(error));
    } else {
        read_result = pux_package_manifest_read_file(path, &manifest, error, sizeof(error));
    }

    if (read_result != 0) {
        fprintf(stderr, "pux: cannot read package: %s\n", error);
        return 1;
    }

    if (pux_package_manifest_validate(&manifest, error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: invalid manifest: %s\n", error);
        pux_package_manifest_free(&manifest);
        return 1;
    }

    if (strcmp(operation, "info") == 0) {
        pux_package_manifest_print(&manifest);
    } else {
        puts("manifest: valid");
    }

    pux_package_manifest_free(&manifest);
    return 0;
}


static const char *database_root(void)
{
    const char *value = getenv("PUX_DB_ROOT");
    return (value != NULL && value[0] != '\0') ? value : PUX_DB_DEFAULT_ROOT;
}

static const char *installation_root(void)
{
    const char *value = getenv("PUX_ROOT");
    return (value != NULL && value[0] != '\0') ? value : PUX_ROOT_DEFAULT;
}

static int manifests_match_exact(const struct pux_package_manifest *left,
                                  const struct pux_package_manifest *right)
{
    return strcmp(left->name, right->name) == 0 &&
           strcmp(left->version, right->version) == 0 &&
           left->release == right->release &&
           strcmp(left->arch, right->arch) == 0;
}

static int install_from_repository(const char *package_name, const char *repository_dir)
{
    char error[512] = {0};
    struct pux_resolve_plan plan = {0};

    if (pux_resolve_package_plan(package_name, repository_dir, &plan,
                                 error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: dependency resolution failed: %s\n", error);
        return 1;
    }

    /* Validate every plan item and preflight already-installed packages before
     * changing the filesystem. The actual transaction remains per-package
     * atomic; cross-package rollback is a later milestone. */
    for (size_t i = 0U; i < plan.count; ++i) {
        struct pux_package_manifest manifest;
        if (pux_package_archive_validate(plan.package_paths[i], &manifest,
                                         error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository package validation failed: %s\n", error);
            pux_resolve_plan_free(&plan);
            return 1;
        }
        struct pux_package_manifest installed;
        struct pux_db_file_list installed_files = {0};
        char db_error[512] = {0};
        const int installed_result = pux_db_read_package(database_root(), manifest.name,
                                                         &installed, &installed_files,
                                                         db_error, sizeof(db_error));
        if (installed_result == 0) {
            if (!manifests_match_exact(&manifest, &installed)) {
                fprintf(stderr,
                        "pux: package already installed at a different version: %s\n",
                        manifest.name);
                pux_package_manifest_free(&installed);
                pux_db_file_list_free(&installed_files);
                pux_package_manifest_free(&manifest);
                pux_resolve_plan_free(&plan);
                return 1;
            }
        }
        pux_package_manifest_free(&installed);
        pux_db_file_list_free(&installed_files);
        pux_package_manifest_free(&manifest);
    }

    for (size_t i = 0U; i < plan.count; ++i) {
        struct pux_package_manifest manifest;
        if (pux_package_archive_validate(plan.package_paths[i], &manifest,
                                         error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository package validation failed: %s\n", error);
            pux_resolve_plan_free(&plan);
            return 1;
        }

        struct pux_package_manifest installed;
        struct pux_db_file_list installed_files = {0};
        char db_error[512] = {0};
        const int installed_result = pux_db_read_package(database_root(), manifest.name,
                                                         &installed, &installed_files,
                                                         db_error, sizeof(db_error));
        const int already_exact = installed_result == 0 && manifests_match_exact(&manifest, &installed);
        pux_package_manifest_free(&installed);
        pux_db_file_list_free(&installed_files);

        if (already_exact != 0) {
            printf("already installed: %s %s-%u %s\n",
                   manifest.name, manifest.version, manifest.release, manifest.arch);
            pux_package_manifest_free(&manifest);
            continue;
        }

        if (pux_install_package(plan.package_paths[i], installation_root(),
                                 database_root(), error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: install failed: %s\n", error);
            pux_package_manifest_free(&manifest);
            pux_resolve_plan_free(&plan);
            return 1;
        }
        printf("installed: %s %s-%u %s\n",
               manifest.name, manifest.version, manifest.release, manifest.arch);
        pux_package_manifest_free(&manifest);
    }

    pux_resolve_plan_free(&plan);
    return 0;
}

static int install_command(int argc, char **argv)
{
    if (argc == 3) {
        char error[512] = {0};
        if (pux_install_package(argv[2], installation_root(), database_root(),
                                error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: install failed: %s\n", error);
            return 1;
        }
        printf("installed: %s\n", argv[2]);
        return 0;
    }

    if (argc == 4) {
        return install_from_repository(argv[2], argv[3]);
    }

    fprintf(stderr, "Usage: %s install <package.pux>\n", argv[0]);
    fprintf(stderr, "       %s install <package-name> <repository-dir>\n", argv[0]);
    return 2;
}

static int db_command(int argc, char **argv)
{
    const char *root = database_root();
    char error[512] = {0};

    if (argc < 3) {
        fprintf(stderr,
                "Usage: %s db <list|info|register|unregister> ...\n",
                argv[0]);
        return 2;
    }

    const char *operation = argv[2];
    if (strcmp(operation, "list") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s db list\n", argv[0]);
            return 2;
        }
        if (pux_db_list_packages(root, stdout, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: database list failed: %s\n", error);
            return 1;
        }
        return 0;
    }

    if (strcmp(operation, "info") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s db info <name>\n", argv[0]);
            return 2;
        }
        struct pux_package_manifest manifest;
        struct pux_db_file_list files = {0};
        if (pux_db_read_package(root, argv[3], &manifest, &files, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: database info failed: %s\n", error);
            return 1;
        }
        pux_package_manifest_print(&manifest);
        puts("files=");
        for (size_t i = 0U; i < files.count; ++i) {
            printf("%c %s\n", files.items[i].type, files.items[i].path);
        }
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
        return 0;
    }

    if (strcmp(operation, "register") == 0) {
        if (argc != 5) {
            fprintf(stderr, "Usage: %s db register <manifest> <files-list>\n", argv[0]);
            return 2;
        }
        struct pux_package_manifest manifest;
        struct pux_db_file_list files = {0};
        if (pux_package_manifest_read_file(argv[3], &manifest, error, sizeof(error)) != 0 ||
            pux_package_manifest_validate(&manifest, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot register package: %s\n", error);
            return 1;
        }
        if (pux_db_read_file_list(argv[4], &files, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot register package: %s\n", error);
            pux_package_manifest_free(&manifest);
            return 1;
        }
        if (pux_db_register_package(root, &manifest, &files, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot register package: %s\n", error);
            pux_package_manifest_free(&manifest);
            pux_db_file_list_free(&files);
            return 1;
        }
        printf("registered: %s\n", manifest.name);
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
        return 0;
    }

    if (strcmp(operation, "unregister") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s db unregister <name>\n", argv[0]);
            return 2;
        }
        if (pux_db_unregister_package(root, argv[3], error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot unregister package: %s\n", error);
            return 1;
        }
        printf("unregistered: %s\n", argv[3]);
        return 0;
    }

    fprintf(stderr, "pux: unknown database operation '%s'\n", operation);
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

    if (strcmp(command, "package") == 0) {
        return package_command(argc, argv);
    }

    if (strcmp(command, "install") == 0) {
        return install_command(argc, argv);
    }

    if (strcmp(command, "db") == 0) {
        return db_command(argc, argv);
    }

    if (strcmp(command, "build") == 0) {
        return build_command(argc, argv);
    }

    if (strcmp(command, "remove") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s remove <package-name>\n", argv[0]);
            return 2;
        }
        char error[512] = {0};
        if (pux_remove_package(argv[2], installation_root(), database_root(),
                               error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: remove failed: %s\n", error);
            return 1;
        }
        printf("removed: %s\n", argv[2]);
        return 0;
    }

    if (strcmp(command, "list") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Usage: %s list\n", argv[0]);
            return 2;
        }
        char error[512] = {0};
        if (pux_db_list_packages(database_root(), stdout, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot list installed packages: %s\n", error);
            return 1;
        }
        return 0;
    }

    if (strcmp(command, "info") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s info <name>\n", argv[0]);
            return 2;
        }
        char error[512] = {0};
        struct pux_package_manifest manifest;
        struct pux_db_file_list files = {0};
        if (pux_db_read_package(database_root(), argv[2], &manifest, &files, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot read installed package: %s\n", error);
            return 1;
        }
        pux_package_manifest_print(&manifest);
        puts("files=");
        for (size_t i = 0U; i < files.count; ++i) {
            printf("%c %s\n", files.items[i].type, files.items[i].path);
        }
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
        return 0;
    }

    if (strcmp(command, "resolve") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s resolve <package-name> <repository-dir>\n", argv[0]);
            return 2;
        }
        char error[512] = {0};
        if (pux_resolve_package(argv[2], argv[3], stdout, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: dependency resolution failed: %s\n", error);
            return 1;
        }
        return 0;
    }

    if (strcmp(command, "search") == 0 || strcmp(command, "update") == 0 ||
        strcmp(command, "upgrade") == 0 || strcmp(command, "verify") == 0 ||
        strcmp(command, "repo") == 0) {
        return command_not_implemented(command);
    }

    fprintf(stderr, "pux: unknown command '%s'\n", command);
    fprintf(stderr, "pux: run '%s help' for usage\n", argv[0]);
    return 2;
}
