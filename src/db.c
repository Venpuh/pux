#define _GNU_SOURCE
#include "pux/db.h"

#include <ctype.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static void set_errorf(char *error, size_t error_size, const char *format,
                       const char *value)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, format, value);
    }
}

static char *duplicate_string(const char *value)
{
    const size_t length = strlen(value);
    char *copy = malloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value, length + 1U);
    return copy;
}

static char *duplicate_range(const char *start, size_t length)
{
    char *copy = malloc(length + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, start, length);
    copy[length] = '\0';
    return copy;
}

static int valid_package_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    for (size_t i = 0U; name[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (!(c == '-' || c == '_' || c == '.' || c == '+' || c == ':' ||
              (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z'))) {
            return 0;
        }
    }
    return 1;
}

static int safe_relative_path(const char *path)
{
    if (path == NULL || path[0] == '\0' || path[0] == '/') {
        return 0;
    }
    const size_t length = strlen(path);
    if (length >= 4096U || path[length - 1U] == '/') {
        return 0;
    }

    size_t component_start = 0U;
    for (size_t i = 0U; i <= length; ++i) {
        if (i < length && path[i] != '/') {
            continue;
        }
        const size_t component_length = i - component_start;
        if (component_length == 0U ||
            (component_length == 1U && path[component_start] == '.') ||
            (component_length == 2U && path[component_start] == '.' &&
             path[component_start + 1U] == '.')) {
            return 0;
        }
        component_start = i + 1U;
    }
    return 1;
}

static void free_entry(struct pux_db_file_entry *entry)
{
    free(entry->path);
    free(entry->target);
    entry->path = NULL;
    entry->target = NULL;
    entry->type = '\0';
}

void pux_db_file_list_free(struct pux_db_file_list *files)
{
    if (files == NULL) {
        return;
    }
    for (size_t i = 0U; i < files->count; ++i) {
        free_entry(&files->items[i]);
    }
    free(files->items);
    files->items = NULL;
    files->count = 0U;
}

static int append_file(struct pux_db_file_list *files, char type, const char *path, const char *target)
{
    if (files->count >= PUX_DB_MAX_FILE_LIST ||
        (type != 'f' && type != 'd' && type != 'l') || !safe_relative_path(path) ||
        (type == 'l' && (target == NULL || target[0] == '\0' || strlen(target) >= 4096U))) {
        return -1;
    }

    for (size_t i = 0U; i < files->count; ++i) {
        if (strcmp(files->items[i].path, path) == 0) {
            return -1;
        }
    }

    char *copy = duplicate_string(path);
    char *target_copy = target == NULL ? NULL : duplicate_string(target);
    if (copy == NULL || (target != NULL && target_copy == NULL)) {
        free(copy);
        free(target_copy);
        return -1;
    }
    struct pux_db_file_entry *items = realloc(
        files->items, (files->count + 1U) * sizeof(*items));
    if (items == NULL) {
        free(copy);
        return -1;
    }

    files->items = items;
    files->items[files->count].type = type;
    files->items[files->count].path = copy;
    files->items[files->count].target = target_copy;
    files->count++;
    return 0;
}

static int compare_file_entries(const void *left, const void *right)
{
    const struct pux_db_file_entry *a = left;
    const struct pux_db_file_entry *b = right;
    const int by_path = strcmp(a->path, b->path);
    if (by_path != 0) {
        return by_path;
    }
    return (int)a->type - (int)b->type;
}

static int ensure_dir(const char *path, mode_t mode,
                      char *error, size_t error_size)
{
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            set_errorf(error, error_size, "database path is not a directory: %s", path);
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT) {
        set_errorf(error, error_size, "cannot inspect database directory: %s", strerror(errno));
        return -1;
    }
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        set_errorf(error, error_size, "cannot create database directory: %s", strerror(errno));
        return -1;
    }
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_error(error, error_size, "database directory was replaced by a non-directory");
        return -1;
    }
    return 0;
}

static int prepare_database(const char *db_root, char *packages_dir, size_t packages_size,
                            char *error, size_t error_size)
{
    if (db_root == NULL || db_root[0] == '\0') {
        set_error(error, error_size, "invalid database root");
        return -1;
    }

    if (strlen(db_root) + strlen("/packages") + 1U > packages_size) {
        set_error(error, error_size, "database root path is too long");
        return -1;
    }

    if (ensure_dir(db_root, 0755U, error, error_size) != 0) {
        return -1;
    }

    (void)snprintf(packages_dir, packages_size, "%s/packages", db_root);
    return ensure_dir(packages_dir, 0755U, error, error_size);
}

