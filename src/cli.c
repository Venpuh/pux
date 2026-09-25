#define _POSIX_C_SOURCE 200809L
#include "pux/cli.h"
#include "pux/package.h"
#include "pux/resolver.h"
#include "pux/db.h"
#include "pux/container.h"
#include "pux/builder.h"
#include "pux/extract.h"
#include "pux/transaction.h"
#include "pux/repo.h"
#include "pux/sha256.h"
#include "pux/signature.h"
#include "pux/trust.h"
#include "pux/update.h"
#include "pux/transport.h"
#include "pux/config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#define PUX_VERSION "0.19.0-dev"

static const char *trusted_keys_root(void);
static int repository_signature_required(void);
static int verify_trusted_repository(const char *repository_dir);

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
        "  package     Inspect, validate, extract, and checksum packages\n"
        "  db          Inspect and maintain the local package database\n\n"
        "Package creation:\n"
        "  build       Build a .pux package\n"
        "  repo        Repository management\n  keygen      Generate an Ed25519 repository keypair\n  trust       Manage trusted repository keys\n\n"
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



enum cli_version_compare_result {
    CLI_VERSION_OLDER = -1,
    CLI_VERSION_EQUAL = 0,
    CLI_VERSION_NEWER = 1
};

static int cli_compare_version_part(const char **cursor,
                                    char *buffer, size_t buffer_size,
                                    int *numeric)
{
    const char *p = *cursor;
    while (*p != '\0' && !isalnum((unsigned char)*p)) ++p;
    if (*p == '\0') {
        buffer[0] = '\0';
        *numeric = 0;
        *cursor = p;
        return 0;
    }
    const char *start = p;
    while (*p != '\0' && isalnum((unsigned char)*p)) ++p;
    const size_t length = (size_t)(p - start);
    if (length + 1U > buffer_size) return -1;
    memcpy(buffer, start, length);
    buffer[length] = '\0';
    *numeric = 1;
    for (size_t i = 0U; i < length; ++i) {
        if (!isdigit((unsigned char)buffer[i])) {
            *numeric = 0;
            break;
        }
    }
    *cursor = p;
    return 1;
}

static int cli_compare_versions(const char *left, const char *right)
{
    const char *l = left;
    const char *r = right;
    for (;;) {
        char lp[128];
        char rp[128];
        int ln = 0;
        int rn = 0;
        const int lm = cli_compare_version_part(&l, lp, sizeof(lp), &ln);
        const int rm = cli_compare_version_part(&r, rp, sizeof(rp), &rn);
        if (lm < 0 || rm < 0) return 0;
        if (lm == 0 && rm == 0) return CLI_VERSION_EQUAL;
        if (lm == 0) return CLI_VERSION_OLDER;
        if (rm == 0) return CLI_VERSION_NEWER;
        if (ln != 0 && rn != 0) {
            const char *lz = lp;
            const char *rz = rp;
            while (*lz == '0') ++lz;
            while (*rz == '0') ++rz;
            const size_t llen = strlen(lz);
            const size_t rlen = strlen(rz);
            if (llen != rlen) return llen < rlen ? CLI_VERSION_OLDER : CLI_VERSION_NEWER;
            const int cmp = strcmp(lz, rz);
            if (cmp != 0) return cmp < 0 ? CLI_VERSION_OLDER : CLI_VERSION_NEWER;
        } else if (ln != rn) {
            return ln != 0 ? CLI_VERSION_NEWER : CLI_VERSION_OLDER;
        } else {
            const int cmp = strcmp(lp, rp);
            if (cmp != 0) return cmp < 0 ? CLI_VERSION_OLDER : CLI_VERSION_NEWER;
        }
    }
}

static int manifest_is_newer(const struct pux_package_manifest *candidate,
                             const struct pux_package_manifest *installed)
{
    const int version = cli_compare_versions(candidate->version, installed->version);
    if (version > 0) return 1;
    if (version < 0) return 0;
    return candidate->release > installed->release;
}

static const char *database_root(void);
static const char *installation_root(void);

