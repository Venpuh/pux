#define _POSIX_C_SOURCE 200809L

#include "pux/repo.h"
#include "pux/package.h"
#include "pux/container.h"
#include "pux/sha256.h"

#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static void set_errorf(char *error, size_t error_size, const char *fmt, const char *value)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, fmt, value);
    }
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
    const size_t base_length = strlen(base);
    const size_t name_length = strlen(name);
    const int needs_separator = base_length > 0U && base[base_length - 1U] != '/';
    if (base_length > SIZE_MAX - name_length - (needs_separator ? 1U : 0U) - 1U) return -1;
    const size_t total = base_length + name_length + (needs_separator ? 1U : 0U);
    if (total + 1U > output_size) return -1;
    memcpy(output, base, base_length);
    size_t offset = base_length;
    if (needs_separator) output[offset++] = '/';
    memcpy(output + offset, name, name_length + 1U);
    return 0;
}

static int has_suffix(const char *value, const char *suffix)
{
    const size_t value_length = strlen(value);
    const size_t suffix_length = strlen(suffix);
    return value_length >= suffix_length &&
           strcmp(value + value_length - suffix_length, suffix) == 0;
}

static int is_package_filename(const char *name)
{
    return name != NULL && name[0] != '\0' &&
           strcmp(name, PUX_REPO_INDEX_NAME) != 0 &&
           strlen(name) < PUX_REPO_MAX_FILENAME && has_suffix(name, ".pux") &&
           strchr(name, '/') == NULL;
}

static int manifest_clone(const struct pux_package_manifest *source,
                          struct pux_package_manifest *destination)
{
    pux_package_manifest_init(destination);
    FILE *tmp = tmpfile();
    if (tmp == NULL) return -1;
    if (pux_package_manifest_write_stream(source, tmp) != 0 || fflush(tmp) != 0 ||
        fseek(tmp, 0L, SEEK_END) != 0) {
        fclose(tmp);
        return -1;
    }
    const long end = ftell(tmp);
    if (end <= 0L || (unsigned long)end > (unsigned long)PUX_PACKAGE_MAX_MANIFEST_SIZE ||
        fseek(tmp, 0L, SEEK_SET) != 0) {
        fclose(tmp);
        return -1;
    }
    const size_t size = (size_t)end;
    unsigned char *buffer = malloc(size);
    if (buffer == NULL) {
        fclose(tmp);
        return -1;
    }
    const size_t read_size = fread(buffer, 1U, size, tmp);
    const int failed = ferror(tmp) != 0;
    fclose(tmp);
    if (failed || read_size != size) {
        free(buffer);
        return -1;
    }
    char parse_error[256] = {0};
    const int result = pux_package_manifest_read_buffer(buffer, size, destination,
                                                         parse_error, sizeof(parse_error));
    free(buffer);
    return result;
}

static void package_free(struct pux_repo_package *package)
{
    if (package == NULL) return;
    free(package->filename);
    package->filename = NULL;
    pux_package_manifest_free(&package->manifest);
    package->size = 0U;
}

void pux_repo_catalog_free(struct pux_repo_catalog *catalog)
{
    if (catalog == NULL) return;
    for (size_t i = 0U; i < catalog->count; ++i) package_free(&catalog->packages[i]);
    free(catalog->packages);
    catalog->packages = NULL;
    catalog->count = 0U;
}