static int make_record_path(const char *packages_dir, const char *name,
                            char *path, size_t path_size,
                            char *error, size_t error_size)
{
    if (!valid_package_name(name)) {
        set_error(error, error_size, "invalid package name");
        return -1;
    }
    if (snprintf(path, path_size, "%s/%s.record", packages_dir, name) < 0 ||
        strlen(packages_dir) + strlen(name) + strlen("/.record") + 1U > path_size) {
        set_error(error, error_size, "package record path is too long");
        return -1;
    }
    return 0;
}


static int write_file_list(FILE *file, const struct pux_db_file_list *files)
{
    for (size_t i = 0U; i < files->count; ++i) {
        if (files->items[i].type == 'l') {
            if (fprintf(file, "l %s -> %s\n", files->items[i].path, files->items[i].target) < 0) return -1;
        } else if (fprintf(file, "%c %s\n", files->items[i].type, files->items[i].path) < 0) {
            return -1;
        }
    }
    return 0;
}

static int read_entire_file(const char *path, unsigned char **buffer, size_t *size,
                            char *error, size_t error_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_errorf(error, error_size, "cannot open file: %s", strerror(errno));
        return -1;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        set_error(error, error_size, "cannot seek file");
        return -1;
    }
    const long end = ftell(file);
    if (end < 0L || (unsigned long)end > (unsigned long)PUX_DB_MAX_RECORD_SIZE) {
        fclose(file);
        set_error(error, error_size, "file is too large");
        return -1;
    }
    if (fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        set_error(error, error_size, "cannot rewind file");
        return -1;
    }

    const size_t length = (size_t)end;
    unsigned char *data = malloc(length == 0U ? 1U : length);
    if (data == NULL) {
        fclose(file);
        set_error(error, error_size, "out of memory while reading file");
        return -1;
    }
    if (length > 0U && fread(data, 1U, length, file) != length) {
        free(data);
        fclose(file);
        set_error(error, error_size, "cannot read file");
        return -1;
    }
    if (fclose(file) != 0) {
        free(data);
        set_error(error, error_size, "cannot close file");
        return -1;
    }
    *buffer = data;
    *size = length;
    return 0;
}

int pux_db_read_file_list(const char *path, struct pux_db_file_list *files,
                          char *error, size_t error_size)
{
    if (path == NULL || files == NULL) {
        set_error(error, error_size, "invalid file list argument");
        return -1;
    }
    pux_db_file_list_free(files);

    unsigned char *buffer = NULL;
    size_t size = 0U;
    if (read_entire_file(path, &buffer, &size, error, error_size) != 0) {
        return -1;
    }

    char *text = malloc(size + 1U);
    if (text == NULL) {
        free(buffer);
        set_error(error, error_size, "out of memory while reading file list");
        return -1;
    }
    memcpy(text, buffer, size);
    text[size] = '\0';
    free(buffer);

    char *cursor = text;
    while (*cursor != '\0') {
        char *line_end = strchr(cursor, '\n');
        if (line_end != NULL) {
            *line_end = '\0';
        }
        if (strlen(cursor) > 4095U) {
            free(text);
            pux_db_file_list_free(files);
            set_error(error, error_size, "file list line is too long");
            return -1;
        }
        if (cursor[0] != '\0' && cursor[0] != '#') {
            const size_t length = strlen(cursor);
            const char type = cursor[0];
            if (type == 'l') {
                char *arrow = strstr(cursor + 2U, " -> " );
                if (arrow == NULL || arrow == cursor + 2U || arrow[4] == '\0') {
                    free(text); pux_db_file_list_free(files);
                    set_error(error, error_size, "invalid symbolic link file list entry");
                    return -1;
                }
                *arrow = '\0';
                if (append_file(files, type, cursor + 2U, arrow + 4U) != 0) {
                    free(text); pux_db_file_list_free(files);
                    set_error(error, error_size, "invalid or duplicate file list entry");
                    return -1;
                }
            } else if (length < 3U || cursor[1] != ' ' || append_file(files, type, cursor + 2U, NULL) != 0) {
                free(text);
                pux_db_file_list_free(files);
                set_error(error, error_size, "invalid or duplicate file list entry");
                return -1;
            }
        }
        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }

    free(text);
    if (files->count > 1U) qsort(files->items, files->count, sizeof(*files->items), compare_file_entries);
    return 0;
}