static int upgrade_from_repository_policy(const char *package_name,
                                          const char *repository_dir,
                                          int require_signed)
{
    char error[512] = {0};
    struct pux_resolve_plan plan = {0};
    if (require_signed != 0 && verify_trusted_repository(repository_dir) != 0) return 1;
    if (pux_resolve_package_plan(package_name, repository_dir, &plan,
                                 error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: dependency resolution failed: %s\n", error);
        return 1;
    }

    int root_seen = 0;
    for (size_t i = 0U; i < plan.count; ++i) {
        if (pux_repo_verify_package(repository_dir, plan.package_paths[i], error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository package verification failed: %s\n", error);
            pux_resolve_plan_free(&plan);
            return 1;
        }
        struct pux_package_manifest candidate;
        if (pux_package_archive_validate(plan.package_paths[i], &candidate,
                                         error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository package validation failed: %s\n", error);
            pux_resolve_plan_free(&plan);
            return 1;
        }

        if (strcmp(candidate.name, package_name) == 0) root_seen = 1;

        struct pux_package_manifest installed;
        struct pux_db_file_list installed_files = {0};
        char db_error[512] = {0};
        const int installed_result = pux_db_read_package(database_root(), candidate.name,
                                                         &installed, &installed_files,
                                                         db_error, sizeof(db_error));
        if (installed_result != 0) {
            if (pux_install_package(plan.package_paths[i], installation_root(),
                                    database_root(), error, sizeof(error)) != 0) {
                fprintf(stderr, "pux: install failed: %s\n", error);
                pux_package_manifest_free(&candidate);
                pux_resolve_plan_free(&plan);
                return 1;
            }
            printf("installed: %s %s-%u %s\n",
                   candidate.name, candidate.version, candidate.release, candidate.arch);
        } else if (manifest_is_newer(&candidate, &installed) != 0) {
            const char *old_version = installed.version;
            const unsigned old_release = installed.release;
            if (pux_upgrade_package(plan.package_paths[i], installation_root(),
                                    database_root(), error, sizeof(error)) != 0) {
                if (error[0] == '\0') {
                    (void)snprintf(error, sizeof(error), "upgrade failed");
                }
                fprintf(stderr, "pux: upgrade failed: %s\n", error);
                (void)old_release;
                (void)old_version;
                pux_package_manifest_free(&installed);
                pux_db_file_list_free(&installed_files);
                pux_package_manifest_free(&candidate);
                pux_resolve_plan_free(&plan);
                return 1;
            }
            printf("upgraded: %s %s-%u -> %s-%u\n",
                   candidate.name, old_version, old_release,
                   candidate.version, candidate.release);
        } else {
            printf("already up to date: %s %s-%u %s\n",
                   candidate.name, installed.version, installed.release, installed.arch);
        }

        pux_package_manifest_free(&installed);
        pux_db_file_list_free(&installed_files);
        pux_package_manifest_free(&candidate);
    }

    if (root_seen == 0) {
        pux_resolve_plan_free(&plan);
        fprintf(stderr, "pux: requested package was not present in resolution plan\n");
        return 1;
    }
    pux_resolve_plan_free(&plan);
    return 0;
}

static int upgrade_from_repository(const char *package_name, const char *repository_dir)
{
    return upgrade_from_repository_policy(package_name, repository_dir,
                                          repository_signature_required());
}

static int trust_command(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s trust <add|remove|list> ...\n", argv[0]);
        return 2;
    }
    char error[512] = {0};
    const char *root = trusted_keys_root();
    const char *operation = argv[2];
    if (strcmp(operation, "add") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s trust add <public-key>\n", argv[0]);
            return 2;
        }
        char keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
        if (pux_trust_add_key(root, argv[3], keyid, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot trust key: %s\n", error);
            return 1;
        }
        printf("trusted: %s\n", keyid);
        return 0;
    }
    if (strcmp(operation, "remove") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s trust remove <keyid>\n", argv[0]);
            return 2;
        }
        if (pux_trust_remove_key(root, argv[3], error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot remove trusted key: %s\n", error);
            return 1;
        }
        printf("untrusted: %s\n", argv[3]);
        return 0;
    }
    if (strcmp(operation, "list") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s trust list\n", argv[0]);
            return 2;
        }
        if (pux_trust_list_keys(root, stdout, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot list trusted keys: %s\n", error);
            return 1;
        }
        return 0;
    }
    fprintf(stderr, "pux: unknown trust operation '%s'\n", operation);
    return 2;
}

static int cli_parse_bool(const char *value, int *result)
{
    if (value == NULL || result == NULL) return -1;
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "on") == 0) { *result = 1; return 0; }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0 || strcmp(value, "off") == 0) { *result = 0; return 0; }
    return -1;
}

static int update_command(int argc, char **argv);

