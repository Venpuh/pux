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

static int file_list_append(struct pux_db_file_list *files, char type, const char *path, const char *target)
{
    if (files->count >= PUX_TXN_MAX_FILES) return -1;
    struct pux_db_file_entry *items = realloc(files->items,
                                               (files->count + 1U) * sizeof(*items));
    if (items == NULL) return -1;
    files->items = items;
    files->items[files->count].type = type;
    files->items[files->count].path = duplicate_string(path);
    files->items[files->count].target = target == NULL ? NULL : duplicate_string(target);
    if (files->items[files->count].path == NULL ||
        (target != NULL && files->items[files->count].target == NULL)) {
        free(files->items[files->count].path);
        free(files->items[files->count].target);
        files->items[files->count].path = NULL;
        files->items[files->count].target = NULL;
        return -1;
    }
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
            if (file_list_append(files, 'd', child_relative, NULL) != 0) {
                closedir(dir);
                set_error(error, error_size, "staged package contains too many files");
                return -1;
            }
            if (collect_tree(child_path, child_relative, files, error, error_size) != 0) {
                closedir(dir);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (file_list_append(files, 'f', child_relative, NULL) != 0) {
                closedir(dir);
                set_error(error, error_size, "staged package contains too many files");
                return -1;
            }
        } else if (S_ISLNK(st.st_mode)) {
            char target[4096];
            const ssize_t length = readlink(child_path, target, sizeof(target) - 1U);
            if (length <= 0 || (size_t)length >= sizeof(target)) {
                closedir(dir);
                set_error(error, error_size, "invalid staged symbolic link");
                return -1;
            }
            target[length] = '\0';
            for (ssize_t i = 0; i < length; ++i) {
                const unsigned char c = (unsigned char)target[i];
                if (c < 0x20U || c == 0x7fU) {
                    closedir(dir);
                    set_error(error, error_size, "invalid staged symbolic link target");
                    return -1;
                }
            }
            if (file_list_append(files, 'l', child_relative, target) != 0) {
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
        if (owned != 0 && (files->items[i].type == 'f' || files->items[i].type == 'l')) {
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
            if (pass == 0) {
                if (files.items[i].type != 'd') continue;
            } else if (files.items[i].type != 'f' && files.items[i].type != 'l') {
                continue;
            }
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

            if (files.items[i].type == 'd') {
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

static int ensure_directory_for_backup(const char *stage_removed, const char *relative,
                                       char *error, size_t error_size)
{
    return ensure_destination_parent(stage_removed, relative, error, error_size);
}

static int validate_removal_paths(const char *root, const char *db_root, const char *package_name,
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
        if (owned == 0) {
            set_errorf(error, error_size, "database ownership is missing for: %s", relative);
            return -1;
        }
        if (strcmp(owner, package_name) != 0 && (files->items[i].type == 'f' || files->items[i].type == 'l')) {
            set_errorf(error, error_size, "file is owned by another package: %s", relative);
            return -1;
        }

        char destination[PUX_TXN_MAX_PATH];
        if (path_join(root, relative, destination, sizeof(destination)) != 0) {
            set_error(error, error_size, "removal path is too long");
            return -1;
        }

        struct stat st;
        if (lstat(destination, &st) != 0) {
            if (errno == ENOENT) continue;
            set_errorf(error, error_size, "cannot inspect installed path: %s", strerror(errno));
            return -1;
        }
        if (files->items[i].type == 'f') {
            if (!S_ISREG(st.st_mode)) {
                set_errorf(error, error_size, "installed file changed type: %s", relative);
                return -1;
            }
        } else if (files->items[i].type == 'l') {
            if (!S_ISLNK(st.st_mode)) {
                set_errorf(error, error_size, "installed symbolic link changed type: %s", relative);
                return -1;
            }
            char target[4096];
            const ssize_t length = readlink(destination, target, sizeof(target) - 1U);
            if (length <= 0 || (size_t)length >= sizeof(target)) {
                set_errorf(error, error_size, "cannot read installed symbolic link: %s", relative);
                return -1;
            }
            target[length] = '\0';
            if (files->items[i].target == NULL || strcmp(target, files->items[i].target) != 0) {
                set_errorf(error, error_size, "installed symbolic link target changed: %s", relative);
                return -1;
            }
        } else if (files->items[i].type == 'd') {
            if (!S_ISDIR(st.st_mode)) {
                set_errorf(error, error_size, "installed directory changed type: %s", relative);
                return -1;
            }
        } else {
            set_error(error, error_size, "database contains unsupported file type");
            return -1;
        }
    }
    return 0;
}

static int compare_paths_desc(const void *left, const void *right)
{
    const struct pux_db_file_entry *const a = left;
    const struct pux_db_file_entry *const b = right;
    const int cmp = strcmp(a->path, b->path);
    return cmp == 0 ? 0 : -cmp;
}

int pux_remove_package(const char *name, const char *root, const char *db_root,
                       char *error, size_t error_size)
{
    if (name == NULL || root == NULL || db_root == NULL || root[0] == '\0' || db_root[0] == '\0') {
        set_error(error, error_size, "invalid remove argument");
        return -1;
    }

    struct pux_package_manifest manifest;
    struct pux_db_file_list files = {0};
    if (pux_db_read_package(db_root, name, &manifest, &files, error, error_size) != 0) {
        return -1;
    }

    /* Never remove a package that an installed package still requires. */
    for (size_t i = 0U; i < files.count; ++i) {
        (void)i;
    }
    char dependent[256] = {0};
    int has_dependent = 0;
    if (pux_db_find_reverse_dependency(db_root, &manifest, dependent, sizeof(dependent),
                                       &has_dependent, error, error_size) != 0) {
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
        return -1;
    }
    if (has_dependent != 0) {
        set_errorf(error, error_size, "cannot remove package; required by: %s", dependent);
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
        return -1;
    }

    struct stat root_st;
    if (lstat(root, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        set_error(error, error_size, "installation root is not a directory");
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
        return -1;
    }

    if (validate_removal_paths(root, db_root, name, &files, error, error_size) != 0) {
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
        return -1;
    }

    if (files.count > 1U) {
        qsort(files.items, files.count, sizeof(*files.items), compare_paths_desc);
    }

    char template[PUX_TXN_MAX_STAGING_TEMPLATE];
    const size_t root_len = strlen(root);
    const int separator = root_len != 0U && root[root_len - 1U] != '/';
    const char *suffix = "/.pux-remove-XXXXXX";
    const size_t total = root_len + (size_t)separator + strlen(suffix + 1U) + 1U;
    if (total > sizeof(template)) {
        set_error(error, error_size, "removal staging path is too long");
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
        return -1;
    }
    size_t offset = root_len;
    memcpy(template, root, root_len);
    if (separator != 0) template[offset++] = '/';
    memcpy(template + offset, suffix + 1U, strlen(suffix + 1U) + 1U);
    char *stage = mkdtemp(template);
    if (stage == NULL) {
        set_errorf(error, error_size, "cannot create removal staging directory: %s", strerror(errno));
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
        return -1;
    }

    char removed_root[PUX_TXN_MAX_PATH];
    if (path_join(stage, "removed", removed_root, sizeof(removed_root)) != 0 ||
        mkdir(removed_root, 0700) != 0) {
        set_error(error, error_size, "cannot create removal backup directory");
        remove_tree(stage);
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
        return -1;
    }

    struct moved_list moved = {0};
    if (moved_reserve(&moved, files.count == 0U ? 1U : files.count) != 0) {
        set_error(error, error_size, "out of memory tracking removal");
        remove_tree(stage);
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
        return -1;
    }

    for (size_t i = 0U; i < files.count; ++i) {
        if (files.items[i].type != 'f' && files.items[i].type != 'l') continue;
        char destination[PUX_TXN_MAX_PATH];
        char backup[PUX_TXN_MAX_PATH];
        if (path_join(root, files.items[i].path, destination, sizeof(destination)) != 0 ||
            path_join(removed_root, files.items[i].path, backup, sizeof(backup)) != 0) {
            set_error(error, error_size, "removal path is too long");
            goto rollback_remove;
        }
        if (access(destination, F_OK) != 0) {
            if (errno == ENOENT) continue;
            set_errorf(error, error_size, "cannot access installed file: %s", strerror(errno));
            goto rollback_remove;
        }
        if (ensure_directory_for_backup(removed_root, files.items[i].path,
                                         error, error_size) != 0) {
            goto rollback_remove;
        }
        /* Record the path before moving the live file so an allocation failure
         * cannot leave an untracked file in the staging area. */
        if (moved_append(&moved, files.items[i].path) != 0) {
            set_error(error, error_size, "out of memory tracking removal");
            goto rollback_remove;
        }
        if (rename(destination, backup) != 0) {
            set_errorf(error, error_size, "cannot stage installed file for removal: %s", strerror(errno));
            moved.count--;
            free(moved.paths[moved.count]);
            moved.paths[moved.count] = NULL;
            goto rollback_remove;
        }
    }

    if (pux_db_unregister_package(db_root, name, error, error_size) != 0) {
        goto rollback_remove;
    }

    /* DB commit succeeded. Remove empty package-owned directories from deepest
     * to shallowest; non-empty/shared directories are intentionally preserved. */
    for (size_t i = 0U; i < files.count; ++i) {
        if (files.items[i].type != 'd') continue;
        char destination[PUX_TXN_MAX_PATH];
        if (path_join(root, files.items[i].path, destination, sizeof(destination)) != 0) continue;
        char other_owner[256] = {0};
        int owned_elsewhere = 0;
        char local_error[512] = {0};
        if (pux_db_find_other_owner(db_root, files.items[i].path, name,
                                    other_owner, sizeof(other_owner), &owned_elsewhere,
                                    local_error, sizeof(local_error)) == 0 && owned_elsewhere == 0) {
            (void)rmdir(destination);
        }
    }

    moved_free(&moved);
    remove_tree(stage);
    pux_package_manifest_free(&manifest);
    pux_db_file_list_free(&files);
    return 0;

rollback_remove:
    for (size_t i = moved.count; i > 0U; --i) {
        const char *relative = moved.paths[i - 1U];
        char destination[PUX_TXN_MAX_PATH];
        char backup[PUX_TXN_MAX_PATH];
        if (path_join(root, relative, destination, sizeof(destination)) != 0 ||
            path_join(removed_root, relative, backup, sizeof(backup)) != 0) {
            continue;
        }
        (void)ensure_destination_parent(root, relative, NULL, 0U);
        (void)rename(backup, destination);
    }
    moved_free(&moved);
    remove_tree(stage);
    pux_package_manifest_free(&manifest);
    pux_db_file_list_free(&files);
    return -1;
}


static int version_part(const char **cursor, char *buffer, size_t buffer_size,
                        int *numeric)
{
    const char *p = *cursor;
    while (*p != '\0' && !isalnum((unsigned char)*p)) ++p;
    if (*p == '\0') {
        *cursor = p;
        buffer[0] = '\0';
        *numeric = 0;
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

static int compare_version_local(const char *left, const char *right)
{
    const char *l = left;
    const char *r = right;
    for (;;) {
        char lp[128];
        char rp[128];
        int ln = 0;
        int rn = 0;
        const int lm = version_part(&l, lp, sizeof(lp), &ln);
        const int rm = version_part(&r, rp, sizeof(rp), &rn);
        if (lm <= 0 && rm <= 0) return 0;
        if (lm <= 0) return -1;
        if (rm <= 0) return 1;

        if (ln != 0 && rn != 0) {
            const char *lz = lp;
            const char *rz = rp;
            while (*lz == '0') ++lz;
            while (*rz == '0') ++rz;
            const size_t llen = strlen(lz);
            const size_t rlen = strlen(rz);
            if (llen != rlen) return llen < rlen ? -1 : 1;
            const int numeric_cmp = strcmp(lz, rz);
            if (numeric_cmp != 0) return numeric_cmp < 0 ? -1 : 1;
        } else if (ln != rn) {
            return ln != 0 ? 1 : -1;
        } else {
            const int lexical = strcmp(lp, rp);
            if (lexical != 0) return lexical < 0 ? -1 : 1;
        }
    }
}

enum upgrade_req_op {
    UPGRADE_REQ_ANY = 0,
    UPGRADE_REQ_EQ,
    UPGRADE_REQ_LT,
    UPGRADE_REQ_LE,
    UPGRADE_REQ_GT,
    UPGRADE_REQ_GE
};

struct upgrade_requirement {
    char *name;
    char *version;
    enum upgrade_req_op op;
};

static void upgrade_requirement_free(struct upgrade_requirement *req)
{
    free(req->name);
    free(req->version);
    req->name = NULL;
    req->version = NULL;
    req->op = UPGRADE_REQ_ANY;
}

static int parse_upgrade_requirement(const char *expression,
                                     struct upgrade_requirement *req,
                                     char *error, size_t error_size)
{
    memset(req, 0, sizeof(*req));
    req->op = UPGRADE_REQ_ANY;
    const char *op_pos = strpbrk(expression, "<>=");
    const size_t name_len = op_pos == NULL ? strlen(expression) : (size_t)(op_pos - expression);
    if (name_len == 0U || name_len >= 256U) {
        set_error(error, error_size, "invalid dependency expression");
        return -1;
    }

    req->name = duplicate_string(expression);
    if (req->name == NULL) {
        set_error(error, error_size, "out of memory parsing dependency");
        return -1;
    }
    req->name[name_len] = '\0';

    if (op_pos == NULL) return 0;
    const char *version = op_pos;
    if (version[1] == '=') {
        if (version[0] == '<') req->op = UPGRADE_REQ_LE;
        else if (version[0] == '>') req->op = UPGRADE_REQ_GE;
        else req->op = UPGRADE_REQ_EQ;
        version += 2;
    } else {
        if (version[0] == '<') req->op = UPGRADE_REQ_LT;
        else if (version[0] == '>') req->op = UPGRADE_REQ_GT;
        else req->op = UPGRADE_REQ_EQ;
        ++version;
    }
    if (*version == '\0' || strlen(version) >= 256U || strpbrk(version, " \t\r\n") != NULL) {
        upgrade_requirement_free(req);
        set_error(error, error_size, "invalid dependency version constraint");
        return -1;
    }
    req->version = duplicate_string(version);
    if (req->version == NULL) {
        upgrade_requirement_free(req);
        set_error(error, error_size, "out of memory parsing dependency version");
        return -1;
    }
    return 0;
}

static int manifest_has_capability(const struct pux_package_manifest *manifest,
                                   const char *capability)
{
    if (strcmp(manifest->name, capability) == 0) return 1;
    for (size_t i = 0U; i < manifest->provides.count; ++i) {
        if (strcmp(manifest->provides.items[i], capability) == 0) return 1;
    }
    return 0;
}

static int upgrade_requirement_satisfied_by_manifest(
    const struct upgrade_requirement *req,
    const struct pux_package_manifest *manifest)
{
    if (req->op == UPGRADE_REQ_ANY) return manifest_has_capability(manifest, req->name);
    if (strcmp(req->name, manifest->name) != 0) return 0;
    const int cmp = compare_version_local(manifest->version, req->version);
    switch (req->op) {
        case UPGRADE_REQ_EQ: return cmp == 0;
        case UPGRADE_REQ_LT: return cmp < 0;
        case UPGRADE_REQ_LE: return cmp <= 0;
        case UPGRADE_REQ_GT: return cmp > 0;
        case UPGRADE_REQ_GE: return cmp >= 0;
        case UPGRADE_REQ_ANY: break;
    }
    return 0;
}

static int dependency_check_for_replacement(
    const char *db_root,
    const struct pux_package_manifest *old_manifest,
    const struct pux_package_manifest *new_manifest,
    char *error, size_t error_size)
{
    for (size_t i = 0U; i < new_manifest->depends.count; ++i) {
        struct upgrade_requirement req;
        if (parse_upgrade_requirement(new_manifest->depends.items[i], &req,
                                      error, error_size) != 0) return -1;

        int satisfied = 0;
        if (upgrade_requirement_satisfied_by_manifest(&req, new_manifest) != 0) {
            satisfied = 1;
        } else if (req.op == UPGRADE_REQ_ANY &&
                   manifest_has_capability(old_manifest, req.name)) {
            satisfied = manifest_has_capability(new_manifest, req.name);
        } else if (strcmp(req.name, new_manifest->name) != 0) {
            char db_error[512] = {0};
            if (pux_db_dependency_satisfied(db_root, new_manifest->depends.items[i],
                                            &satisfied, db_error, sizeof(db_error)) != 0) {
                set_errorf(error, error_size, "cannot check dependency: %s", db_error);
                upgrade_requirement_free(&req);
                return -1;
            }
        }

        if (satisfied == 0) {
            set_errorf(error, error_size, "unsatisfied dependency after upgrade: %s",
                       new_manifest->depends.items[i]);
            upgrade_requirement_free(&req);
            return -1;
        }
        upgrade_requirement_free(&req);
    }
    return 0;
}

static int upgrade_plan_dependent_check(
    const char *db_root,
    const struct pux_package_manifest *old_manifest,
    const struct pux_package_manifest *new_manifest,
    char *error, size_t error_size)
{
    char packages_dir[4096];
    if (snprintf(packages_dir, sizeof(packages_dir), "%s/packages", db_root) < 0 ||
        strlen(db_root) + strlen("/packages") + 1U > sizeof(packages_dir)) {
        set_error(error, error_size, "database path is too long");
        return -1;
    }

    DIR *dir = opendir(packages_dir);
    if (dir == NULL) {
        set_errorf(error, error_size, "cannot open package database: %s", strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const size_t length = strlen(entry->d_name);
        const size_t suffix_len = strlen(".record");
        if (length <= suffix_len || strcmp(entry->d_name + length - suffix_len, ".record") != 0) {
            continue;
        }
        char name[256];
        const size_t name_len = length - suffix_len;
        if (name_len == 0U || name_len + 1U > sizeof(name) ||
            strcmp(entry->d_name, "") == 0) {
            continue;
        }
        memcpy(name, entry->d_name, name_len);
        name[name_len] = '\0';
        if (strcmp(name, old_manifest->name) == 0) continue;

        struct pux_package_manifest installed;
        struct pux_db_file_list files = {0};
        char db_error[512] = {0};
        if (pux_db_read_package(db_root, name, &installed, &files,
                                db_error, sizeof(db_error)) != 0) {
            closedir(dir);
            set_errorf(error, error_size, "cannot read dependent package: %s", db_error);
            return -1;
        }

        for (size_t i = 0U; i < installed.depends.count; ++i) {
            struct upgrade_requirement req;
            if (parse_upgrade_requirement(installed.depends.items[i], &req,
                                          error, error_size) != 0) {
                pux_package_manifest_free(&installed);
                pux_db_file_list_free(&files);
                closedir(dir);
                return -1;
            }
            if (manifest_has_capability(old_manifest, req.name)) {
                if (upgrade_requirement_satisfied_by_manifest(&req, new_manifest) == 0) {
                    if (error != NULL && error_size > 0U) {
                        (void)snprintf(error, error_size,
                                       "upgrade would break dependency of %s: %s",
                                       installed.name, installed.depends.items[i]);
                    }
                    upgrade_requirement_free(&req);
                    pux_package_manifest_free(&installed);
                    pux_db_file_list_free(&files);
                    closedir(dir);
                    return -1;
                }
            }
            upgrade_requirement_free(&req);
        }

        pux_package_manifest_free(&installed);
        pux_db_file_list_free(&files);
    }
    closedir(dir);
    return 0;
}

static int upgrade_conflict_check(
    const char *db_root,
    const struct pux_package_manifest *old_manifest,
    const struct pux_package_manifest *new_manifest,
    char *error, size_t error_size)
{
    for (size_t i = 0U; i < new_manifest->conflicts.count; ++i) {
        if (strpbrk(new_manifest->conflicts.items[i], "<>=") != NULL) {
            set_error(error, error_size, "versioned conflicts are not supported yet");
            return -1;
        }
    }

    char packages_dir[4096];
    if (snprintf(packages_dir, sizeof(packages_dir), "%s/packages", db_root) < 0 ||
        strlen(db_root) + strlen("/packages") + 1U > sizeof(packages_dir)) {
        set_error(error, error_size, "database path is too long");
        return -1;
    }
    DIR *dir = opendir(packages_dir);
    if (dir == NULL) {
        set_errorf(error, error_size, "cannot open package database: %s", strerror(errno));
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const size_t length = strlen(entry->d_name);
        const size_t suffix_len = strlen(".record");
        if (length <= suffix_len || strcmp(entry->d_name + length - suffix_len, ".record") != 0) continue;
        char name[256];
        const size_t name_len = length - suffix_len;
        if (name_len == 0U || name_len + 1U > sizeof(name)) continue;
        memcpy(name, entry->d_name, name_len);
        name[name_len] = '\0';
        if (strcmp(name, old_manifest->name) == 0) continue;

        struct pux_package_manifest installed;
        struct pux_db_file_list files = {0};
        char db_error[512] = {0};
        if (pux_db_read_package(db_root, name, &installed, &files,
                                db_error, sizeof(db_error)) != 0) {
            closedir(dir);
            set_errorf(error, error_size, "cannot read installed package: %s", db_error);
            return -1;
        }

        for (size_t c = 0U; c < new_manifest->conflicts.count; ++c) {
            if (manifest_has_capability(&installed, new_manifest->conflicts.items[c]) != 0) {
                if (error != NULL && error_size > 0U) {
                    (void)snprintf(error, error_size, "package conflict with %s: %s",
                                   installed.name, new_manifest->conflicts.items[c]);
                }
                pux_package_manifest_free(&installed);
                pux_db_file_list_free(&files);
                closedir(dir);
                return -1;
            }
        }
        for (size_t c = 0U; c < installed.conflicts.count; ++c) {
            if (strpbrk(installed.conflicts.items[c], "<>=") != NULL) {
                set_error(error, error_size, "versioned conflicts are not supported yet");
                pux_package_manifest_free(&installed);
                pux_db_file_list_free(&files);
                closedir(dir);
                return -1;
            }
            if (manifest_has_capability(new_manifest, installed.conflicts.items[c]) != 0) {
                set_errorf(error, error_size, "upgrade conflicts with installed package: %s",
                           installed.name);
                pux_package_manifest_free(&installed);
                pux_db_file_list_free(&files);
                closedir(dir);
                return -1;
            }
        }

        pux_package_manifest_free(&installed);
        pux_db_file_list_free(&files);
    }
    closedir(dir);
    return 0;
}

static int list_contains_path(const struct pux_db_file_list *files,
                              char type, const char *path)
{
    for (size_t i = 0U; i < files->count; ++i) {
        if (files->items[i].type == type && strcmp(files->items[i].path, path) == 0) return 1;
    }
    return 0;
}

static int list_contains_any_path(const struct pux_db_file_list *files, const char *path)
{
    for (size_t i = 0U; i < files->count; ++i) {
        if (strcmp(files->items[i].path, path) == 0) return 1;
    }
    return 0;
}

/* Upgrade one installed package in-place. The transaction is atomic for this
 * package; repository-wide multi-package rollback remains a later milestone. */
int pux_upgrade_package(const char *package_path,
                        const char *root,
                        const char *db_root,
                        char *error,
                        size_t error_size)
{
    if (package_path == NULL || root == NULL || db_root == NULL ||
        root[0] == '\0' || db_root[0] == '\0') {
        set_error(error, error_size, "invalid upgrade argument");
        return -1;
    }

    struct pux_package_manifest new_manifest;
    if (pux_package_archive_validate(package_path, &new_manifest, error, error_size) != 0) return -1;

    char arch[128];
    if (target_arch(arch, sizeof(arch), error, error_size) != 0) {
        pux_package_manifest_free(&new_manifest);
        return -1;
    }
    if (strcmp(new_manifest.arch, "noarch") != 0 && strcmp(new_manifest.arch, arch) != 0) {
        set_errorf(error, error_size, "package architecture does not match host: %s", new_manifest.arch);
        pux_package_manifest_free(&new_manifest);
        return -1;
    }

    struct pux_package_manifest old_manifest;
    struct pux_db_file_list old_files = {0};
    if (pux_db_read_package(db_root, new_manifest.name, &old_manifest, &old_files,
                            error, error_size) != 0) {
        pux_package_manifest_free(&new_manifest);
        return -1;
    }

    const int version_cmp = compare_version_local(new_manifest.version, old_manifest.version);
    if (version_cmp == 0 && new_manifest.release == old_manifest.release) {
        set_error(error, error_size, "package is already at requested version");
        pux_package_manifest_free(&old_manifest);
        pux_db_file_list_free(&old_files);
        pux_package_manifest_free(&new_manifest);
        return 1;
    }
    if (version_cmp < 0 || (version_cmp == 0 && new_manifest.release < old_manifest.release)) {
        set_error(error, error_size, "repository package is older than installed version");
        pux_package_manifest_free(&old_manifest);
        pux_db_file_list_free(&old_files);
        pux_package_manifest_free(&new_manifest);
        return 1;
    }

    if (dependency_check_for_replacement(db_root, &old_manifest, &new_manifest,
                                         error, error_size) != 0 ||
        upgrade_plan_dependent_check(db_root, &old_manifest, &new_manifest,
                                     error, error_size) != 0 ||
        upgrade_conflict_check(db_root, &old_manifest, &new_manifest,
                               error, error_size) != 0) {
        pux_package_manifest_free(&old_manifest);
        pux_db_file_list_free(&old_files);
        pux_package_manifest_free(&new_manifest);
        return -1;
    }

    struct stat root_st;
    if (lstat(root, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        set_error(error, error_size, "installation root is not a directory");
        pux_package_manifest_free(&old_manifest);
        pux_db_file_list_free(&old_files);
        pux_package_manifest_free(&new_manifest);
        return -1;
    }
    if (validate_removal_paths(root, db_root, old_manifest.name, &old_files,
                               error, error_size) != 0) {
        pux_package_manifest_free(&old_manifest);
        pux_db_file_list_free(&old_files);
        pux_package_manifest_free(&new_manifest);
        return -1;
    }

    char template[PUX_TXN_MAX_STAGING_TEMPLATE];
    const size_t root_len = strlen(root);
    const int separator = root_len != 0U && root[root_len - 1U] != '/';
    const char *suffix = ".pux-upgrade-XXXXXX";
    const size_t total = root_len + (size_t)separator + strlen(suffix) + 1U;
    if (total > sizeof(template)) {
        set_error(error, error_size, "upgrade staging path is too long");
        pux_package_manifest_free(&old_manifest);
        pux_db_file_list_free(&old_files);
        pux_package_manifest_free(&new_manifest);
        return -1;
    }
    size_t offset = root_len;
    memcpy(template, root, root_len);
    if (separator != 0) template[offset++] = '/';
    memcpy(template + offset, suffix, strlen(suffix) + 1U);
    char *stage = mkdtemp(template);
    if (stage == NULL) {
        set_errorf(error, error_size, "cannot create upgrade staging directory: %s", strerror(errno));
        pux_package_manifest_free(&old_manifest);
        pux_db_file_list_free(&old_files);
        pux_package_manifest_free(&new_manifest);
        return -1;
    }

    char payload_stage[PUX_TXN_MAX_PATH];
    char removed_root[PUX_TXN_MAX_PATH];
    if (path_join(stage, "payload-root", payload_stage, sizeof(payload_stage)) != 0 ||
        path_join(stage, "removed", removed_root, sizeof(removed_root)) != 0 ||
        mkdir(payload_stage, 0755) != 0 || mkdir(removed_root, 0700) != 0) {
        set_error(error, error_size, "cannot create upgrade staging directories");
        remove_tree(stage);
        pux_package_manifest_free(&old_manifest);
        pux_db_file_list_free(&old_files);
        pux_package_manifest_free(&new_manifest);
        return -1;
    }

    char extract_error[512] = {0};
    if (pux_package_archive_extract(package_path, payload_stage,
                                    extract_error, sizeof(extract_error)) != 0) {
        set_errorf(error, error_size, "cannot stage upgrade package: %s", extract_error);
        remove_tree(stage);
        pux_package_manifest_free(&old_manifest);
        pux_db_file_list_free(&old_files);
        pux_package_manifest_free(&new_manifest);
        return -1;
    }

    struct pux_db_file_list new_files = {0};
    if (collect_tree(payload_stage, "", &new_files, error, error_size) != 0 || new_files.count == 0U) {
        if (new_files.count == 0U) set_error(error, error_size, "upgrade package contains no payload files");
        pux_db_file_list_free(&new_files);
        remove_tree(stage);
        pux_package_manifest_free(&old_manifest);
        pux_db_file_list_free(&old_files);
        pux_package_manifest_free(&new_manifest);
        return -1;
    }

    for (size_t i = 0U; i < new_files.count; ++i) {
        char owner[256] = {0};
        int owned = 0;
        if (pux_db_find_owner(db_root, new_files.items[i].path, owner, sizeof(owner),
                              &owned, error, error_size) != 0) goto upgrade_fail_preflight;
        if (owned != 0 && strcmp(owner, old_manifest.name) != 0 &&
            (new_files.items[i].type == 'f' || new_files.items[i].type == 'l')) {
            set_errorf(error, error_size, "upgrade file is owned by another package: %s", owner);
            goto upgrade_fail_preflight;
        }

        char destination[PUX_TXN_MAX_PATH];
        if (path_join(root, new_files.items[i].path, destination, sizeof(destination)) != 0) {
            set_error(error, error_size, "upgrade destination path is too long");
            goto upgrade_fail_preflight;
        }
        struct stat st;
        if (lstat(destination, &st) == 0) {
            if (new_files.items[i].type == 'f' && !S_ISREG(st.st_mode)) {
                set_errorf(error, error_size, "upgrade file conflicts with existing path: %s",
                           new_files.items[i].path);
                goto upgrade_fail_preflight;
            }
            if (new_files.items[i].type == 'd' && !S_ISDIR(st.st_mode)) {
                set_errorf(error, error_size, "upgrade directory conflicts with existing path: %s",
                           new_files.items[i].path);
                goto upgrade_fail_preflight;
            }
            if (new_files.items[i].type == 'l' && !S_ISLNK(st.st_mode)) {
                set_errorf(error, error_size, "upgrade symlink conflicts with existing path: %s",
                           new_files.items[i].path);
                goto upgrade_fail_preflight;
            }
        } else if (errno != ENOENT) {
            set_errorf(error, error_size, "cannot inspect upgrade destination: %s", strerror(errno));
            goto upgrade_fail_preflight;
        }
    }

    for (size_t i = 0U; i < old_files.count; ++i) {
        if (list_contains_path(&new_files, old_files.items[i].type, old_files.items[i].path) == 0 &&
            list_contains_any_path(&new_files, old_files.items[i].path) != 0) {
            set_errorf(error, error_size, "package file type changed during upgrade: %s",
                       old_files.items[i].path);
            goto upgrade_fail_preflight;
        }
    }

    struct moved_list old_moved = {0};
    struct moved_list new_moved = {0};
    struct moved_list created_dirs = {0};
    if (moved_reserve(&old_moved, old_files.count == 0U ? 1U : old_files.count) != 0 ||
        moved_reserve(&new_moved, new_files.count == 0U ? 1U : new_files.count) != 0 ||
        moved_reserve(&created_dirs, new_files.count == 0U ? 1U : new_files.count) != 0) {
        set_error(error, error_size, "out of memory tracking upgrade rollback");
        moved_free(&old_moved);
        moved_free(&new_moved);
        moved_free(&created_dirs);
        goto upgrade_fail_preflight;
    }

    for (size_t i = 0U; i < old_files.count; ++i) {
        if (old_files.items[i].type != 'f' && old_files.items[i].type != 'l') continue;
        char destination[PUX_TXN_MAX_PATH];
        char backup[PUX_TXN_MAX_PATH];
        if (path_join(root, old_files.items[i].path, destination, sizeof(destination)) != 0 ||
            path_join(removed_root, old_files.items[i].path, backup, sizeof(backup)) != 0) {
            set_error(error, error_size, "upgrade path is too long");
            goto upgrade_rollback;
        }
        if (lstat(destination, &(struct stat){0}) != 0) {
            if (errno == ENOENT) continue;
            set_errorf(error, error_size, "cannot inspect installed file: %s", strerror(errno));
            goto upgrade_rollback;
        }
        if (ensure_directory_for_backup(removed_root, old_files.items[i].path,
                                         error, error_size) != 0) goto upgrade_rollback;
        if (moved_append(&old_moved, old_files.items[i].path) != 0) {
            set_error(error, error_size, "out of memory tracking old package files");
            goto upgrade_rollback;
        }
        if (rename(destination, backup) != 0) {
            set_errorf(error, error_size, "cannot stage old package file: %s", strerror(errno));
            free(old_moved.paths[old_moved.count - 1U]);
            old_moved.paths[--old_moved.count] = NULL;
            goto upgrade_rollback;
        }
    }

    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0U; i < new_files.count; ++i) {
            if (pass == 0) {
                if (new_files.items[i].type != 'd') continue;
            } else if (new_files.items[i].type != 'f' && new_files.items[i].type != 'l') {
                continue;
            }
            const char *relative = new_files.items[i].path;
            char stage_relative[PUX_TXN_MAX_PATH];
            char stage_path[PUX_TXN_MAX_PATH];
            char destination[PUX_TXN_MAX_PATH];
            if (path_join("payload-root", relative, stage_relative, sizeof(stage_relative)) != 0 ||
                path_join(stage, stage_relative, stage_path, sizeof(stage_path)) != 0 ||
                path_join(root, relative, destination, sizeof(destination)) != 0) {
                set_error(error, error_size, "upgrade path is too long");
                goto upgrade_rollback;
            }

            if (new_files.items[i].type == 'd') {
                struct stat st;
                if (lstat(destination, &st) == 0) {
                    if (!S_ISDIR(st.st_mode)) {
                        set_errorf(error, error_size, "destination directory conflicts with existing path: %s", relative);
                        goto upgrade_rollback;
                    }
                    continue;
                }
                if (errno != ENOENT) {
                    set_errorf(error, error_size, "cannot inspect upgrade directory: %s", strerror(errno));
                    goto upgrade_rollback;
                }
                struct stat staged_st;
                if (lstat(stage_path, &staged_st) != 0 ||
                    mkdir(destination, (mode_t)(staged_st.st_mode & 07777U)) != 0) {
                    set_errorf(error, error_size, "cannot create upgrade directory: %s", strerror(errno));
                    goto upgrade_rollback;
                }
                if (moved_append(&created_dirs, destination) != 0) {
                    set_error(error, error_size, "out of memory tracking upgrade directories");
                    goto upgrade_rollback;
                }
            } else {
                if (ensure_destination_parent(root, relative, error, error_size) != 0 ||
                    rename(stage_path, destination) != 0) {
                    if (error[0] == '\0') set_errorf(error, error_size, "cannot commit upgraded file: %s", strerror(errno));
                    goto upgrade_rollback;
                }
                if (moved_append(&new_moved, destination) != 0) {
                    set_error(error, error_size, "out of memory tracking new package files");
                    goto upgrade_rollback;
                }
            }
        }
    }

    if (pux_db_register_package(db_root, &new_manifest, &new_files,
                                error, error_size) != 0) goto upgrade_rollback;

    for (size_t i = old_files.count; i > 0U; --i) {
        const struct pux_db_file_entry *old_entry = &old_files.items[i - 1U];
        if (old_entry->type != 'd' || list_contains_path(&new_files, 'd', old_entry->path) != 0) continue;
        char destination[PUX_TXN_MAX_PATH];
        if (path_join(root, old_entry->path, destination, sizeof(destination)) != 0) continue;
        char other_owner[256] = {0};
        int owned_elsewhere = 0;
        char local_error[512] = {0};
        if (pux_db_find_other_owner(db_root, old_entry->path, new_manifest.name,
                                    other_owner, sizeof(other_owner), &owned_elsewhere,
                                    local_error, sizeof(local_error)) == 0 && owned_elsewhere == 0) {
            (void)rmdir(destination);
        }
    }

    moved_free(&old_moved);
    moved_free(&new_moved);
    moved_free(&created_dirs);
    remove_tree(stage);
    pux_db_file_list_free(&new_files);
    pux_package_manifest_free(&old_manifest);
    pux_db_file_list_free(&old_files);
    pux_package_manifest_free(&new_manifest);
    return 0;

upgrade_rollback:
    for (size_t i = new_moved.count; i > 0U; --i) {
        (void)unlink(new_moved.paths[i - 1U]);
    }
    for (size_t i = created_dirs.count; i > 0U; --i) {
        (void)rmdir(created_dirs.paths[i - 1U]);
    }
    for (size_t i = old_moved.count; i > 0U; --i) {
        const char *relative = old_moved.paths[i - 1U];
        char destination[PUX_TXN_MAX_PATH];
        char backup[PUX_TXN_MAX_PATH];
        if (path_join(root, relative, destination, sizeof(destination)) != 0 ||
            path_join(removed_root, relative, backup, sizeof(backup)) != 0) continue;
        (void)ensure_destination_parent(root, relative, NULL, 0U);
        (void)rename(backup, destination);
    }
    moved_free(&old_moved);
    moved_free(&new_moved);
    moved_free(&created_dirs);

upgrade_fail_preflight:
    remove_tree(stage);
    pux_db_file_list_free(&new_files);
    pux_package_manifest_free(&old_manifest);
    pux_db_file_list_free(&old_files);
    pux_package_manifest_free(&new_manifest);
    return -1;
}