static int create_temp_record(const char *record_path, int *fd,
                              char *temp_path, size_t temp_path_size,
                              char *error, size_t error_size)
{
    const unsigned long pid = (unsigned long)getpid();
    if (snprintf(temp_path, temp_path_size, "%s.tmp.%lu", record_path, pid) < 0 ||
        strlen(record_path) + 32U > temp_path_size) {
        set_error(error, error_size, "temporary record path is too long");
        return -1;
    }

    *fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (*fd < 0) {
        set_errorf(error, error_size, "cannot create temporary database record: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int pux_db_register_package(const char *db_root,
                            const struct pux_package_manifest *manifest,
                            const struct pux_db_file_list *files,
                            char *error, size_t error_size)
{
    if (db_root == NULL || manifest == NULL || files == NULL) {
        set_error(error, error_size, "invalid package database registration argument");
        return -1;
    }
    if (pux_package_manifest_validate(manifest, error, error_size) != 0) {
        return -1;
    }
    if (!valid_package_name(manifest->name) || manifest->name[0] == '\0') {
        set_error(error, error_size, "invalid package name");
        return -1;
    }
    for (size_t i = 0U; i < files->count; ++i) {
        if ((files->items[i].type != 'f' && files->items[i].type != 'd' && files->items[i].type != 'l') ||
            !safe_relative_path(files->items[i].path)) {
            set_error(error, error_size, "invalid database file path");
            return -1;
        }
        if (files->items[i].type == 'l' &&
            (files->items[i].target == NULL || files->items[i].target[0] == '\0' || strlen(files->items[i].target) >= 4096U)) {
            set_error(error, error_size, "invalid symbolic link target");
            return -1;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (strcmp(files->items[j].path, files->items[i].path) == 0) {
                set_error(error, error_size, "duplicate database file path");
                return -1;
            }
        }
    }

    char packages_dir[4096];
    if (prepare_database(db_root, packages_dir, sizeof(packages_dir), error, error_size) != 0) {
        return -1;
    }

    char record_path[4096];
    if (make_record_path(packages_dir, manifest->name, record_path, sizeof(record_path),
                         error, error_size) != 0) {
        return -1;
    }

    char temp_path[4096];
    int fd = -1;
    if (create_temp_record(record_path, &fd, temp_path, sizeof(temp_path), error, error_size) != 0) {
        return -1;
    }

    FILE *file = fdopen(fd, "w");
    if (file == NULL) {
        const int saved_errno = errno;
        close(fd);
        unlink(temp_path);
        set_errorf(error, error_size, "cannot open temporary database record: %s", strerror(saved_errno));
        return -1;
    }

    int result = 0;
    if (fprintf(file, "%s\n", PUX_DB_RECORD_HEADER) < 0) {
        result = -1;
    }
    if (result == 0 && pux_package_manifest_write_stream(manifest, file) != 0) {
        result = -1;
    }
    if (result == 0 && fprintf(file, "%s\n", PUX_DB_FILES_MARKER) < 0) {
        result = -1;
    }
    if (result == 0 && write_file_list(file, files) != 0) {
        result = -1;
    }

    if (fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0) {
        result = -1;
    }

    if (result != 0) {
        unlink(temp_path);
        set_error(error, error_size, "cannot write database record");
        return -1;
    }

    if (rename(temp_path, record_path) != 0) {
        const int saved_errno = errno;
        unlink(temp_path);
        set_errorf(error, error_size, "cannot replace database record: %s", strerror(saved_errno));
        return -1;
    }
    return 0;
}

static int parse_record_buffer(const unsigned char *buffer, size_t size,
                               struct pux_package_manifest *manifest,
                               struct pux_db_file_list *files,
                               char *error, size_t error_size)
{
    pux_package_manifest_init(manifest);
    pux_db_file_list_free(files);

    char *text = malloc(size + 1U);
    if (text == NULL) {
        set_error(error, error_size, "out of memory while reading database record");
        return -1;
    }
    memcpy(text, buffer, size);
    text[size] = '\0';

    char *marker = strstr(text, "\n" PUX_DB_FILES_MARKER "\n");
    if (marker == NULL) {
        free(text);
        set_error(error, error_size, "invalid database record: missing files section");
        return -1;
    }

    *marker = '\0';
    const char *manifest_text = text;
    const size_t header_length = strlen(PUX_DB_RECORD_HEADER);
    if (strncmp(manifest_text, PUX_DB_RECORD_HEADER, header_length) != 0 ||
        manifest_text[header_length] != '\n') {
        free(text);
        set_error(error, error_size, "invalid database record header");
        return -1;
    }

    manifest_text += header_length + 1U;
    const size_t manifest_size = strlen(manifest_text);
    if (pux_package_manifest_read_buffer((const unsigned char *)manifest_text,
                                          manifest_size, manifest, error, error_size) != 0 ||
        pux_package_manifest_validate(manifest, error, error_size) != 0) {
        free(text);
        pux_package_manifest_free(manifest);
        return -1;
    }

    char *files_text = marker + 1U + strlen(PUX_DB_FILES_MARKER) + 1U;
    char *cursor = files_text;
    while (*cursor != '\0') {
        char *line_end = strchr(cursor, '\n');
        if (line_end != NULL) {
            *line_end = '\0';
        }
        if (strlen(cursor) > 4095U) {
            free(text);
            pux_package_manifest_free(manifest);
            pux_db_file_list_free(files);
            set_error(error, error_size, "database file entry is too long");
            return -1;
        }
        if (cursor[0] != '\0') {
            const size_t length = strlen(cursor);
            const char type = cursor[0];
            if (type == 'l') {
                char *arrow = strstr(cursor + 2U, " -> " );
                if (length < 7U || arrow == NULL || arrow == cursor + 2U || arrow[4] == '\0') {
                    free(text); pux_package_manifest_free(manifest); pux_db_file_list_free(files);
                    set_error(error, error_size, "invalid symbolic link database entry");
                    return -1;
                }
                *arrow = '\0';
                if (append_file(files, type, cursor + 2U, arrow + 4U) != 0) {
                    free(text); pux_package_manifest_free(manifest); pux_db_file_list_free(files);
                    set_error(error, error_size, "invalid or duplicate database file entry");
                    return -1;
                }
            } else if (length < 3U || cursor[1] != ' ' || append_file(files, type, cursor + 2U, NULL) != 0) {
                free(text);
                pux_package_manifest_free(manifest);
                pux_db_file_list_free(files);
                set_error(error, error_size, "invalid or duplicate database file entry");
                return -1;
            }
        }
        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }

    free(text);
    if (files->count > 1U) qsort(files->items, files->count, sizeof(*files->items), compare_file_entries);
    return 0;
}

int pux_db_read_package(const char *db_root, const char *name,
                        struct pux_package_manifest *manifest,
                        struct pux_db_file_list *files,
                        char *error, size_t error_size)
{
    if (manifest == NULL || files == NULL) {
        set_error(error, error_size, "invalid database read argument");
        return -1;
    }
    pux_package_manifest_init(manifest);
    pux_db_file_list_free(files);

    if (!valid_package_name(name)) {
        set_error(error, error_size, "invalid package name");
        return -1;
    }

    char packages_dir[4096];
    if (prepare_database(db_root, packages_dir, sizeof(packages_dir), error, error_size) != 0) {
        return -1;
    }
    char record_path[4096];
    if (make_record_path(packages_dir, name, record_path, sizeof(record_path), error, error_size) != 0) {
        return -1;
    }

    int fd = open(record_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            set_errorf(error, error_size, "package is not installed: %s", name);
        } else {
            set_errorf(error, error_size, "cannot open database record: %s", strerror(errno));
        }
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 ||
        (uintmax_t)st.st_size > (uintmax_t)PUX_DB_MAX_RECORD_SIZE) {
        close(fd);
        set_error(error, error_size, "invalid or oversized database record");
        return -1;
    }
    const size_t size = (size_t)st.st_size;
    unsigned char *buffer = malloc(size == 0U ? 1U : size);
    if (buffer == NULL) {
        close(fd);
        set_error(error, error_size, "out of memory while reading database record");
        return -1;
    }
    size_t offset = 0U;
    while (offset < size) {
        const ssize_t got = read(fd, buffer + offset, size - offset);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buffer);
            close(fd);
            set_errorf(error, error_size, "cannot read database record: %s", strerror(errno));
            return -1;
        }
        if (got == 0) {
            free(buffer);
            close(fd);
            set_error(error, error_size, "truncated database record");
            return -1;
        }
        offset += (size_t)got;
    }
    close(fd);

    const int result = parse_record_buffer(buffer, size, manifest, files, error, error_size);
    free(buffer);
    return result;
}