static int compare_version(const char *left, const char *right)
{
    /* This comparison deliberately follows the same initial policy as resolver.c. */
    const char *l = left;
    const char *r = right;
    for (;;) {
        char lp[128];
        char rp[128];
        int ln = 0;
        int rn = 0;
        const char *ls = l;
        const char *rs = r;
        while (*ls != '\0' && !isalnum((unsigned char)*ls)) ++ls;
        while (*rs != '\0' && !isalnum((unsigned char)*rs)) ++rs;
        if (*ls == '\0' && *rs == '\0') return 0;
        if (*ls == '\0') return -1;
        if (*rs == '\0') return 1;
        ln = isdigit((unsigned char)*ls) != 0;
        rn = isdigit((unsigned char)*rs) != 0;
        const char *lend = ls;
        const char *rend = rs;
        while (*lend != '\0' && isalnum((unsigned char)*lend) &&
               (isdigit((unsigned char)*lend) != 0) == (ln != 0)) ++lend;
        while (*rend != '\0' && isalnum((unsigned char)*rend) &&
               (isdigit((unsigned char)*rend) != 0) == (rn != 0)) ++rend;
        const size_t llen = (size_t)(lend - ls);
        const size_t rlen = (size_t)(rend - rs);
        if (llen == 0U || rlen == 0U || llen + 1U > sizeof(lp) || rlen + 1U > sizeof(rp)) {
            return strcmp(left, right);
        }
        memcpy(lp, ls, llen); lp[llen] = '\0';
        memcpy(rp, rs, rlen); rp[rlen] = '\0';
        if (ln != 0 && rn != 0) {
            size_t lo = 0U, ro = 0U;
            while (lo + 1U < llen && lp[lo] == '0') ++lo;
            while (ro + 1U < rlen && rp[ro] == '0') ++ro;
            const size_t lnum = llen - lo;
            const size_t rnum = rlen - ro;
            if (lnum != rnum) return lnum < rnum ? -1 : 1;
            const int cmp = strcmp(lp + lo, rp + ro);
            if (cmp != 0) return cmp < 0 ? -1 : 1;
        } else if (ln != rn) {
            return ln != 0 ? 1 : -1;
        } else {
            const int cmp = strcmp(lp, rp);
            if (cmp != 0) return cmp < 0 ? -1 : 1;
        }
        l = lend;
        r = rend;
    }
}

static int package_compare_for_index(const void *left_ptr, const void *right_ptr)
{
    const struct pux_repo_package *left = left_ptr;
    const struct pux_repo_package *right = right_ptr;
    int cmp = strcmp(left->manifest.name, right->manifest.name);
    if (cmp != 0) return cmp;
    cmp = compare_version(left->manifest.version, right->manifest.version);
    if (cmp != 0) return cmp;
    if (left->manifest.release != right->manifest.release) {
        return left->manifest.release < right->manifest.release ? -1 : 1;
    }
    cmp = strcmp(left->manifest.arch, right->manifest.arch);
    if (cmp != 0) return cmp;
    return strcmp(left->filename, right->filename);
}

static int append_package(struct pux_repo_catalog *catalog,
                          const char *filename,
                          size_t size,
                          const char *sha256,
                          const struct pux_package_manifest *manifest)
{
    if (catalog->count >= PUX_REPO_MAX_PACKAGES) return -1;
    struct pux_repo_package *packages = realloc(catalog->packages,
                                                 (catalog->count + 1U) * sizeof(*packages));
    if (packages == NULL) return -1;
    catalog->packages = packages;
    struct pux_repo_package *package = &catalog->packages[catalog->count];
    memset(package, 0, sizeof(*package));
    package->filename = duplicate_string(filename);
    if (package->filename == NULL || manifest_clone(manifest, &package->manifest) != 0) {
        package_free(package);
        return -1;
    }
    package->size = size;
    memcpy(package->sha256, sha256, PUX_SHA256_HEX_SIZE);
    catalog->count++;
    return 0;
}