static int repo_config_command(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s repo <create|validate|sign|verify|verify-trusted|add|remove|list|update> ...\n", argv[0]);
        return 2;
    }
    const char *operation = argv[2];
    char error[512] = {0};

    if (strcmp(operation, "add") == 0) {
        if (argc < 5 || argc > 8) {
            fprintf(stderr, "Usage: %s repo add <name> <url> [priority] [enabled] [require-signature]\n", argv[0]);
            return 2;
        }
        int priority = 100;
        int enabled = 1;
        int require_signature = 0;
        if (argc >= 6) {
            char *end = NULL;
            long value = strtol(argv[5], &end, 10);
            if (end == argv[5] || *end != '\0' || value < 0L || value > PUX_CONFIG_MAX_PRIORITY) {
                fprintf(stderr, "pux: invalid repository priority\n");
                return 2;
            }
            priority = (int)value;
        }
        if (argc >= 7) {
            if (cli_parse_bool(argv[6], &enabled) != 0) {
                fprintf(stderr, "pux: invalid repository enabled value\n");
                return 2;
            }
        }
        if (argc == 8) {
            if (cli_parse_bool(argv[7], &require_signature) != 0) {
                fprintf(stderr, "pux: invalid repository signature requirement\n");
                return 2;
            }
        }
        if (pux_repo_config_add(pux_repo_config_root(), pux_repo_cache_root(), argv[3], argv[4],
                                priority, enabled, require_signature, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot add repository: %s\n", error);
            return 1;
        }
        printf("added: %s\n", argv[3]);
        return 0;
    }
    if (strcmp(operation, "remove") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s repo remove <name>\n", argv[0]);
            return 2;
        }
        if (pux_repo_config_remove(pux_repo_config_root(), argv[3], error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot remove repository: %s\n", error);
            return 1;
        }
        printf("removed: %s\n", argv[3]);
        return 0;
    }
    if (strcmp(operation, "list") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s repo list\n", argv[0]);
            return 2;
        }
        struct pux_repo_config_list list = {0};
        if (pux_repo_config_load_all(pux_repo_config_root(), pux_repo_cache_root(), &list, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot load repository configuration: %s\n", error);
            return 1;
        }
        const int result = pux_repo_config_print(&list, stdout);
        pux_repo_config_list_free(&list);
        if (result != 0) {
            fprintf(stderr, "pux: cannot print repository configuration\n");
            return 1;
        }
        return 0;
    }
    if (strcmp(operation, "update") == 0) {
        if (argc != 3 && argc != 4) {
            fprintf(stderr, "Usage: %s repo update [name]\n", argv[0]);
            return 2;
        }
        if (argc == 4) {
            char *args[] = { argv[0], (char *)"update", argv[3] };
            return update_command(3, args);
        }
        return update_command(2, (char *[]) { argv[0], "update" });
    }
    return -2; /* let the caller continue with the legacy repo operations */
}