int pux_db_unregister_package(const char *db_root, const char *name,
                             char *error, size_t error_size)
{
    if (db_root == NULL || !valid_package_name(name)) {
        set_error(error, error_size, "invalid package database removal argument");
        return -1;
    }

    char packages_dir[4096];
    if (prepare_database(db_root, packages_dir, sizeof(packages_dir), error, error_size) != 0) {
        return -1;
    }
    char record_path[4096];
    if (make_record_path(packages_dir, name, record_path, sizeof(record_path), error, error_size) != 0) {
        return -1;
    }
    if (unlink(record_path) != 0) {
        if (errno == ENOENT) {
            set_errorf(error, error_size, "package is not installed: %s", name);
        } else {
            set_errorf(error, error_size, "cannot remove database record: %s", strerror(errno));
        }
        return -1;
    }
    return 0;
}

static int record_name_from_filename(const char *filename, char *name, size_t name_size)
{
    const size_t length = strlen(filename);
    const size_t suffix_length = strlen(".record");
    if (length <= suffix_length || strcmp(filename + length - suffix_length, ".record") != 0) {
        return -1;
    }
    const size_t name_length = length - suffix_length;
    if (name_length == 0U || name_length + 1U > name_size) {
        return -1;
    }
    memcpy(name, filename, name_length);
    name[name_length] = '\0';
    return valid_package_name(name) ? 0 : -1;
}