static int scan_directory(const char *repository_dir,
                          struct pux_repo_catalog *catalog,
                          char *error, size_t error_size)
{
    struct stat st;
    if (stat(repository_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_errorf(error, error_size, "repository is not a directory: %s", repository_dir);
        return -1;
    }
    DIR *dir = opendir(repository_dir);
    if (dir == NULL) {
        set_errorf(error, error_size, "cannot open repository: %s", strerror(errno));
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_package_filename(entry->d_name)) continue;
        char path[PATH_MAX];
        if (join_path(repository_dir, entry->d_name, path, sizeof(path)) != 0) {
            closedir(dir);
            set_error(error, error_size, "repository package path is too long");
            return -1;
        }
        struct stat item_st;
        if (stat(path, &item_st) != 0) {
            closedir(dir);
            set_errorf(error, error_size, "cannot stat package: %s", strerror(errno));
            return -1;
        }
        if (!S_ISREG(item_st.st_mode)) continue;
        struct pux_package_manifest manifest;
        char package_error[512] = {0};
        if (pux_package_archive_validate(path, &manifest, package_error, sizeof(package_error)) != 0) {
            set_errorf(error, error_size, "invalid repository package: %s", entry->d_name);
            closedir(dir);
            return -1;
        }
        char sha256[PUX_SHA256_HEX_SIZE];
        if (pux_sha256_file(path, sha256, package_error, sizeof(package_error)) != 0) {
            pux_package_manifest_free(&manifest);
            closedir(dir);
            set_errorf(error, error_size, "cannot hash repository package: %s", entry->d_name);
            return -1;
        }
        if (append_package(catalog, entry->d_name, (size_t)item_st.st_size, sha256, &manifest) != 0) {
            pux_package_manifest_free(&manifest);
            closedir(dir);
            set_error(error, error_size, "out of memory while indexing repository");
            return -1;
        }
        pux_package_manifest_free(&manifest);
    }
    if (closedir(dir) != 0) {
        set_error(error, error_size, "cannot close repository");
        return -1;
    }
    if (catalog->count == 0U) {
        set_error(error, error_size, "repository contains no .pux packages");
        return -1;
    }
    return 0;
}

static int write_index(const char *repository_dir,
                       const struct pux_repo_catalog *catalog,
                       char *error, size_t error_size)
{
    char path[PATH_MAX];
    char temp[PATH_MAX];
    if (join_path(repository_dir, PUX_REPO_INDEX_NAME, path, sizeof(path)) != 0 ||
        join_path(repository_dir, ".index.pux.tmp", temp, sizeof(temp)) != 0) {
        set_error(error, error_size, "repository index path is too long");
        return -1;
    }

    FILE *file = fopen(temp, "wb");
    if (file == NULL) {
        set_errorf(error, error_size, "cannot create repository index: %s", strerror(errno));
        return -1;
    }
    if (fprintf(file, "# pux-index=%u\n\n", PUX_REPO_INDEX_FORMAT) < 0) goto fail;
    for (size_t i = 0U; i < catalog->count; ++i) {
        const struct pux_repo_package *package = &catalog->packages[i];
        if (fprintf(file, "package=%s\nsize=%zu\nsha256=%s\n", package->filename, package->size, package->sha256) < 0 ||
            pux_package_manifest_write_stream(&package->manifest, file) != 0 ||
            fputc('\n', file) == EOF) goto fail;
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0) {
        file = NULL;
        goto fail_path;
    }
    if (rename(temp, path) != 0) {
        set_errorf(error, error_size, "cannot install repository index: %s", strerror(errno));
        unlink(temp);
        return -1;
    }
    return 0;

fail:
    (void)fclose(file);
fail_path:
    unlink(temp);
    set_error(error, error_size, "cannot write repository index");
    return -1;
}

int pux_repo_create_index(const char *repository_dir,
                          char *error, size_t error_size)
{
    struct pux_repo_catalog catalog = {0};
    if (scan_directory(repository_dir, &catalog, error, error_size) != 0) {
        pux_repo_catalog_free(&catalog);
        return -1;
    }
    qsort(catalog.packages, catalog.count, sizeof(catalog.packages[0]), package_compare_for_index);
    const int result = write_index(repository_dir, &catalog, error, error_size);
    pux_repo_catalog_free(&catalog);
    return result;
}

static int read_line(FILE *file, char *buffer, size_t size)
{
    if (fgets(buffer, (int)size, file) == NULL) return feof(file) ? 0 : -1;
    const size_t length = strlen(buffer);
    if (length == 0U || buffer[length - 1U] != '\n') return -1;
    buffer[length - 1U] = '\0';
    if (length > 1U && buffer[length - 2U] == '\r') buffer[length - 2U] = '\0';
    return 1;
}

