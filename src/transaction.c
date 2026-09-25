#define _GNU_SOURCE 1

#include "pux/transaction.h"

#include "pux/container.h"
#include "pux/db.h"
#include "pux/extract.h"
#include "pux/package.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#define PUX_TXN_MAX_PATH 4096U
#define PUX_TXN_MAX_FILES 4096U
#define PUX_TXN_MAX_STAGING_TEMPLATE 4096U

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0U) (void)snprintf(error, error_size, "%s", message);
}

static void set_errorf(char *error, size_t error_size, const char *format, const char *value)
{
    if (error != NULL && error_size > 0U) (void)snprintf(error, error_size, format, value);
}

static char *duplicate_string(const char *value)
{
    const size_t length = strlen(value);
    char *copy = malloc(length + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, value, length + 1U);
    return copy;
}

struct moved_list {
    char **paths;
    size_t count;
    size_t capacity;
};

static int target_arch(char *buffer, size_t buffer_size, char *error, size_t error_size)
{
    const char *override = getenv("PUX_ARCH");
    if (override != NULL && override[0] != '\0') {
        if (strlen(override) + 1U > buffer_size) {
            set_error(error, error_size, "PUX_ARCH is too long");
            return -1;
        }
        memcpy(buffer, override, strlen(override) + 1U);
        return 0;
    }

    struct utsname info;
    if (uname(&info) != 0) {
        set_errorf(error, error_size, "cannot determine system architecture: %s", strerror(errno));
        return -1;
    }
    if (strlen(info.machine) + 1U > buffer_size) {
        set_error(error, error_size, "system architecture name is too long");
        return -1;
    }
    memcpy(buffer, info.machine, strlen(info.machine) + 1U);
    return 0;
}

static int path_join(const char *base, const char *relative,
                     char *output, size_t output_size)
{
    const size_t base_len = strlen(base);
    const size_t rel_len = strlen(relative);
    const int separator = (base_len != 0U && base[base_len - 1U] != '/');
    const size_t total = base_len + (size_t)separator + rel_len + 1U;
    if (total > output_size) return -1;
    memcpy(output, base, base_len);
    size_t offset = base_len;
    if (separator != 0) output[offset++] = '/';
    memcpy(output + offset, relative, rel_len + 1U);
    return 0;
}

static int moved_reserve(struct moved_list *list, size_t capacity)
{
    if (capacity <= list->capacity) return 0;
    char **items = realloc(list->paths, capacity * sizeof(*items));
    if (items == NULL) return -1;
    list->paths = items;
    list->capacity = capacity;
    return 0;
}

static int moved_append(struct moved_list *list, const char *path)
{
    if (list->count >= list->capacity) return -1;
    char *copy = duplicate_string(path);
    if (copy == NULL) return -1;
    list->paths[list->count++] = copy;
    return 0;
}

static void moved_free(struct moved_list *list)
{
    for (size_t i = 0U; i < list->count; ++i) free(list->paths[i]);
    free(list->paths);
    list->paths = NULL;
    list->count = 0U;
    list->capacity = 0U;
}

static int file_list_append(struct pux_db_file_list *files, char type, const char *path)
{
    if (files->count >= PUX_TXN_MAX_FILES) return -1;
    struct pux_db_file_entry *items = realloc(files->items,
                                               (files->count + 1U) * sizeof(*items));
    if (items == NULL) return -1;
    files->items = items;
    files->items[files->count].type = type;
    files->items[files->count].path = duplicate_string(path);
    if (files->items[files->count].path == NULL) return -1;
    files->count++;
    return 0;
}