static int compare_names(const void *left, const void *right)
{
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

int pux_db_list_packages(const char *db_root, FILE *output,
                         char *error, size_t error_size)
{
    if (db_root == NULL || output == NULL) {
        set_error(error, error_size, "invalid package database list argument");
        return -1;
    }

    char packages_dir[4096];
    if (prepare_database(db_root, packages_dir, sizeof(packages_dir), error, error_size) != 0) {
        return -1;
    }

    DIR *dir = opendir(packages_dir);
    if (dir == NULL) {
        set_errorf(error, error_size, "cannot open package database: %s", strerror(errno));
        return -1;
    }

    char **names = NULL;
    size_t count = 0U;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char name[256];
        if (record_name_from_filename(entry->d_name, name, sizeof(name)) != 0) {
            continue;
        }
        char *copy = duplicate_string(name);
        if (copy == NULL) {
            closedir(dir);
            for (size_t i = 0U; i < count; ++i) free(names[i]);
            free(names);
            set_error(error, error_size, "out of memory while listing database");
            return -1;
        }
        char **new_names = realloc(names, (count + 1U) * sizeof(*new_names));
        if (new_names == NULL) {
            free(copy);
            closedir(dir);
            for (size_t i = 0U; i < count; ++i) free(names[i]);
            free(names);
            set_error(error, error_size, "out of memory while listing database");
            return -1;
        }
        names = new_names;
        names[count++] = copy;
    }
    closedir(dir);

    if (count > 1U) {
        qsort(names, count, sizeof(*names), compare_names);
    }

    for (size_t i = 0U; i < count; ++i) {
        struct pux_package_manifest manifest;
        struct pux_db_file_list files = {0};
        if (pux_db_read_package(db_root, names[i], &manifest, &files, error, error_size) != 0) {
            for (size_t j = 0U; j < count; ++j) free(names[j]);
            free(names);
            return -1;
        }
        fprintf(output, "%s %s-%u %s\n", manifest.name, manifest.version,
                manifest.release, manifest.arch);
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
    }

    for (size_t i = 0U; i < count; ++i) free(names[i]);
    free(names);
    return 0;
}

struct pux_db_requirement_local {
    char *name;
    char *version;
    int op;
};