static int parse_index(const char *path, struct pux_repo_catalog *catalog,
                       char *error, size_t error_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_errorf(error, error_size, "cannot open repository index: %s", strerror(errno));
        return -1;
    }
    char line[PUX_PACKAGE_MAX_LINE];
    int result = read_line(file, line, sizeof(line));
    if (result != 1 || strcmp(line, "# pux-index=1") != 0) {
        fclose(file);
        set_error(error, error_size, "invalid repository index header");
        return -1;
    }

    struct pux_repo_package current = {0};
    int in_package = 0;
    int saw_package_field = 0;
    int saw_size_field = 0;
    int saw_sha256_field = 0;
    int saw_manifest_field = 0;
    for (;;) {
        result = read_line(file, line, sizeof(line));
        if (result == 0) break;
        if (result < 0) {
            fclose(file);
            package_free(&current);
            set_error(error, error_size, "invalid or oversized repository index line");
            return -1;
        }
        if (line[0] == '\0') {
            if (in_package == 0) continue;
            if (saw_package_field == 0 || saw_size_field == 0 || saw_sha256_field == 0 || saw_manifest_field == 0 ||
                pux_package_manifest_validate(&current.manifest, error, error_size) != 0) {
                fclose(file);
                package_free(&current);
                if (error[0] == '\0') set_error(error, error_size, "invalid package entry in repository index");
                return -1;
            }
            struct pux_repo_package *items = realloc(catalog->packages,
                                                       (catalog->count + 1U) * sizeof(*items));
            if (items == NULL) {
                fclose(file);
                package_free(&current);
                set_error(error, error_size, "out of memory while reading repository index");
                return -1;
            }
            catalog->packages = items;
            catalog->packages[catalog->count] = current;
            catalog->count++;
            memset(&current, 0, sizeof(current));
            in_package = 0;
            saw_package_field = 0;
            saw_size_field = 0;
            saw_sha256_field = 0;
            saw_manifest_field = 0;
            continue;
        }

        if (strncmp(line, "package=", 8U) == 0) {
            if (in_package != 0 || line[8] == '\0' || !is_package_filename(line + 8)) {
                fclose(file); package_free(&current);
                set_error(error, error_size, "invalid or duplicate package field in repository index");
                return -1;
            }
            current.filename = duplicate_string(line + 8);
            if (current.filename == NULL) {
                fclose(file); package_free(&current);
                set_error(error, error_size, "out of memory while reading repository index");
                return -1;
            }
            in_package = 1;
            saw_package_field = 1;
            continue;
        }

        if (in_package == 0) {
            fclose(file); package_free(&current);
            set_error(error, error_size, "repository index field outside package entry");
            return -1;
        }

        if (strncmp(line, "size=", 5U) == 0) {
            if (saw_size_field != 0 || line[5] == '\0') {
                fclose(file); package_free(&current);
                set_error(error, error_size, "invalid or duplicate size field in repository index");
                return -1;
            }
            char *end = NULL;
            errno = 0;
            const unsigned long long value = strtoull(line + 5, &end, 10);
            if (errno != 0 || end == line + 5 || *end != '\0' || value > SIZE_MAX) {
                fclose(file); package_free(&current);
                set_error(error, error_size, "invalid package size in repository index");
                return -1;
            }
            current.size = (size_t)value;
            saw_size_field = 1;
            continue;
        }

        if (strncmp(line, "sha256=", 7U) == 0) {
            if (saw_sha256_field != 0 || strlen(line + 7U) != 64U) {
                fclose(file); package_free(&current);
                set_error(error, error_size, "invalid or duplicate SHA-256 field in repository index");
                return -1;
            }
            for (size_t i = 0U; i < 64U; ++i) {
                const unsigned char c = (unsigned char)line[7U + i];
                const int valid = (c >= '0' && c <= '9') ||
                                  (c >= 'a' && c <= 'f') ||
                                  (c >= 'A' && c <= 'F');
                if (!valid) {
                    fclose(file); package_free(&current);
                    set_error(error, error_size, "invalid SHA-256 field in repository index");
                    return -1;
                }
                current.sha256[i] = (char)tolower(c);
            }
            current.sha256[64U] = '\0';
            saw_sha256_field = 1;
            continue;
        }

        /* The remaining records form the canonical package manifest. */
        if (strcmp(line, "format=1") == 0 || strncmp(line, "name=", 5U) == 0 ||
            strncmp(line, "version=", 8U) == 0 || strncmp(line, "release=", 8U) == 0 ||
            strncmp(line, "arch=", 5U) == 0 || strncmp(line, "description=", 12U) == 0 ||
            strncmp(line, "license=", 8U) == 0 || strncmp(line, "depends=", 8U) == 0 ||
            strncmp(line, "provides=", 9U) == 0 || strncmp(line, "conflicts=", 10U) == 0 ||
            strncmp(line, "replaces=", 9U) == 0) {
            /* Capture manifest lines into a bounded temporary file by canonical serialization.
             * We parse the line through the package parser using a small accumulated buffer. */
            static char manifest_buffer[PUX_REPO_MAX_INDEX_SIZE];
            (void)manifest_buffer;
        }

        /* Simpler and safer: reparse each manifest record via a temporary file containing
         * the current canonical manifest plus this line. */
        if (strcmp(line, "--- files ---") == 0) {
            fclose(file); package_free(&current);
            set_error(error, error_size, "unexpected database marker in repository index");
            return -1;
        }
        FILE *tmp = tmpfile();
        if (tmp == NULL) {
            fclose(file); package_free(&current);
            set_error(error, error_size, "cannot allocate repository manifest parser");
            return -1;
        }
        if (current.manifest.format != 0U) (void)fprintf(tmp, "format=%u\n", current.manifest.format);
        if (current.manifest.name != NULL) (void)fprintf(tmp, "name=%s\n", current.manifest.name);
        if (current.manifest.version != NULL) (void)fprintf(tmp, "version=%s\n", current.manifest.version);
        if (current.manifest.release != 0U) (void)fprintf(tmp, "release=%u\n", current.manifest.release);
        if (current.manifest.arch != NULL) (void)fprintf(tmp, "arch=%s\n", current.manifest.arch);
        if (current.manifest.description != NULL) (void)fprintf(tmp, "description=%s\n", current.manifest.description);
        if (current.manifest.license != NULL) (void)fprintf(tmp, "license=%s\n", current.manifest.license);
        for (size_t i = 0U; i < current.manifest.depends.count; ++i) (void)fprintf(tmp, "depends=%s\n", current.manifest.depends.items[i]);
        for (size_t i = 0U; i < current.manifest.provides.count; ++i) (void)fprintf(tmp, "provides=%s\n", current.manifest.provides.items[i]);
        for (size_t i = 0U; i < current.manifest.conflicts.count; ++i) (void)fprintf(tmp, "conflicts=%s\n", current.manifest.conflicts.items[i]);
        for (size_t i = 0U; i < current.manifest.replaces.count; ++i) (void)fprintf(tmp, "replaces=%s\n", current.manifest.replaces.items[i]);
        (void)fprintf(tmp, "%s\n", line);
        if (fflush(tmp) != 0 || fseek(tmp, 0L, SEEK_END) != 0) {
            fclose(tmp); fclose(file); package_free(&current);
            set_error(error, error_size, "cannot prepare repository manifest parser");
            return -1;
        }
        const long end = ftell(tmp);
        if (end <= 0L || (unsigned long)end > (unsigned long)PUX_PACKAGE_MAX_MANIFEST_SIZE || fseek(tmp, 0L, SEEK_SET) != 0) {
            fclose(tmp); fclose(file); package_free(&current);
            set_error(error, error_size, "repository manifest is too large");
            return -1;
        }
        const size_t buffer_size = (size_t)end;
        unsigned char *buffer = malloc(buffer_size);
        if (buffer == NULL) {
            fclose(tmp); fclose(file); package_free(&current);
            set_error(error, error_size, "out of memory while parsing repository index");
            return -1;
        }
        const size_t read_size = fread(buffer, 1U, buffer_size, tmp);
        fclose(tmp);
        if (read_size != buffer_size) {
            free(buffer); fclose(file); package_free(&current);
            set_error(error, error_size, "cannot read repository manifest buffer");
            return -1;
        }
        struct pux_package_manifest parsed;
        char manifest_error[512] = {0};
        if (pux_package_manifest_read_buffer(buffer, buffer_size, &parsed,
                                             manifest_error, sizeof(manifest_error)) != 0) {
            free(buffer); fclose(file); package_free(&current);
            set_errorf(error, error_size, "invalid repository manifest record: %s", line);
            return -1;
        }
        free(buffer);
        pux_package_manifest_free(&current.manifest);
        current.manifest = parsed;
        saw_manifest_field = 1;
    }
    fclose(file);

    if (in_package != 0) {
        if (saw_package_field == 0 || saw_size_field == 0 || saw_sha256_field == 0 || saw_manifest_field == 0 ||
            pux_package_manifest_validate(&current.manifest, error, error_size) != 0) {
            package_free(&current);
            if (error[0] == '\0') set_error(error, error_size, "invalid final package entry in repository index");
            return -1;
        }
        struct pux_repo_package *items = realloc(catalog->packages,
                                                   (catalog->count + 1U) * sizeof(*items));
        if (items == NULL) {
            package_free(&current);
            set_error(error, error_size, "out of memory while reading repository index");
            return -1;
        }
        catalog->packages = items;
        catalog->packages[catalog->count++] = current;
        memset(&current, 0, sizeof(current));
    }

    if (catalog->count == 0U) {
        set_error(error, error_size, "repository index contains no packages");
        return -1;
    }
    return 0;
}