static int collect_tree(const char *directory, const char *relative,
                        struct pux_db_file_list *files,
                        char *error, size_t error_size)
{
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        set_errorf(error, error_size, "cannot read staging directory: %s", strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char child_relative[PUX_TXN_MAX_PATH];
        if (path_join(relative, entry->d_name, child_relative, sizeof(child_relative)) != 0) {
            closedir(dir);
            set_error(error, error_size, "staged package path is too long");
            return -1;
        }
        char child_path[PUX_TXN_MAX_PATH];
        if (path_join(directory, entry->d_name, child_path, sizeof(child_path)) != 0) {
            closedir(dir);
            set_error(error, error_size, "staging path is too long");
            return -1;
        }

        struct stat st;
        if (lstat(child_path, &st) != 0) {
            closedir(dir);
            set_errorf(error, error_size, "cannot inspect staged path: %s", strerror(errno));
            return -1;
        }
        if (S_ISDIR(st.st_mode)) {
            if (file_list_append(files, 'd', child_relative) != 0) {
                closedir(dir);
                set_error(error, error_size, "staged package contains too many files");
                return -1;
            }
            if (collect_tree(child_path, child_relative, files, error, error_size) != 0) {
                closedir(dir);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (file_list_append(files, 'f', child_relative) != 0) {
                closedir(dir);
                set_error(error, error_size, "staged package contains too many files");
                return -1;
            }
        } else {
            closedir(dir);
            set_errorf(error, error_size, "unexpected file type in staging area: %s", child_relative);
            return -1;
        }
    }
    if (closedir(dir) != 0) {
        set_error(error, error_size, "cannot close staging directory");
        return -1;
    }
    return 0;
}

static int dependency_check(const char *db_root,
                            const struct pux_package_manifest *manifest,
                            char *error, size_t error_size)
{
    for (size_t i = 0U; i < manifest->depends.count; ++i) {
        int satisfied = 0;
        char dependency_error[512] = {0};
        if (pux_db_dependency_satisfied(db_root, manifest->depends.items[i],
                                        &satisfied, dependency_error, sizeof(dependency_error)) != 0) {
            set_errorf(error, error_size, "cannot check dependency: %s", dependency_error);
            return -1;
        }
        if (satisfied == 0) {
            set_errorf(error, error_size, "unsatisfied dependency: %s", manifest->depends.items[i]);
            return -1;
        }
    }
    return 0;
}

static int ensure_destination_parent(const char *root, const char *relative,
                                     char *error, size_t error_size)
{
    char path[PUX_TXN_MAX_PATH];
    if (path_join(root, relative, path, sizeof(path)) != 0) {
        set_error(error, error_size, "destination path is too long");
        return -1;
    }
    char *last = strrchr(path, '/');
    if (last == NULL || last == path) return 0;
    *last = '\0';

    char current[PUX_TXN_MAX_PATH];
    const size_t root_len = strlen(root);
    if (root_len + 1U > sizeof(current)) {
        set_error(error, error_size, "destination root is too long");
        return -1;
    }
    memcpy(current, root, root_len + 1U);

    const char *cursor = current;
    if (root[0] == '/' && strcmp(current, "/") == 0) cursor = current + 1;
    (void)cursor;

    /* Build one component at a time using mkdir/stat to avoid following a
     * conflicting regular file or symlink in the parent chain. */
    size_t prefix = root_len;
    while (path[prefix] == '/') ++prefix;
    while (path[prefix] != '\0') {
        const char *slash = strchr(path + prefix, '/');
        const size_t end = slash == NULL ? strlen(path) : (size_t)(slash - path);
        if (end >= sizeof(current)) {
            set_error(error, error_size, "destination path is too long");
            return -1;
        }
        memcpy(current, path, end);
        current[end] = '\0';
        struct stat st;
        if (lstat(current, &st) != 0) {
            if (errno != ENOENT) {
                set_errorf(error, error_size, "cannot inspect destination parent: %s", strerror(errno));
                return -1;
            }
            if (mkdir(current, 0755) != 0 && errno != EEXIST) {
                set_errorf(error, error_size, "cannot create destination parent: %s", strerror(errno));
                return -1;
            }
            if (lstat(current, &st) != 0) {
                set_errorf(error, error_size, "cannot inspect created destination parent: %s", strerror(errno));
                return -1;
            }
        }
        if (!S_ISDIR(st.st_mode)) {
            set_errorf(error, error_size, "destination parent is not a directory: %s", current);
            return -1;
        }
        if (slash == NULL) break;
        prefix = end + 1U;
        while (path[prefix] == '/') ++prefix;
    }
    return 0;
}

static int preflight_paths(const char *root, const char *db_root,
                           const struct pux_db_file_list *files,
                           char *error, size_t error_size)
{
    for (size_t i = 0U; i < files->count; ++i) {
        const char *relative = files->items[i].path;
        char owner[256] = {0};
        int owned = 0;
        if (pux_db_find_owner(db_root, relative, owner, sizeof(owner), &owned,
                              error, error_size) != 0) {
            return -1;
        }
        if (owned != 0 && files->items[i].type == 'f') {
            set_errorf(error, error_size, "file is already owned by installed package: %s", owner);
            return -1;
        }

        char destination[PUX_TXN_MAX_PATH];
        if (path_join(root, relative, destination, sizeof(destination)) != 0) {
            set_error(error, error_size, "destination path is too long");
            return -1;
        }
        struct stat st;
        if (lstat(destination, &st) != 0) {
            if (errno != ENOENT) {
                set_errorf(error, error_size, "cannot inspect destination path: %s", strerror(errno));
                return -1;
            }
            continue;
        }
        if (files->items[i].type == 'd') {
            if (!S_ISDIR(st.st_mode)) {
                set_errorf(error, error_size, "destination path conflicts with directory: %s", relative);
                return -1;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            set_errorf(error, error_size, "destination file already exists: %s", relative);
            return -1;
        } else {
            set_errorf(error, error_size, "destination path conflicts with file: %s", relative);
            return -1;
        }
    }
    return 0;
}

static void rollback_transaction(const struct moved_list *moved,
                                 const struct moved_list *created_dirs)
{
    for (size_t i = moved->count; i > 0U; --i) {
        (void)unlink(moved->paths[i - 1U]);
    }
    for (size_t i = created_dirs->count; i > 0U; --i) {
        (void)rmdir(created_dirs->paths[i - 1U]);
    }
}

static void remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        (void)unlink(path);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[PUX_TXN_MAX_PATH];
        if (path_join(path, entry->d_name, child, sizeof(child)) != 0) continue;
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) remove_tree(child);
        else (void)unlink(child);
    }
    closedir(dir);
    (void)rmdir(path);
}