static int repo_command(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s repo <create|validate|sign|verify|verify-trusted> ...\n", argv[0]);
        return 2;
    }
    char error[512] = {0};
    const char *operation = argv[2];
    if (strcmp(operation, "add") == 0 || strcmp(operation, "remove") == 0 ||
        strcmp(operation, "list") == 0 || strcmp(operation, "update") == 0) {
        const int configured_result = repo_config_command(argc, argv);
        if (configured_result != -2) return configured_result;
    }
    if (strcmp(operation, "create") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s repo create <repository-dir>\n", argv[0]);
            return 2;
        }
        if (pux_repo_create_index(argv[3], error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository index creation failed: %s\n", error);
            return 1;
        }
        printf("index: %s/%s\n", argv[3], PUX_REPO_INDEX_NAME);
        return 0;
    }
    if (strcmp(operation, "sign") == 0) {
        if (argc != 5) {
            fprintf(stderr, "Usage: %s repo sign <repository-dir> <private-key>\n", argv[0]);
            return 2;
        }
        char index_path[4096];
        char signature_path[4096];
        const int index_len = snprintf(index_path, sizeof(index_path), "%s/%s", argv[3], PUX_REPO_INDEX_NAME);
        const int sig_len = snprintf(signature_path, sizeof(signature_path), "%s/%s.sig", argv[3], PUX_REPO_INDEX_NAME);
        if (index_len < 0 || sig_len < 0 || (size_t)index_len >= sizeof(index_path) || (size_t)sig_len >= sizeof(signature_path)) {
            fprintf(stderr, "pux: repository path is too long\n");
            return 1;
        }
        if (pux_repo_validate_index(argv[3], error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository validation failed before signing: %s\n", error);
            return 1;
        }
        if (pux_signature_sign_file(index_path, argv[4], signature_path, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository signing failed: %s\n", error);
            return 1;
        }
        printf("signature: %s\n", signature_path);
        return 0;
    }
    if (strcmp(operation, "verify-trusted") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s repo verify-trusted <repository-dir>\n", argv[0]);
            return 2;
        }
        char keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
        if (pux_trust_verify_repository(trusted_keys_root(), argv[3], keyid, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository trust verification failed: %s\n", error);
            return 1;
        }
        printf("signature: trusted %s\n", keyid);
        return 0;
    }
    if (strcmp(operation, "verify") == 0) {
        if (argc != 5) {
            fprintf(stderr, "Usage: %s repo verify <repository-dir> <public-key>\n", argv[0]);
            return 2;
        }
        char index_path[4096];
        char signature_path[4096];
        const int index_len = snprintf(index_path, sizeof(index_path), "%s/%s", argv[3], PUX_REPO_INDEX_NAME);
        const int sig_len = snprintf(signature_path, sizeof(signature_path), "%s/%s.sig", argv[3], PUX_REPO_INDEX_NAME);
        if (index_len < 0 || sig_len < 0 || (size_t)index_len >= sizeof(index_path) || (size_t)sig_len >= sizeof(signature_path)) {
            fprintf(stderr, "pux: repository path is too long\n");
            return 1;
        }
        if (pux_signature_verify_file(index_path, signature_path, argv[4], error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository signature verification failed: %s\n", error);
            return 1;
        }
        puts("signature: valid");
        return 0;
    }
    if (strcmp(operation, "validate") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s repo validate <repository-dir>\n", argv[0]);
            return 2;
        }
        if (pux_repo_validate_index(argv[3], error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository validation failed: %s\n", error);
            return 1;
        }
        puts("repository: valid");
        return 0;
    }
    fprintf(stderr, "pux: unknown repository operation '%s'\n", operation);
    return 2;
}

static int configured_repo_update(const struct pux_repo_config_entry *item)
{
    char error[512] = {0};
    const int require_signed = repository_signature_required() != 0 || item->require_signature != 0;
    if (pux_repo_update(item->url, item->cache_dir, trusted_keys_root(), require_signed,
                        error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: repository '%s' update failed: %s\n", item->name, error);
        return 1;
    }
    printf("updated: %s\n", item->name);
    return 0;
}

static int update_command(int argc, char **argv)
{
    char error[512] = {0};
    if (argc == 4) {
        if (pux_repo_update(argv[2], argv[3], trusted_keys_root(), repository_signature_required(),
                            error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository update failed: %s\n", error);
            return 1;
        }
        printf("updated: %s\n", argv[3]);
        return 0;
    }

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Usage: %s update\n", argv[0]);
        fprintf(stderr, "       %s update <repository-name>\n", argv[0]);
        fprintf(stderr, "       %s update <repository-url> <local-repository-dir>\n", argv[0]);
        return 2;
    }

    struct pux_repo_config_list list = {0};
    if (pux_repo_config_load_all(pux_repo_config_root(), pux_repo_cache_root(), &list, error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: cannot load repository configuration: %s\n", error);
        return 1;
    }
    int result = 0;
    if (argc == 3) {
        size_t found = 0U;
        for (size_t i = 0U; i < list.count; ++i) {
            if (strcmp(list.items[i].name, argv[2]) == 0) {
                found = 1U;
                if (list.items[i].enabled != 0) result = configured_repo_update(&list.items[i]);
                else fprintf(stderr, "pux: repository '%s' is disabled\n", argv[2]);
                break;
            }
        }
        if (found == 0U) {
            fprintf(stderr, "pux: repository '%s' is not configured\n", argv[2]);
            result = 1;
        }
    } else {
        size_t enabled = 0U;
        for (size_t i = 0U; i < list.count; ++i) {
            if (list.items[i].enabled == 0) continue;
            ++enabled;
            if (configured_repo_update(&list.items[i]) != 0) result = 1;
        }
        if (enabled == 0U) {
            fprintf(stderr, "pux: no enabled repositories configured\n");
            result = 1;
        }
    }
    pux_repo_config_list_free(&list);
    return result;
}

static int search_configured_repositories(const char *term)
{
    char error[512] = {0};
    struct pux_repo_config_list list = {0};
    if (pux_repo_config_load_all(pux_repo_config_root(), pux_repo_cache_root(), &list,
                                 error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: cannot load repository configuration: %s\n", error);
        return 1;
    }
    size_t enabled = 0U;
    size_t successful = 0U;
    for (size_t i = 0U; i < list.count; ++i) {
        if (list.items[i].enabled == 0) continue;
        ++enabled;
        char repo_error[512] = {0};
        if (pux_repo_search(list.items[i].cache_dir, term, stdout,
                            repo_error, sizeof(repo_error)) == 0) {
            ++successful;
        } else {
            fprintf(stderr, "pux: repository '%s' search failed: %s\n",
                    list.items[i].name, repo_error);
        }
    }
    pux_repo_config_list_free(&list);
    if (enabled == 0U) {
        fprintf(stderr, "pux: no enabled repositories configured\n");
        return 1;
    }
    return successful == 0U ? 1 : 0;
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
        fprintf(stderr, "Usage: %s package <validate|info|extract|checksum> ...\n", argv[0]);
        return 2;
    }

    const char *operation = argv[2];
    if (strcmp(operation, "checksum") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s package checksum <file>\n", argv[0]);
            return 2;
        }
        char error[512] = {0};
        char digest[PUX_SHA256_HEX_SIZE];
        if (pux_sha256_file(argv[3], digest, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: checksum failed: %s\n", error);
            return 1;
        }
        puts(digest);
        return 0;
    }

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

static const char *trusted_keys_root(void)
{
    const char *value = getenv(PUX_TRUST_ENV_ROOT);
    return (value != NULL && value[0] != '\0') ? value : PUX_TRUST_DEFAULT_ROOT;
}

static int repository_signature_required(void)
{
    const char *value = getenv("PUX_REQUIRE_SIGNED_REPOSITORY");
    return value != NULL && (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0);
}

static int verify_trusted_repository(const char *repository_dir)
{
    char error[512] = {0};
    char keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
    if (pux_trust_verify_repository(trusted_keys_root(), repository_dir, keyid, error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: repository trust verification failed: %s\n", error);
        return 1;
    }
    return 0;
}

static int manifests_match_exact(const struct pux_package_manifest *left,
                                  const struct pux_package_manifest *right)
{
    return strcmp(left->name, right->name) == 0 &&
           strcmp(left->version, right->version) == 0 &&
           left->release == right->release &&
           strcmp(left->arch, right->arch) == 0;
}

static int install_from_repository(const char *package_name, const char *repository_dir);

static int repository_filename(const char *package_path, const char **filename_out)
{
    if (package_path == NULL || filename_out == NULL) return -1;
    const char *filename = strrchr(package_path, '/');
    filename = filename != NULL ? filename + 1 : package_path;
    if (filename[0] == '\0' || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) return -1;
    for (const char *p = filename; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '?' || *p == '#') return -1;
    }
    *filename_out = filename;
    return 0;
}

static int verify_file_against_repository_index(const char *repository_dir,
                                                 const char *filename,
                                                 const char *path,
                                                 char *error,
                                                 size_t error_size)
{
    struct pux_repo_catalog catalog = {0};
    if (pux_repo_load_index(repository_dir, &catalog, error, error_size) != 0) return -1;
    const struct pux_repo_package *found = NULL;
    for (size_t i = 0U; i < catalog.count; ++i) {
        if (strcmp(catalog.packages[i].filename, filename) == 0) {
            found = &catalog.packages[i];
            break;
        }
    }
    if (found == NULL) {
        pux_repo_catalog_free(&catalog);
        (void)snprintf(error, error_size, "package is not present in repository index: %s", filename);
        return -1;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0L ||
        (size_t)st.st_size != found->size) {
        pux_repo_catalog_free(&catalog);
        (void)snprintf(error, error_size, "repository package size mismatch: %s", filename);
        return -1;
    }

    char actual[PUX_SHA256_HEX_SIZE];
    if (pux_sha256_file(path, actual, error, error_size) != 0) {
        pux_repo_catalog_free(&catalog);
        return -1;
    }
    if (memcmp(actual, found->sha256, PUX_SHA256_HEX_SIZE) != 0) {
        pux_repo_catalog_free(&catalog);
        (void)snprintf(error, error_size, "repository package SHA-256 mismatch: %s", filename);
        return -1;
    }
    pux_repo_catalog_free(&catalog);
    return 0;
}

static int download_remote_package(const char *repository_url,
                                  const char *repository_dir,
                                  const char *package_path,
                                  char *error,
                                  size_t error_size)
{
    const char *filename = NULL;
    if (repository_filename(package_path, &filename) != 0 ||
        !has_suffix(filename, ".pux")) {
        (void)snprintf(error, error_size, "repository package filename is invalid");
        return -1;
    }

    char destination[PATH_MAX];
    const int destination_len = snprintf(destination, sizeof(destination), "%s/%s", repository_dir, filename);
    if (destination_len < 0 || (size_t)destination_len >= sizeof(destination)) {
        (void)snprintf(error, error_size, "local package cache path is too long");
        return -1;
    }

    if (access(destination, F_OK) == 0 &&
        verify_file_against_repository_index(repository_dir, filename, destination, error, error_size) == 0) {
        return 0;
    }

    char temp_path[PATH_MAX];
    const int temp_len = snprintf(temp_path, sizeof(temp_path), "%s/.pux-download-XXXXXX", repository_dir);
    if (temp_len < 0 || (size_t)temp_len >= sizeof(temp_path)) {
        (void)snprintf(error, error_size, "package download staging path is too long");
        return -1;
    }
    const int fd = mkstemp(temp_path);
    if (fd < 0) {
        (void)snprintf(error, error_size, "cannot create package download staging file: %s", strerror(errno));
        return -1;
    }
    if (fchmod(fd, 0600U) != 0) {
        const int saved_errno = errno;
        close(fd);
        unlink(temp_path);
        (void)snprintf(error, error_size, "cannot protect package download staging file: %s", strerror(saved_errno));
        return -1;
    }
    close(fd);

    const size_t base_len = strlen(repository_url);
    const size_t name_len = strlen(filename);
    const int separator = base_len > 0U && repository_url[base_len - 1U] != '/';
    if (base_len > SIZE_MAX - name_len - (separator ? 1U : 0U) - 1U) {
        unlink(temp_path);
        (void)snprintf(error, error_size, "repository package URL is too long");
        return -1;
    }
    char url[PATH_MAX * 2U];
    const size_t total = base_len + name_len + (separator ? 1U : 0U);
    if (total + 1U > sizeof(url)) {
        unlink(temp_path);
        (void)snprintf(error, error_size, "repository package URL is too long");
        return -1;
    }
    memcpy(url, repository_url, base_len);
    size_t offset = base_len;
    if (separator != 0) url[offset++] = '/';
    memcpy(url + offset, filename, name_len + 1U);

    long status = 0L;
    if (pux_transport_download(url, temp_path, 64U * 1024U * 1024U,
                               &status, error, error_size) != 0) {
        unlink(temp_path);
        return -1;
    }
    if (status != 200L) {
        unlink(temp_path);
        (void)snprintf(error, error_size, "repository package download returned HTTP %ld", status);
        return -1;
    }

    if (verify_file_against_repository_index(repository_dir, filename, temp_path,
                                              error, error_size) != 0) {
        unlink(temp_path);
        return -1;
    }

    struct pux_package_manifest manifest;
    if (pux_package_archive_validate(temp_path, &manifest, error, error_size) != 0) {
        unlink(temp_path);
        return -1;
    }
    pux_package_manifest_free(&manifest);

    if (rename(temp_path, destination) != 0) {
        const int saved_errno = errno;
        unlink(temp_path);
        (void)snprintf(error, error_size, "cannot install downloaded package: %s", strerror(saved_errno));
        return -1;
    }
    return 0;
}

static int install_from_remote_repository_policy(const char *package_name,
                                                 const char *repository_url,
                                                 const char *repository_dir,
                                                 int refresh_metadata,
                                                 int require_signed)
{
    char error[512] = {0};
    if (refresh_metadata != 0) {
        if (pux_repo_update(repository_url, repository_dir, trusted_keys_root(),
                            require_signed, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository update failed: %s\n", error);
            return 1;
        }
    } else {
        char index_path[4096];
        const int n = snprintf(index_path, sizeof(index_path), "%s/%s",
                               repository_dir, PUX_REPO_INDEX_NAME);
        if (n < 0 || (size_t)n >= sizeof(index_path)) {
            fprintf(stderr, "pux: repository index path is too long\n");
            return 1;
        }
        if (access(index_path, R_OK) != 0) {
            fprintf(stderr, "pux: repository metadata is missing; run 'pux update' first\n");
            return 1;
        }
    }
    if (require_signed != 0 && verify_trusted_repository(repository_dir) != 0) return 1;

    struct pux_resolve_plan plan = {0};
    if (pux_resolve_package_plan(package_name, repository_dir, &plan,
                                 error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: dependency resolution failed: %s\n", error);
        return 1;
    }

    /* Download every package in the plan before changing the installation root. */
    for (size_t i = 0U; i < plan.count; ++i) {
        if (download_remote_package(repository_url, repository_dir, plan.package_paths[i],
                                    error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: package download failed: %s\n", error);
            pux_resolve_plan_free(&plan);
            return 1;
        }
    }

    const int result = install_from_repository(package_name, repository_dir);
    pux_resolve_plan_free(&plan);
    return result;
}

static int install_from_remote_repository(const char *package_name,
                                          const char *repository_url,
                                          const char *repository_dir)
{
    return install_from_remote_repository_policy(package_name, repository_url, repository_dir,
                                                 1, repository_signature_required());
}

static int install_from_repository_policy(const char *package_name,
                                           const char *repository_dir,
                                           int require_signed)
{
    char error[512] = {0};
    struct pux_resolve_plan plan = {0};

    if (require_signed != 0 && verify_trusted_repository(repository_dir) != 0) return 1;

    if (pux_resolve_package_plan(package_name, repository_dir, &plan,
                                 error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: dependency resolution failed: %s\n", error);
        return 1;
    }

    /* Validate every plan item and preflight already-installed packages before
     * changing the filesystem. The actual transaction remains per-package
     * atomic; cross-package rollback is a later milestone. */
    for (size_t i = 0U; i < plan.count; ++i) {
        if (pux_repo_verify_package(repository_dir, plan.package_paths[i], error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository package verification failed: %s\n", error);
            pux_resolve_plan_free(&plan);
            return 1;
        }
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

static int install_from_repository(const char *package_name, const char *repository_dir)
{
    return install_from_repository_policy(package_name, repository_dir,
                                          repository_signature_required());
}

static int is_local_package_argument(const char *value)
{
    if (value == NULL || value[0] == '\0') return 0;
    return strchr(value, '/') != NULL || has_suffix(value, ".pux");
}

static int install_from_configured_repositories(const char *package_name)
{
    char error[512] = {0};
    struct pux_repo_config_list list = {0};
    if (pux_repo_config_load_all(pux_repo_config_root(), pux_repo_cache_root(),
                                 &list, error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: cannot load repository configuration: %s\n", error);
        return 1;
    }

    int had_index = 0;
    int result = 1;
    for (size_t i = 0U; i < list.count; ++i) {
        const struct pux_repo_config_entry *item = &list.items[i];
        if (item->enabled == 0) continue;

        char index_path[PATH_MAX];
        const int n = snprintf(index_path, sizeof(index_path), "%s/%s",
                               item->cache_dir, PUX_REPO_INDEX_NAME);
        if (n < 0 || (size_t)n >= sizeof(index_path)) continue;
        if (access(index_path, R_OK) != 0) continue;
        had_index = 1;

        struct pux_resolve_plan probe = {0};
        char probe_error[512] = {0};
        if (pux_resolve_package_plan(package_name, item->cache_dir, &probe,
                                     probe_error, sizeof(probe_error)) != 0) {
            pux_resolve_plan_free(&probe);
            continue;
        }
        pux_resolve_plan_free(&probe);

        const int require_signed = repository_signature_required() != 0 || item->require_signature != 0;
        result = install_from_remote_repository_policy(package_name, item->url, item->cache_dir,
                                                       0, require_signed);
        if (result == 0) break;
    }

    if (result != 0) {
        if (had_index == 0) {
            fprintf(stderr, "pux: no repository metadata available; run 'pux update' first\n");
        } else {
            fprintf(stderr, "pux: package '%s' is not available in configured repositories\n", package_name);
        }
    }
    pux_repo_config_list_free(&list);
    return result;
}

static int install_command(int argc, char **argv)
{
    if (argc == 3) {
        if (!is_local_package_argument(argv[2])) {
            return install_from_configured_repositories(argv[2]);
        }
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

    if (argc == 5) {
        if (strncmp(argv[3], "http://", 7U) != 0 && strncmp(argv[3], "https://", 8U) != 0) {
            fprintf(stderr, "Usage: %s install <package-name> <repository-url> <local-repository-dir>\n", argv[0]);
            return 2;
        }
        return install_from_remote_repository(argv[2], argv[3], argv[4]);
    }

    fprintf(stderr, "Usage: %s install <package.pux>\n", argv[0]);
    fprintf(stderr, "       %s install <package-name>\n", argv[0]);
    fprintf(stderr, "       %s install <package-name> <repository-dir>\n", argv[0]);
    fprintf(stderr, "       %s install <package-name> <repository-url> <local-repository-dir>\n", argv[0]);
    return 2;
}

static int upgrade_from_remote_repository_policy(const char *package_name,
                                                 const char *repository_url,
                                                 const char *repository_dir,
                                                 int refresh_metadata,
                                                 int require_signed)
{
    char error[512] = {0};
    if (refresh_metadata != 0) {
        if (pux_repo_update(repository_url, repository_dir, trusted_keys_root(),
                            require_signed, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: repository update failed: %s\n", error);
            return 1;
        }
    }
    char index_path[PATH_MAX];
    const int n = snprintf(index_path, sizeof(index_path), "%s/%s",
                           repository_dir, PUX_REPO_INDEX_NAME);
    if (n < 0 || (size_t)n >= sizeof(index_path) || access(index_path, R_OK) != 0) {
        fprintf(stderr, "pux: repository metadata is missing; run 'pux update' first\n");
        return 1;
    }
    if (require_signed != 0 && verify_trusted_repository(repository_dir) != 0) return 1;

    struct pux_resolve_plan plan = {0};
    if (pux_resolve_package_plan(package_name, repository_dir, &plan, error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: dependency resolution failed: %s\n", error);
        return 1;
    }
    for (size_t i = 0U; i < plan.count; ++i) {
        if (download_remote_package(repository_url, repository_dir, plan.package_paths[i],
                                    error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: package download failed: %s\n", error);
            pux_resolve_plan_free(&plan);
            return 1;
        }
    }
    pux_resolve_plan_free(&plan);
    return upgrade_from_repository_policy(package_name, repository_dir, require_signed);
}

static int upgrade_from_configured_repositories(const char *package_name)
{
    char error[512] = {0};
    struct pux_repo_config_list list = {0};
    if (pux_repo_config_load_all(pux_repo_config_root(), pux_repo_cache_root(),
                                 &list, error, sizeof(error)) != 0) {
        fprintf(stderr, "pux: cannot load repository configuration: %s\n", error);
        return 1;
    }
    int had_index = 0;
    int result = 1;
    for (size_t i = 0U; i < list.count; ++i) {
        const struct pux_repo_config_entry *item = &list.items[i];
        if (item->enabled == 0) continue;
        char index_path[PATH_MAX];
        const int n = snprintf(index_path, sizeof(index_path), "%s/%s",
                               item->cache_dir, PUX_REPO_INDEX_NAME);
        if (n < 0 || (size_t)n >= sizeof(index_path) || access(index_path, R_OK) != 0) continue;
        had_index = 1;
        struct pux_resolve_plan probe = {0};
        char probe_error[512] = {0};
        if (pux_resolve_package_plan(package_name, item->cache_dir, &probe,
                                     probe_error, sizeof(probe_error)) != 0) {
            pux_resolve_plan_free(&probe);
            continue;
        }
        pux_resolve_plan_free(&probe);
        const int require_signed = repository_signature_required() != 0 || item->require_signature != 0;
        result = upgrade_from_remote_repository_policy(package_name, item->url, item->cache_dir,
                                                       0, require_signed);
        if (result == 0) break;
    }
    if (result != 0) {
        if (had_index == 0) fprintf(stderr, "pux: no repository metadata available; run 'pux update' first\n");
        else fprintf(stderr, "pux: package '%s' is not available for upgrade in configured repositories\n", package_name);
    }
    pux_repo_config_list_free(&list);
    return result;
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

    if (strcmp(command, "keygen") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: %s keygen <private-key> <public-key>\n", argv[0]);
            return 2;
        }
        char error[512] = {0};
        if (pux_signature_keygen(argv[2], argv[3], error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: key generation failed: %s\n", error);
            return 1;
        }
        char keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
        if (pux_signature_keyid(argv[3], keyid, error, sizeof(error)) != 0) {
            fprintf(stderr, "pux: cannot read generated public key: %s\n", error);
            return 1;
        }
        printf("keyid: %s\n", keyid);
        return 0;
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

    if (strcmp(command, "upgrade") == 0) {
        if (argc == 3) return upgrade_from_configured_repositories(argv[2]);
        if (argc == 4) return upgrade_from_repository(argv[2], argv[3]);
        fprintf(stderr, "Usage: %s upgrade <package-name>\n", argv[0]);
        fprintf(stderr, "       %s upgrade <package-name> <repository-dir>\n", argv[0]);
        return 2;
    }

    if (strcmp(command, "search") == 0) {
        if (argc == 3) return search_configured_repositories(argv[2]);
        if (argc == 4) {
            char error[512] = {0};
            if (pux_repo_search(argv[3], argv[2], stdout, error, sizeof(error)) != 0) {
                fprintf(stderr, "pux: repository search failed: %s\n", error);
                return 1;
            }
            return 0;
        }
        fprintf(stderr, "Usage: %s search <term> [repository-dir]\n", argv[0]);
        return 2;
    }
    if (strcmp(command, "repo") == 0) {
        return repo_command(argc, argv);
    }
    if (strcmp(command, "trust") == 0) {
        return trust_command(argc, argv);
    }
    if (strcmp(command, "update") == 0) {
        return update_command(argc, argv);
    }
    if (strcmp(command, "verify") == 0) {
        return command_not_implemented(command);
    }

    fprintf(stderr, "pux: unknown command '%s'\n", command);
    fprintf(stderr, "pux: run '%s help' for usage\n", argv[0]);
    return 2;
}