int pux_repo_load_index(const char *repository_dir,
                        struct pux_repo_catalog *catalog,
                        char *error, size_t error_size)
{
    if (catalog == NULL) {
        set_error(error, error_size, "invalid repository catalog argument");
        return -1;
    }
    memset(catalog, 0, sizeof(*catalog));
    char path[PATH_MAX];
    if (join_path(repository_dir, PUX_REPO_INDEX_NAME, path, sizeof(path)) != 0) {
        set_error(error, error_size, "repository index path is too long");
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0L ||
        (unsigned long long)st.st_size > PUX_REPO_MAX_INDEX_SIZE) {
        set_error(error, error_size, "repository index is missing or invalid");
        return -1;
    }
    return parse_index(path, catalog, error, error_size);
}

int pux_repo_validate_index(const char *repository_dir,
                            char *error, size_t error_size)
{
    struct pux_repo_catalog catalog = {0};
    if (pux_repo_load_index(repository_dir, &catalog, error, error_size) != 0) return -1;

    for (size_t i = 0U; i < catalog.count; ++i) {
        char package_path[PATH_MAX];
        if (join_path(repository_dir, catalog.packages[i].filename,
                      package_path, sizeof(package_path)) != 0) {
            pux_repo_catalog_free(&catalog);
            set_error(error, error_size, "repository package path is too long");
            return -1;
        }
        struct stat st;
        if (stat(package_path, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size < 0L || (size_t)st.st_size != catalog.packages[i].size) {
            set_errorf(error, error_size, "repository index size mismatch: %s", catalog.packages[i].filename);
            pux_repo_catalog_free(&catalog);
            return -1;
        }
        char actual_sha256[PUX_SHA256_HEX_SIZE];
        char hash_error[512] = {0};
        if (pux_sha256_file(package_path, actual_sha256, hash_error, sizeof(hash_error)) != 0) {
            set_errorf(error, error_size, "cannot hash repository package: %s", catalog.packages[i].filename);
            pux_repo_catalog_free(&catalog);
            return -1;
        }
        if (memcmp(actual_sha256, catalog.packages[i].sha256, PUX_SHA256_HEX_SIZE) != 0) {
            set_errorf(error, error_size, "repository index SHA-256 mismatch: %s", catalog.packages[i].filename);
            pux_repo_catalog_free(&catalog);
            return -1;
        }
        struct pux_package_manifest manifest;
        char package_error[512] = {0};
        if (pux_package_archive_validate(package_path, &manifest, package_error, sizeof(package_error)) != 0) {
            set_errorf(error, error_size, "repository package is invalid: %s", catalog.packages[i].filename);
            pux_repo_catalog_free(&catalog);
            return -1;
        }
        const int same = strcmp(manifest.name, catalog.packages[i].manifest.name) == 0 &&
                         strcmp(manifest.version, catalog.packages[i].manifest.version) == 0 &&
                         manifest.release == catalog.packages[i].manifest.release &&
                         strcmp(manifest.arch, catalog.packages[i].manifest.arch) == 0;
        pux_package_manifest_free(&manifest);
        if (same == 0) {
            set_errorf(error, error_size, "repository index metadata mismatch: %s", catalog.packages[i].filename);
            pux_repo_catalog_free(&catalog);
            return -1;
        }
    }

    pux_repo_catalog_free(&catalog);
    return 0;
}

int pux_repo_verify_package(const char *repository_dir,
                            const char *package_path,
                            char *error, size_t error_size)
{
    if (repository_dir == NULL || package_path == NULL) {
        set_error(error, error_size, "invalid repository package verification argument");
        return -1;
    }
    const char *filename = strrchr(package_path, '/');
    filename = filename != NULL ? filename + 1 : package_path;
    if (!is_package_filename(filename)) {
        set_error(error, error_size, "repository package filename is invalid");
        return -1;
    }

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
        set_errorf(error, error_size, "package is not present in repository index: %s", filename);
        return -1;
    }

    char indexed_path[PATH_MAX];
    if (join_path(repository_dir, filename, indexed_path, sizeof(indexed_path)) != 0) {
        pux_repo_catalog_free(&catalog);
        set_error(error, error_size, "repository package path is too long");
        return -1;
    }
    char actual_sha256[PUX_SHA256_HEX_SIZE];
    if (pux_sha256_file(indexed_path, actual_sha256, error, error_size) != 0) {
        pux_repo_catalog_free(&catalog);
        return -1;
    }
    if (memcmp(actual_sha256, found->sha256, PUX_SHA256_HEX_SIZE) != 0) {
        set_errorf(error, error_size, "repository package SHA-256 mismatch: %s", filename);
        pux_repo_catalog_free(&catalog);
        return -1;
    }
    pux_repo_catalog_free(&catalog);
    return 0;
}