static int db_requirement_parse(const char *expression,
                                struct pux_db_requirement_local *requirement)
{
    memset(requirement, 0, sizeof(*requirement));
    if (expression == NULL || expression[0] == '\0') return -1;
    const char *op_pos = strpbrk(expression, "<>=");
    if (op_pos == NULL) {
        requirement->name = duplicate_string(expression);
        return requirement->name == NULL ? -1 : 0;
    }
    if (op_pos == expression) return -1;
    requirement->name = duplicate_range(expression, (size_t)(op_pos - expression));
    if (requirement->name == NULL) return -1;
    size_t op_len = ((op_pos[0] == '<' || op_pos[0] == '>') && op_pos[1] == '=') ? 2U : 1U;
    switch (op_pos[0]) {
        case '=': requirement->op = 1; break;
        case '<': requirement->op = op_len == 2U ? 2 : 3; break;
        case '>': requirement->op = op_len == 2U ? 4 : 5; break;
        default: requirement->op = 0; break;
    }
    if (op_pos[op_len] == '\0') {
        free(requirement->name);
        requirement->name = NULL;
        return -1;
    }
    requirement->version = duplicate_string(op_pos + op_len);
    if (requirement->version == NULL) {
        free(requirement->name);
        requirement->name = NULL;
        return -1;
    }
    return 0;
}

static void db_requirement_free(struct pux_db_requirement_local *requirement)
{
    free(requirement->name);
    free(requirement->version);
    memset(requirement, 0, sizeof(*requirement));
}

static int db_next_version_part(const char **cursor, char *buffer, size_t buffer_size, int *numeric)
{
    const char *p = *cursor;
    while (*p != '\0' && !isalnum((unsigned char)*p)) ++p;
    if (*p == '\0') {
        *cursor = p;
        return 0;
    }
    const char *start = p;
    *numeric = isdigit((unsigned char)*p) != 0;
    while (*p != '\0' && isalnum((unsigned char)*p)) {
        if ((isdigit((unsigned char)*p) != 0) != (*numeric != 0)) break;
        ++p;
    }
    const size_t length = (size_t)(p - start);
    if (length == 0U || length + 1U > buffer_size) return -1;
    memcpy(buffer, start, length);
    buffer[length] = '\0';
    *cursor = p;
    return 1;
}

static int db_compare_version(const char *left, const char *right)
{
    const char *l = left;
    const char *r = right;
    for (;;) {
        char lp[64], rp[64];
        int ln = 0, rn = 0;
        const int lm = db_next_version_part(&l, lp, sizeof(lp), &ln);
        const int rm = db_next_version_part(&r, rp, sizeof(rp), &rn);
        if (lm < 0 || rm < 0) return strcmp(left, right);
        if (lm == 0 && rm == 0) return 0;
        if (lm == 0) return -1;
        if (rm == 0) return 1;
        if (ln != 0 && rn != 0) {
            size_t lo = 0U, ro = 0U;
            while (lp[lo] == '0' && lp[lo + 1U] != '\0') ++lo;
            while (rp[ro] == '0' && rp[ro + 1U] != '\0') ++ro;
            const size_t ll = strlen(lp + lo), rl = strlen(rp + ro);
            if (ll != rl) return ll < rl ? -1 : 1;
            const int c = strcmp(lp + lo, rp + ro);
            if (c != 0) return c < 0 ? -1 : 1;
        } else if (ln != rn) {
            return ln != 0 ? 1 : -1;
        } else {
            const int c = strcmp(lp, rp);
            if (c != 0) return c < 0 ? -1 : 1;
        }
    }
}

static int db_requirement_matches(const struct pux_package_manifest *manifest,
                                   const struct pux_db_requirement_local *requirement)
{
    if (strcmp(manifest->name, requirement->name) == 0) {
        if (requirement->op == 0) return 1;
        const int cmp = db_compare_version(manifest->version, requirement->version);
        switch (requirement->op) {
            case 1: return cmp == 0;
            case 2: return cmp <= 0;
            case 3: return cmp < 0;
            case 4: return cmp >= 0;
            case 5: return cmp > 0;
            default: return 0;
        }
    }
    if (requirement->op != 0) return 0;
    for (size_t i = 0U; i < manifest->provides.count; ++i) {
        if (strcmp(manifest->provides.items[i], requirement->name) == 0) return 1;
    }
    return 0;
}

static int db_packages_dir_path(const char *db_root, char *path, size_t size)
{
    const size_t root_len = strlen(db_root);
    const int separator = root_len != 0U && db_root[root_len - 1U] != '/';
    const size_t total = root_len + (size_t)separator + strlen("packages") + 1U;
    if (total > size) return -1;
    size_t offset = root_len;
    memcpy(path, db_root, root_len);
    if (separator != 0) path[offset++] = '/';
    memcpy(path + offset, "packages", strlen("packages") + 1U);
    return 0;
}

