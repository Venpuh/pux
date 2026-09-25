#define _POSIX_C_SOURCE 200809L
#include "pux/config.h"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0U) (void)snprintf(error, error_size, "%s", message);
}

static void set_errorf(char *error, size_t error_size, const char *fmt, const char *value)
{
    if (error != NULL && error_size > 0U) (void)snprintf(error, error_size, fmt, value);
}

static char *duplicate_string(const char *value)
{
    const size_t length = strlen(value);
    char *copy = malloc(length + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, value, length + 1U);
    return copy;
}

static int join_path(const char *base, const char *name, char *output, size_t output_size)
{
    const size_t base_len = strlen(base);
    const size_t name_len = strlen(name);
    const int separator = base_len > 0U && base[base_len - 1U] != '/';
    if (base_len > SIZE_MAX - name_len - (separator ? 1U : 0U) - 1U) return -1;
    const size_t total = base_len + name_len + (separator ? 1U : 0U);
    if (total + 1U > output_size) return -1;
    memcpy(output, base, base_len);
    size_t offset = base_len;
    if (separator != 0) output[offset++] = '/';
    memcpy(output + offset, name, name_len + 1U);
    return 0;
}

static int make_directory(const char *path, mode_t mode, char *error, size_t error_size)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            set_error(error, error_size, "repository configuration path is not a directory");
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT) {
        set_errorf(error, error_size, "cannot inspect repository configuration path: %s", strerror(errno));
        return -1;
    }
    char parent[PATH_MAX];
    if (snprintf(parent, sizeof(parent), "%s", path) < 0) return -1;
    char *slash = strrchr(parent, '/');
    if (slash != NULL && slash != parent) {
        *slash = '\0';
        if (make_directory(parent, mode, error, error_size) != 0) return -1;
    }
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        set_errorf(error, error_size, "cannot create repository configuration path: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void entry_free(struct pux_repo_config_entry *entry)
{
    if (entry == NULL) return;
    free(entry->name);
    free(entry->url);
    free(entry->cache_dir);
    memset(entry, 0, sizeof(*entry));
}

void pux_repo_config_list_free(struct pux_repo_config_list *list)
{
    if (list == NULL) return;
    for (size_t i = 0U; i < list->count; ++i) entry_free(&list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

const char *pux_repo_config_root(void)
{
    const char *value = getenv("PUX_REPO_CONFIG_ROOT");
    return value != NULL && value[0] != '\0' ? value : PUX_CONFIG_DEFAULT_ROOT;
}

const char *pux_repo_cache_root(void)
{
    const char *value = getenv("PUX_REPO_CACHE_ROOT");
    return value != NULL && value[0] != '\0' ? value : PUX_CACHE_DEFAULT_ROOT;
}

int pux_repo_config_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= PUX_CONFIG_MAX_NAME) return 0;
    if (name[0] == '.' || name[strlen(name) - 1U] == '.') return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; ++p) {
        if (!(isalnum(*p) != 0 || *p == '-' || *p == '_' || *p == '.')) return 0;
    }
    return 1;
}

static int parse_bool(const char *value, int *result)
{
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "on") == 0) {
        *result = 1;
        return 0;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0 || strcmp(value, "off") == 0) {
        *result = 0;
        return 0;
    }
    return -1;
}

static char *trim(char *value)
{
    while (isspace((unsigned char)*value) != 0) ++value;
    char *end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1]) != 0) --end;
    *end = '\0';
    return value;
}