static int contains_case_insensitive(const char *value, const char *term)
{
    if (term == NULL || term[0] == '\0') return 1;
    const size_t term_len = strlen(term);
    for (const char *p = value; *p != '\0'; ++p) {
        size_t i = 0U;
        while (i < term_len && p[i] != '\0') {
            unsigned char a = (unsigned char)p[i];
            unsigned char b = (unsigned char)term[i];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
            if (a != b) break;
            ++i;
        }
        if (i == term_len) return 1;
    }
    return 0;
}

int pux_repo_search(const char *repository_dir,
                    const char *term,
                    FILE *output,
                    char *error, size_t error_size)
{
    struct pux_repo_catalog catalog = {0};
    if (pux_repo_load_index(repository_dir, &catalog, error, error_size) != 0) return -1;
    int found = 0;
    for (size_t i = 0U; i < catalog.count; ++i) {
        const struct pux_repo_package *package = &catalog.packages[i];
        if (contains_case_insensitive(package->manifest.name, term) != 0 ||
            contains_case_insensitive(package->manifest.description, term) != 0) {
            fprintf(output, "%s %s-%u %s %s\n",
                    package->manifest.name, package->manifest.version,
                    package->manifest.release, package->manifest.arch,
                    package->manifest.description);
            found = 1;
        }
    }
    if (found == 0) fprintf(output, "No packages found.\n");
    pux_repo_catalog_free(&catalog);
    return 0;
}