int pux_db_dependency_satisfied(const char *db_root, const char *expression,
                                int *satisfied, char *error, size_t error_size)
{
    if (db_root == NULL || expression == NULL || satisfied == NULL) {
        set_error(error, error_size, "invalid dependency database argument");
        return -1;
    }
    *satisfied = 0;
    struct pux_db_requirement_local requirement;
    if (db_requirement_parse(expression, &requirement) != 0) {
        set_error(error, error_size, "invalid dependency expression");
        return -1;
    }

    char packages_dir[4096];
    if (db_packages_dir_path(db_root, packages_dir, sizeof(packages_dir)) != 0) {
        db_requirement_free(&requirement);
        set_error(error, error_size, "package database path is too long");
        return -1;
    }
    DIR *dir = opendir(packages_dir);
    if (dir == NULL) {
        if (errno == ENOENT) {
            db_requirement_free(&requirement);
            return 0;
        }
        set_errorf(error, error_size, "cannot open package database: %s", strerror(errno));
        db_requirement_free(&requirement);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && *satisfied == 0) {
        char name[256];
        if (record_name_from_filename(entry->d_name, name, sizeof(name)) != 0) continue;
        struct pux_package_manifest manifest;
        struct pux_db_file_list files = {0};
        char local_error[512] = {0};
        if (pux_db_read_package(db_root, name, &manifest, &files,
                                local_error, sizeof(local_error)) != 0) {
            closedir(dir);
            db_requirement_free(&requirement);
            set_errorf(error, error_size, "cannot read package database record: %s", name);
            return -1;
        }
        if (db_requirement_matches(&manifest, &requirement)) *satisfied = 1;
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
    }
    closedir(dir);
    db_requirement_free(&requirement);
    return 0;
}