static int read_entry_file(const char *config_root,
                           const char *cache_root,
                           const char *name,
                           struct pux_repo_config_entry *entry,
                           char *error,
                           size_t error_size)
{
    char filename[PATH_MAX];
    if (join_path(config_root, name, filename, sizeof(filename)) != 0) {
        set_error(error, error_size, "repository configuration path is too long");
        return -1;
    }

    const size_t filename_len = strlen(filename);
    if (filename_len + 6U > sizeof(filename)) {
        set_error(error, error_size, "repository configuration path is too long");
        return -1;
    }
    memcpy(filename + filename_len, ".conf", 6U);
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        set_errorf(error, error_size, "cannot open repository configuration: %s", strerror(errno));
        return -1;
    }

    memset(entry, 0, sizeof(*entry));
    entry->name = duplicate_string(name);
    if (entry->name == NULL) goto oom;
    entry->priority = 100;
    entry->enabled = 1;
    entry->require_signature = 0;

    char line[4096];
    unsigned seen = 0U;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *value = trim(line);
        if (*value == '\0' || *value == '#') continue;
        char *equals = strchr(value, '=');
        if (equals == NULL) {
            set_error(error, error_size, "invalid repository configuration line");
            fclose(file);
            entry_free(entry);
            return -1;
        }
        *equals++ = '\0';
        char *key = trim(value);
        value = trim(equals);
        if (*key == '\0' || *value == '\0') {
            set_error(error, error_size, "repository configuration key/value is empty");
            fclose(file);
            entry_free(entry);
            return -1;
        }
        if (strcmp(key, "url") == 0) {
            if ((seen & 1U) != 0U || strlen(value) >= PUX_CONFIG_MAX_URL) {
                set_error(error, error_size, "invalid repository URL setting");
                fclose(file); entry_free(entry); return -1;
            }
            entry->url = duplicate_string(value);
            if (entry->url == NULL) goto oom_file;
            seen |= 1U;
        } else if (strcmp(key, "priority") == 0) {
            if ((seen & 2U) != 0U) { set_error(error, error_size, "duplicate repository priority"); fclose(file); entry_free(entry); return -1; }
            char *end = NULL;
            errno = 0;
            long priority = strtol(value, &end, 10);
            if (errno != 0 || end == value || *end != '\0' || priority < 0L || priority > PUX_CONFIG_MAX_PRIORITY) {
                set_error(error, error_size, "invalid repository priority"); fclose(file); entry_free(entry); return -1;
            }
            entry->priority = (int)priority;
            seen |= 2U;
        } else if (strcmp(key, "enabled") == 0) {
            if ((seen & 4U) != 0U || parse_bool(value, &entry->enabled) != 0) { set_error(error, error_size, "invalid repository enabled setting"); fclose(file); entry_free(entry); return -1; }
            seen |= 4U;
        } else if (strcmp(key, "require-signature") == 0) {
            if ((seen & 8U) != 0U || parse_bool(value, &entry->require_signature) != 0) { set_error(error, error_size, "invalid repository signature policy"); fclose(file); entry_free(entry); return -1; }
            seen |= 8U;
        } else {
            set_error(error, error_size, "unknown repository configuration setting"); fclose(file); entry_free(entry); return -1;
        }
    }
    if (ferror(file) != 0 || fclose(file) != 0) {
        set_error(error, error_size, "cannot read repository configuration");
        entry_free(entry);
        return -1;
    }
    if (entry->url == NULL) {
        set_error(error, error_size, "repository configuration has no url");
        entry_free(entry);
        return -1;
    }
    if (strncmp(entry->url, "http://", 7U) != 0 && strncmp(entry->url, "https://", 8U) != 0) {
        set_error(error, error_size, "repository URL must use http:// or https://");
        entry_free(entry);
        return -1;
    }
    if (!entry->enabled && entry->require_signature != 0) {
        /* This is permitted; disabled repositories are simply skipped. */
    }
    char cache_name[PATH_MAX];
    if (join_path(cache_root, name, cache_name, sizeof(cache_name)) != 0) {
        set_error(error, error_size, "repository cache path is too long");
        entry_free(entry);
        return -1;
    }
    entry->cache_dir = duplicate_string(cache_name);
    if (entry->cache_dir == NULL) goto oom;
    return 0;

oom_file:
    fclose(file);
oom:
    entry_free(entry);
    set_error(error, error_size, "out of memory while reading repository configuration");
    return -1;
}