int pux_install_package(const char *package_path,
                        const char *root,
                        const char *db_root,
                        char *error,
                        size_t error_size)
{
    if (package_path == NULL || root == NULL || db_root == NULL || root[0] == '\0' || db_root[0] == '\0') {
        set_error(error, error_size, "invalid install argument");
        return -1;
    }

    struct pux_package_manifest manifest;
    if (pux_package_archive_validate(package_path, &manifest, error, error_size) != 0) {
        return -1;
    }

    char arch[128];
    if (target_arch(arch, sizeof(arch), error, error_size) != 0) {
        pux_package_manifest_free(&manifest);
        return -1;
    }
    if (strcmp(manifest.arch, "noarch") != 0 && strcmp(manifest.arch, arch) != 0) {
        set_errorf(error, error_size, "package architecture does not match host: %s", manifest.arch);
        pux_package_manifest_free(&manifest);
        return -1;
    }

    struct pux_package_manifest already;
    struct pux_db_file_list old_files = {0};
    char db_error[512] = {0};
    if (pux_db_read_package(db_root, manifest.name, &already, &old_files,
                            db_error, sizeof(db_error)) == 0) {
        pux_package_manifest_free(&already);
        pux_db_file_list_free(&old_files);
        set_errorf(error, error_size, "package is already installed: %s", manifest.name);
        pux_package_manifest_free(&manifest);
        return -1;
    }
    pux_package_manifest_free(&already);
    pux_db_file_list_free(&old_files);

    if (dependency_check(db_root, &manifest, error, error_size) != 0) {
        pux_package_manifest_free(&manifest);
        return -1;
    }

    char root_stat_path[PUX_TXN_MAX_PATH];
    if (strlen(root) + 1U > sizeof(root_stat_path)) {
        set_error(error, error_size, "installation root is too long");
        pux_package_manifest_free(&manifest);
        return -1;
    }
    memcpy(root_stat_path, root, strlen(root) + 1U);
    struct stat root_st;
    if (lstat(root_stat_path, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        set_error(error, error_size, "installation root is not a directory");
        pux_package_manifest_free(&manifest);
        return -1;
    }

    char template[PUX_TXN_MAX_STAGING_TEMPLATE];
    const size_t root_len = strlen(root);
    const int needs_separator = root_len != 0U && root[root_len - 1U] != '/';
    const char *suffix = ".pux-txn-XXXXXX";
    const size_t total = root_len + (size_t)needs_separator + strlen(suffix) + 1U;
    if (total > sizeof(template)) {
        set_error(error, error_size, "transaction staging path is too long");
        pux_package_manifest_free(&manifest);
        return -1;
    }
    size_t offset = root_len;
    memcpy(template, root, root_len);
    if (needs_separator != 0) template[offset++] = '/';
    memcpy(template + offset, suffix, strlen(suffix) + 1U);
    char *stage = mkdtemp(template);
    if (stage == NULL) {
        set_errorf(error, error_size, "cannot create transaction staging directory: %s", strerror(errno));
        pux_package_manifest_free(&manifest);
        return -1;
    }

    char payload_stage[PUX_TXN_MAX_PATH];
    if (path_join(stage, "payload-root", payload_stage, sizeof(payload_stage)) != 0 ||
        mkdir(payload_stage, 0755) != 0) {
        set_error(error, error_size, "cannot create transaction staging payload directory");
        remove_tree(stage);
        pux_package_manifest_free(&manifest);
        return -1;
    }

    char extract_error[512] = {0};
    if (pux_package_archive_extract(package_path, payload_stage,
                                     extract_error, sizeof(extract_error)) != 0) {
        set_errorf(error, error_size, "cannot stage package: %s", extract_error);
        remove_tree(stage);
        pux_package_manifest_free(&manifest);
        return -1;
    }

    struct pux_db_file_list files = {0};
    if (collect_tree(payload_stage, "", &files, error, error_size) != 0 || files.count == 0U) {
        if (files.count == 0U) set_error(error, error_size, "package contains no payload files");
        pux_db_file_list_free(&files);
        remove_tree(stage);
        pux_package_manifest_free(&manifest);
        return -1;
    }

    if (preflight_paths(root, db_root, &files, error, error_size) != 0) {
        pux_db_file_list_free(&files);
        remove_tree(stage);
        pux_package_manifest_free(&manifest);
        return -1;
    }

    /* The extraction helper puts usr/bin/... under payload_stage. Move the
     * actual payload paths into the installation root, not the staging wrapper. */
    struct moved_list moved = {0};
    struct moved_list created_dirs = {0};
    if (moved_reserve(&moved, files.count) != 0 ||
        moved_reserve(&created_dirs, files.count) != 0) {
        set_error(error, error_size, "out of memory tracking transaction rollback");
        moved_free(&moved);
        moved_free(&created_dirs);
        pux_db_file_list_free(&files);
        remove_tree(stage);
        pux_package_manifest_free(&manifest);
        return -1;
    }

    for (size_t i = 0U; i < files.count; ++i) {
        const char *relative = files.items[i].path;
        char stage_relative[PUX_TXN_MAX_PATH];
        if (path_join("payload-root", relative, stage_relative, sizeof(stage_relative)) != 0) {
            set_error(error, error_size, "transaction staging path is too long");
            rollback_transaction(&moved, &created_dirs);
            moved_free(&moved);
            moved_free(&created_dirs);
            pux_db_file_list_free(&files);
            remove_tree(stage);
            pux_package_manifest_free(&manifest);
            return -1;
        }

        char stage_path[PUX_TXN_MAX_PATH];
        char destination[PUX_TXN_MAX_PATH];
        if (path_join(stage, stage_relative, stage_path, sizeof(stage_path)) != 0 ||
            path_join(root, relative, destination, sizeof(destination)) != 0) {
            set_error(error, error_size, "transaction path is too long");
            rollback_transaction(&moved, &created_dirs);
            moved_free(&moved);
            moved_free(&created_dirs);
            pux_db_file_list_free(&files);
            remove_tree(stage);
            pux_package_manifest_free(&manifest);
            return -1;
        }
        (void)stage_path;
        (void)destination;
    }

    /* Process directories first, then regular files. This is deterministic and
     * avoids trying to move a file before its parent directory exists. */
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0U; i < files.count; ++i) {
            const char wanted_type = pass == 0 ? 'd' : 'f';
            if (files.items[i].type != wanted_type) continue;
            const char *relative = files.items[i].path;
            char stage_relative[PUX_TXN_MAX_PATH];
            char stage_path[PUX_TXN_MAX_PATH];
            char destination[PUX_TXN_MAX_PATH];
            if (path_join("payload-root", relative, stage_relative, sizeof(stage_relative)) != 0 ||
                path_join(stage, stage_relative, stage_path, sizeof(stage_path)) != 0 ||
                path_join(root, relative, destination, sizeof(destination)) != 0) {
                set_error(error, error_size, "transaction path is too long");
                rollback_transaction(&moved, &created_dirs);
                moved_free(&moved);
                moved_free(&created_dirs);
                pux_db_file_list_free(&files);
                remove_tree(stage);
                pux_package_manifest_free(&manifest);
                return -1;
            }

            if (wanted_type == 'd') {
                struct stat st;
                if (lstat(destination, &st) != 0) {
                    if (errno != ENOENT) {
                        set_errorf(error, error_size, "cannot inspect destination directory: %s", strerror(errno));
                        rollback_transaction(&moved, &created_dirs);
                        moved_free(&moved); moved_free(&created_dirs); pux_db_file_list_free(&files); remove_tree(stage); pux_package_manifest_free(&manifest); return -1;
                    }
                    struct stat staged_st;
                    if (lstat(stage_path, &staged_st) != 0 || mkdir(destination, (mode_t)(staged_st.st_mode & 07777U)) != 0) {
                        set_errorf(error, error_size, "cannot create destination directory: %s", strerror(errno));
                        rollback_transaction(&moved, &created_dirs);
                        moved_free(&moved); moved_free(&created_dirs); pux_db_file_list_free(&files); remove_tree(stage); pux_package_manifest_free(&manifest); return -1;
                    }
                    if (moved_append(&created_dirs, destination) != 0) {
                        set_error(error, error_size, "out of memory tracking transaction rollback");
                        rollback_transaction(&moved, &created_dirs);
                        moved_free(&moved); moved_free(&created_dirs); pux_db_file_list_free(&files); remove_tree(stage); pux_package_manifest_free(&manifest); return -1;
                    }
                } else if (!S_ISDIR(st.st_mode)) {
                    set_errorf(error, error_size, "destination directory conflicts with existing path: %s", relative);
                    rollback_transaction(&moved, &created_dirs);
                    moved_free(&moved); moved_free(&created_dirs); pux_db_file_list_free(&files); remove_tree(stage); pux_package_manifest_free(&manifest); return -1;
                }
            } else {
                if (ensure_destination_parent(root, relative, error, error_size) != 0 ||
                    rename(stage_path, destination) != 0) {
                    if (error[0] == '\0') set_errorf(error, error_size, "cannot commit package file: %s", strerror(errno));
                    rollback_transaction(&moved, &created_dirs);
                    moved_free(&moved); moved_free(&created_dirs); pux_db_file_list_free(&files); remove_tree(stage); pux_package_manifest_free(&manifest); return -1;
                }
                if (moved_append(&moved, destination) != 0) {
                    set_error(error, error_size, "out of memory tracking transaction rollback");
                    rollback_transaction(&moved, &created_dirs);
                    moved_free(&moved); moved_free(&created_dirs); pux_db_file_list_free(&files); remove_tree(stage); pux_package_manifest_free(&manifest); return -1;
                }
            }
        }
    }

    if (pux_db_register_package(db_root, &manifest, &files, error, error_size) != 0) {
        rollback_transaction(&moved, &created_dirs);
        moved_free(&moved);
        moved_free(&created_dirs);
        pux_db_file_list_free(&files);
        remove_tree(stage);
        pux_package_manifest_free(&manifest);
        return -1;
    }

    moved_free(&moved);
    moved_free(&created_dirs);
    pux_db_file_list_free(&files);
    remove_tree(stage);
    pux_package_manifest_free(&manifest);
    return 0;
}