int pux_db_find_reverse_dependency(const char *db_root,
                                   const struct pux_package_manifest *target,
                                   char *dependent, size_t dependent_size,
                                   int *found, char *error, size_t error_size)
{
    if (db_root == NULL || target == NULL || dependent == NULL || dependent_size == 0U || found == NULL) {
        set_error(error, error_size, "invalid reverse dependency query argument");
        return -1;
    }
    *found = 0;
    dependent[0] = '\0';

    char packages_dir[4096];
    if (db_packages_dir_path(db_root, packages_dir, sizeof(packages_dir)) != 0) {
        set_error(error, error_size, "package database path is too long");
        return -1;
    }
    DIR *dir = opendir(packages_dir);
    if (dir == NULL) {
        if (errno == ENOENT) return 0;
        set_errorf(error, error_size, "cannot open package database: %s", strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && *found == 0) {
        char name[256];
        if (record_name_from_filename(entry->d_name, name, sizeof(name)) != 0) continue;
        if (strcmp(name, target->name) == 0) continue;

        struct pux_package_manifest manifest;
        struct pux_db_file_list files = {0};
        char local_error[512] = {0};
        if (pux_db_read_package(db_root, name, &manifest, &files,
                                local_error, sizeof(local_error)) != 0) {
            closedir(dir);
            set_errorf(error, error_size, "cannot read package database record: %s", name);
            return -1;
        }

        for (size_t i = 0U; i < manifest.depends.count && *found == 0; ++i) {
            struct pux_db_requirement_local requirement;
            if (db_requirement_parse(manifest.depends.items[i], &requirement) != 0) {
                pux_package_manifest_free(&manifest);
                pux_db_file_list_free(&files);
                closedir(dir);
                set_errorf(error, error_size, "invalid dependency in installed package: %s", name);
                return -1;
            }
            const int target_matches = db_requirement_matches(target, &requirement);
            db_requirement_free(&requirement);
            if (target_matches == 0) continue;

            int satisfied_without_target = 0;
            struct pux_db_requirement_local req2;
            if (db_requirement_parse(manifest.depends.items[i], &req2) != 0) {
                pux_package_manifest_free(&manifest);
                pux_db_file_list_free(&files);
                closedir(dir);
                set_error(error, error_size, "invalid dependency expression");
                return -1;
            }

            DIR *other_dir = opendir(packages_dir);
            if (other_dir == NULL) {
                db_requirement_free(&req2);
                pux_package_manifest_free(&manifest);
                pux_db_file_list_free(&files);
                closedir(dir);
                set_errorf(error, error_size, "cannot open package database: %s", strerror(errno));
                return -1;
            }
            struct dirent *other_entry;
            while ((other_entry = readdir(other_dir)) != NULL && satisfied_without_target == 0) {
                char other_name[256];
                if (record_name_from_filename(other_entry->d_name, other_name, sizeof(other_name)) != 0) continue;
                if (strcmp(other_name, target->name) == 0) continue;
                struct pux_package_manifest other_manifest;
                struct pux_db_file_list other_files = {0};
                char other_error[512] = {0};
                if (pux_db_read_package(db_root, other_name, &other_manifest, &other_files,
                                        other_error, sizeof(other_error)) != 0) {
                    closedir(other_dir);
                    db_requirement_free(&req2);
                    pux_package_manifest_free(&manifest);
                    pux_db_file_list_free(&files);
                    closedir(dir);
                    set_errorf(error, error_size, "cannot read package database record: %s", other_name);
                    return -1;
                }
                if (db_requirement_matches(&other_manifest, &req2)) satisfied_without_target = 1;
                pux_package_manifest_free(&other_manifest);
                pux_db_file_list_free(&other_files);
            }
            closedir(other_dir);
            db_requirement_free(&req2);

            if (satisfied_without_target == 0) {
                if (strlen(name) + 1U > dependent_size) {
                    pux_package_manifest_free(&manifest);
                    pux_db_file_list_free(&files);
                    closedir(dir);
                    set_error(error, error_size, "dependent package name is too long");
                    return -1;
                }
                memcpy(dependent, name, strlen(name) + 1U);
                *found = 1;
            }
        }

        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
    }

    closedir(dir);
    return 0;
}
static int db_find_owner_excluding(const char *db_root, const char *path,
                                   const char *excluded_name,
                                   char *owner, size_t owner_size, int *owned,
                                   char *error, size_t error_size)
{
    if (db_root == NULL || path == NULL || owner == NULL || owner_size == 0U || owned == NULL) {
        set_error(error, error_size, "invalid package ownership query argument");
        return -1;
    }
    *owned = 0;
    owner[0] = '\0';

    char packages_dir[4096];
    if (db_packages_dir_path(db_root, packages_dir, sizeof(packages_dir)) != 0) {
        set_error(error, error_size, "package database path is too long");
        return -1;
    }
    DIR *dir = opendir(packages_dir);
    if (dir == NULL) {
        if (errno == ENOENT) return 0;
        set_errorf(error, error_size, "cannot open package database: %s", strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && *owned == 0) {
        char name[256];
        if (record_name_from_filename(entry->d_name, name, sizeof(name)) != 0) continue;
        if (excluded_name != NULL && strcmp(name, excluded_name) == 0) continue;
        struct pux_package_manifest manifest;
        struct pux_db_file_list files = {0};
        char local_error[512] = {0};
        if (pux_db_read_package(db_root, name, &manifest, &files,
                                local_error, sizeof(local_error)) != 0) {
            closedir(dir);
            set_errorf(error, error_size, "cannot read package database record: %s", name);
            return -1;
        }
        for (size_t i = 0U; i < files.count; ++i) {
            if (strcmp(files.items[i].path, path) == 0) {
                if (strlen(name) + 1U > owner_size) {
                    pux_package_manifest_free(&manifest);
                    pux_db_file_list_free(&files);
                    closedir(dir);
                    set_error(error, error_size, "package owner name is too long");
                    return -1;
                }
                memcpy(owner, name, strlen(name) + 1U);
                *owned = 1;
                break;
            }
        }
        pux_package_manifest_free(&manifest);
        pux_db_file_list_free(&files);
    }
    closedir(dir);
    return 0;
}

int pux_db_find_owner(const char *db_root, const char *path,
                      char *owner, size_t owner_size, int *owned,
                      char *error, size_t error_size)
{
    return db_find_owner_excluding(db_root, path, NULL, owner, owner_size,
                                   owned, error, error_size);
}

int pux_db_find_other_owner(const char *db_root, const char *path,
                            const char *excluded_name,
                            char *owner, size_t owner_size, int *owned,
                            char *error, size_t error_size)
{
    return db_find_owner_excluding(db_root, path, excluded_name, owner, owner_size,
                                   owned, error, error_size);
}