static int entry_compare(const void *left, const void *right)
{
    const struct pux_repo_config_entry *a = left;
    const struct pux_repo_config_entry *b = right;
    if (a->priority != b->priority) return a->priority > b->priority ? -1 : 1;
    return strcmp(a->name, b->name);
}

static int append_entry(struct pux_repo_config_list *list,
                        struct pux_repo_config_entry *entry,
                        char *error,
                        size_t error_size)
{
    if (list->count >= PUX_CONFIG_MAX_REPOSITORIES) {
        set_error(error, error_size, "too many configured repositories");
        return -1;
    }
    for (size_t i = 0U; i < list->count; ++i) {
        if (strcmp(list->items[i].name, entry->name) == 0) {
            set_error(error, error_size, "duplicate repository configuration name");
            return -1;
        }
    }
    struct pux_repo_config_entry *items = realloc(list->items, (list->count + 1U) * sizeof(*items));
    if (items == NULL) {
        set_error(error, error_size, "out of memory while loading repository configurations");
        return -1;
    }
    list->items = items;
    list->items[list->count++] = *entry;
    memset(entry, 0, sizeof(*entry));
    return 0;
}

int pux_repo_config_load_all(const char *config_root,
                             const char *cache_root,
                             struct pux_repo_config_list *list,
                             char *error,
                             size_t error_size)
{
    if (config_root == NULL || cache_root == NULL || list == NULL) {
        set_error(error, error_size, "invalid repository configuration arguments");
        return -1;
    }
    memset(list, 0, sizeof(*list));
    struct stat st;
    if (stat(config_root, &st) != 0) {
        if (errno == ENOENT) return 0;
        set_errorf(error, error_size, "cannot inspect repository configuration directory: %s", strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        set_error(error, error_size, "repository configuration root is not a directory");
        return -1;
    }

    /* Directory enumeration deliberately uses the shell-independent POSIX API. */
    DIR *dir = opendir(config_root);
    if (dir == NULL) {
        set_errorf(error, error_size, "cannot open repository configuration directory: %s", strerror(errno));
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *filename = entry->d_name;
        const size_t length = strlen(filename);
        if (length <= 5U || strcmp(filename + length - 5U, ".conf") != 0) continue;
        char name[PUX_CONFIG_MAX_NAME];
        const size_t name_len = length - 5U;
        if (name_len == 0U || name_len >= sizeof(name)) {
            set_error(error, error_size, "repository configuration name is too long");
            closedir(dir); pux_repo_config_list_free(list); return -1;
        }
        memcpy(name, filename, name_len);
        name[name_len] = '\0';
        if (!pux_repo_config_name_valid(name)) {
            set_error(error, error_size, "invalid repository configuration name");
            closedir(dir); pux_repo_config_list_free(list); return -1;
        }
        struct pux_repo_config_entry item;
        memset(&item, 0, sizeof(item));
        if (read_entry_file(config_root, cache_root, name, &item, error, error_size) != 0) {
            closedir(dir); pux_repo_config_list_free(list); return -1;
        }
        if (append_entry(list, &item, error, error_size) != 0) {
            entry_free(&item); closedir(dir); pux_repo_config_list_free(list); return -1;
        }
    }
    if (closedir(dir) != 0) {
        set_error(error, error_size, "cannot close repository configuration directory");
        pux_repo_config_list_free(list);
        return -1;
    }
    if (list->count > 1U) qsort(list->items, list->count, sizeof(list->items[0]), entry_compare);
    return 0;
}

static int write_entry_file(const char *config_root,
                            const char *name,
                            const char *url,
                            int priority,
                            int enabled,
                            int require_signature,
                            char *error,
                            size_t error_size)
{
    char path[PATH_MAX];
    char temp[PATH_MAX];
    if (join_path(config_root, name, path, sizeof(path)) != 0) {
        set_error(error, error_size, "repository configuration path is too long");
        return -1;
    }
    const size_t path_len = strlen(path);
    if (path_len + 6U > sizeof(path)) {
        set_error(error, error_size, "repository configuration path is too long");
        return -1;
    }
    memcpy(path + path_len, ".conf", 6U);
    if (path_len + 5U + 1U > sizeof(temp)) {
        set_error(error, error_size, "repository configuration path is too long");
        return -1;
    }
    memcpy(temp, path, path_len);
    memcpy(temp + path_len, ".tmp", 5U);
    FILE *file = fopen(temp, "wb");
    if (file == NULL) {
        set_errorf(error, error_size, "cannot create repository configuration: %s", strerror(errno));
        return -1;
    }
    const int ok = fprintf(file,
                           "url=%s\npriority=%d\nenabled=%d\nrequire-signature=%d\n",
                           url, priority, enabled, require_signature) >= 0 &&
                   fflush(file) == 0 && fsync(fileno(file)) == 0;
    const int close_result = fclose(file);
    if (!ok || close_result != 0) {
        unlink(temp);
        set_error(error, error_size, "cannot write repository configuration");
        return -1;
    }
    if (chmod(temp, 0644U) != 0 || rename(temp, path) != 0) {
        const int saved_errno = errno;
        unlink(temp);
        set_errorf(error, error_size, "cannot install repository configuration: %s", strerror(saved_errno));
        return -1;
    }
    return 0;
}

int pux_repo_config_add(const char *config_root,
                        const char *cache_root,
                        const char *name,
                        const char *url,
                        int priority,
                        int enabled,
                        int require_signature,
                        char *error,
                        size_t error_size)
{
    (void)cache_root;
    if (!pux_repo_config_name_valid(name)) {
        set_error(error, error_size, "invalid repository name");
        return -1;
    }
    if (url == NULL || (strncmp(url, "http://", 7U) != 0 && strncmp(url, "https://", 8U) != 0)) {
        set_error(error, error_size, "repository URL must use http:// or https://");
        return -1;
    }
    if (priority < 0 || priority > PUX_CONFIG_MAX_PRIORITY) {
        set_error(error, error_size, "invalid repository priority");
        return -1;
    }
    if (make_directory(config_root, 0755U, error, error_size) != 0) return -1;
    return write_entry_file(config_root, name, url, priority, enabled != 0, require_signature != 0, error, error_size);
}

int pux_repo_config_remove(const char *config_root,
                           const char *name,
                           char *error,
                           size_t error_size)
{
    if (!pux_repo_config_name_valid(name)) {
        set_error(error, error_size, "invalid repository name");
        return -1;
    }
    char path[PATH_MAX];
    if (join_path(config_root, name, path, sizeof(path)) != 0) {
        set_error(error, error_size, "repository configuration path is too long");
        return -1;
    }
    const size_t path_len = strlen(path);
    if (path_len + 6U > sizeof(path)) {
        set_error(error, error_size, "repository configuration path is too long");
        return -1;
    }
    memcpy(path + path_len, ".conf", 6U);
    if (unlink(path) != 0) {
        if (errno == ENOENT) {
            set_error(error, error_size, "repository is not configured");
        } else {
            set_errorf(error, error_size, "cannot remove repository configuration: %s", strerror(errno));
        }
        return -1;
    }
    return 0;
}

int pux_repo_config_print(const struct pux_repo_config_list *list, FILE *output)
{
    if (list == NULL || output == NULL) return -1;
    if (list->count == 0U) {
        fputs("No repositories configured.\n", output);
        return ferror(output) == 0 ? 0 : -1;
    }
    for (size_t i = 0U; i < list->count; ++i) {
        const struct pux_repo_config_entry *item = &list->items[i];
        if (fprintf(output, "%s priority=%d enabled=%d signed=%d url=%s cache=%s\n",
                    item->name, item->priority, item->enabled,
                    item->require_signature, item->url, item->cache_dir) < 0) return -1;
    }
    return 0;
}
