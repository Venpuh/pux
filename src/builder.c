#define _POSIX_C_SOURCE 200809L

#include "pux/builder.h"
#include "pux/package.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TAR_BLOCK_SIZE 512U
#define TAR_NAME_SIZE 100U
#define TAR_MODE_SIZE 8U
#define TAR_UID_SIZE 8U
#define TAR_GID_SIZE 8U
#define TAR_SIZE_SIZE 12U
#define TAR_MTIME_SIZE 12U
#define TAR_CHKSUM_SIZE 8U
#define TAR_TYPE_REG '\0'
#define TAR_TYPE_DIR '5'
#define TAR_MAX_MEMBERS 1000000U

struct build_entry {
    char *archive_path;
    char *full_path;
    mode_t mode;
    int is_directory;
};

struct build_entries {
    struct build_entry *items;
    size_t count;
    size_t capacity;
};

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static void set_errorf(char *error, size_t error_size, const char *format, const char *value)
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

static void free_entries(struct build_entries *entries)
{
    if (entries == NULL) {
        return;
    }
    for (size_t i = 0U; i < entries->count; ++i) {
        free(entries->items[i].archive_path);
        free(entries->items[i].full_path);
    }
    free(entries->items);
    entries->items = NULL;
    entries->count = 0U;
    entries->capacity = 0U;
}

static int append_entry(struct build_entries *entries, const char *archive_path,
                        const char *full_path, const struct stat *st, int is_directory)
{
    const size_t archive_length = strlen(archive_path);
    if (archive_length == 0U || archive_length > TAR_NAME_SIZE) {
        return -2;
    }
    if (entries->count >= TAR_MAX_MEMBERS) {
        return -1;
    }

    if (entries->count == entries->capacity) {
        size_t new_capacity = entries->capacity == 0U ? 32U : entries->capacity * 2U;
        if (new_capacity > TAR_MAX_MEMBERS) {
            new_capacity = TAR_MAX_MEMBERS;
        }
        struct build_entry *items = realloc(entries->items, new_capacity * sizeof(*items));
        if (items == NULL) {
            return -1;
        }
        entries->items = items;
        entries->capacity = new_capacity;
    }

    char *archive_copy = duplicate_string(archive_path);
    char *full_copy = duplicate_string(full_path);
    if (archive_copy == NULL || full_copy == NULL) {
        free(archive_copy);
        free(full_copy);
        return -1;
    }

    entries->items[entries->count].archive_path = archive_copy;
    entries->items[entries->count].full_path = full_copy;
    entries->items[entries->count].mode = st->st_mode;
    entries->items[entries->count].is_directory = is_directory;
    ++entries->count;
    return 0;
}

static int compare_entries(const void *left, const void *right)
{
    const struct build_entry *a = left;
    const struct build_entry *b = right;
    return strcmp(a->archive_path, b->archive_path);
}

static int join_path(const char *base, const char *name, char *output, size_t output_size)
{
    const size_t base_length = strlen(base);
    const size_t name_length = strlen(name);
    const int needs_separator = base_length > 0U && base[base_length - 1U] != '/';
    const size_t total = base_length + (needs_separator ? 1U : 0U) + name_length + 1U;

    if (total > output_size) {
        return -1;
    }

    size_t position = 0U;
    memcpy(output + position, base, base_length);
    position += base_length;
    if (needs_separator) {
        output[position++] = '/';
    }
    memcpy(output + position, name, name_length);
    position += name_length;
    output[position] = '\0';
    return 0;
}

static int build_archive_path(const char *relative, int is_directory,
                              char *archive_path, size_t archive_size)
{
    const size_t relative_length = strlen(relative);
    const size_t suffix_length = is_directory ? 2U : 1U;
    const size_t total = 8U + relative_length + suffix_length;

    if (relative_length == 0U || total > archive_size || total - 1U > TAR_NAME_SIZE) {
        return -1;
    }

    (void)snprintf(archive_path, archive_size, "payload/%s%s", relative,
                   is_directory ? "/" : "");
    return 0;
}

static int add_tree(const char *root, const char *relative, struct build_entries *entries,
                    char *error, size_t error_size)
{
    char full_path[PATH_MAX];
    if (relative[0] == '\0') {
        const int written = snprintf(full_path, sizeof(full_path), "%s", root);
        if (written < 0 || (size_t)written >= sizeof(full_path)) {
            set_error(error, error_size, "payload root path is too long");
            return -1;
        }
    } else if (join_path(root, relative, full_path, sizeof(full_path)) != 0) {
        set_error(error, error_size, "payload path is too long");
        return -1;
    }

    struct stat st;
    if (lstat(full_path, &st) != 0) {
        set_errorf(error, error_size, "cannot stat payload entry: %s", strerror(errno));
        return -1;
    }

    if (S_ISLNK(st.st_mode)) {
        set_errorf(error, error_size, "symbolic links are not supported in packages: %s",
                   relative[0] == '\0' ? "." : relative);
        return -1;
    }

    const int is_directory = S_ISDIR(st.st_mode) ? 1 : 0;
    if (!is_directory && !S_ISREG(st.st_mode)) {
        set_errorf(error, error_size, "unsupported payload file type: %s",
                   relative[0] == '\0' ? "." : relative);
        return -1;
    }

    if (relative[0] != '\0') {
        char archive_path[TAR_NAME_SIZE + 1U];
        if (build_archive_path(relative, is_directory, archive_path, sizeof(archive_path)) != 0) {
            set_errorf(error, error_size, "payload path is too long for ustar: %s", relative);
            return -1;
        }

        const int add_result = append_entry(entries, archive_path, full_path, &st, is_directory);
        if (add_result == -2) {
            set_errorf(error, error_size, "payload path is too long for ustar: %s", relative);
            return -1;
        }
        if (add_result != 0) {
            set_error(error, error_size, "out of memory while collecting payload");
            return -1;
        }
    }

    if (!is_directory) {
        return 0;
    }

    DIR *directory = opendir(full_path);
    if (directory == NULL) {
        set_errorf(error, error_size, "cannot open payload directory: %s", strerror(errno));
        return -1;
    }

    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                set_errorf(error, error_size, "cannot read payload directory: %s", strerror(errno));
                result = -1;
            }
            break;
        }

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_relative[PATH_MAX];
        if (relative[0] == '\0') {
            const int written = snprintf(child_relative, sizeof(child_relative), "%s", entry->d_name);
            if (written < 0 || (size_t)written >= sizeof(child_relative)) {
                set_error(error, error_size, "payload path is too long");
                result = -1;
                break;
            }
        } else if (join_path(relative, entry->d_name, child_relative, sizeof(child_relative)) != 0) {
            set_error(error, error_size, "payload path is too long");
            result = -1;
            break;
        }

        if (add_tree(root, child_relative, entries, error, error_size) != 0) {
            result = -1;
            break;
        }
    }

    closedir(directory);
    return result;
}

static int write_full(FILE *file, const void *data, size_t size)
{
    return fwrite(data, 1U, size, file) == size ? 0 : -1;
}

static int write_octal(unsigned char *field, size_t field_size, uint64_t value)
{
    if (field_size < 2U) {
        return -1;
    }

    memset(field, '0', field_size);
    field[field_size - 1U] = '\0';

    size_t index = field_size - 2U;
    do {
        if (index == 0U && value > 7U) {
            return -1;
        }
        field[index] = (unsigned char)('0' + (value & 7U));
        value >>= 3U;
        if (index == 0U) {
            break;
        }
        --index;
    } while (value != 0U);

    return value == 0U ? 0 : -1;
}

static int write_header(FILE *file, const char *name, const struct stat *st, int is_directory)
{
    if (strlen(name) > TAR_NAME_SIZE) {
        return -1;
    }

    unsigned char header[TAR_BLOCK_SIZE];
    memset(header, 0, sizeof(header));
    memcpy(header, name, strlen(name));

    const mode_t mode = st->st_mode & 07777U;
    if (write_octal(header + 100U, TAR_MODE_SIZE, (uint64_t)mode) != 0 ||
        write_octal(header + 108U, TAR_UID_SIZE, 0U) != 0 ||
        write_octal(header + 116U, TAR_GID_SIZE, 0U) != 0 ||
        write_octal(header + 124U, TAR_SIZE_SIZE,
                    is_directory ? 0U : (uint64_t)st->st_size) != 0 ||
        write_octal(header + 136U, TAR_MTIME_SIZE, 0U) != 0) {
        return -1;
    }

    memset(header + 148U, ' ', TAR_CHKSUM_SIZE);
    header[156U] = (unsigned char)(is_directory ? TAR_TYPE_DIR : TAR_TYPE_REG);
    memcpy(header + 257U, "ustar\0", 6U);
    memcpy(header + 263U, "00", 2U);
    memcpy(header + 265U, "root", 4U);
    memcpy(header + 297U, "root", 4U);

    uint64_t sum = 0U;
    for (size_t i = 0U; i < sizeof(header); ++i) {
        sum += header[i];
    }

    if (sum > 07777777U) {
        return -1;
    }
    (void)snprintf((char *)(header + 148U), TAR_CHKSUM_SIZE, "%06" PRIo64, sum);
    header[154U] = '\0';
    header[155U] = ' ';

    return write_full(file, header, sizeof(header));
}

static int write_padding(FILE *file, uint64_t size)
{
    const uint64_t remainder = size % TAR_BLOCK_SIZE;
    const size_t padding = (size_t)((TAR_BLOCK_SIZE - remainder) % TAR_BLOCK_SIZE);
    if (padding == 0U) {
        return 0;
    }

    unsigned char zeros[TAR_BLOCK_SIZE] = {0};
    return write_full(file, zeros, padding);
}

static int write_zero_blocks(FILE *file)
{
    unsigned char block[TAR_BLOCK_SIZE] = {0};
    return write_full(file, block, sizeof(block)) == 0 &&
           write_full(file, block, sizeof(block)) == 0 ? 0 : -1;
}

static int copy_file(FILE *output, const char *path, uint64_t expected_size,
                     char *error, size_t error_size)
{
    FILE *input = fopen(path, "rb");
    if (input == NULL) {
        set_errorf(error, error_size, "cannot open payload file: %s", strerror(errno));
        return -1;
    }

    unsigned char buffer[64U * 1024U];
    uint64_t copied = 0U;
    int result = 0;
    for (;;) {
        const size_t read_size = fread(buffer, 1U, sizeof(buffer), input);
        if (read_size > 0U) {
            if (copied > UINT64_MAX - (uint64_t)read_size ||
                write_full(output, buffer, read_size) != 0) {
                set_error(error, error_size, "cannot write package payload");
                result = -1;
                break;
            }
            copied += (uint64_t)read_size;
        }

        if (read_size < sizeof(buffer)) {
            if (ferror(input) != 0) {
                set_error(error, error_size, "cannot read payload file");
                result = -1;
            }
            break;
        }
    }

    if (result == 0 && copied != expected_size) {
        set_error(error, error_size, "payload file changed while building package");
        result = -1;
    }

    if (fclose(input) != 0 && result == 0) {
        set_error(error, error_size, "cannot close payload file");
        result = -1;
    }
    return result;
}

static int read_file_size(const char *path, uint64_t *size,
                          char *error, size_t error_size)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        set_errorf(error, error_size, "cannot stat manifest: %s", strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0L ||
        (uintmax_t)st.st_size > (uintmax_t)PUX_PACKAGE_MAX_MANIFEST_SIZE) {
        set_error(error, error_size, "manifest must be a regular file within size limit");
        return -1;
    }
    *size = (uint64_t)st.st_size;
    return 0;
}

static int write_manifest_entry(FILE *output, const char *manifest_path,
                                char *error, size_t error_size)
{
    uint64_t size = 0U;
    if (read_file_size(manifest_path, &size, error, error_size) != 0) {
        return -1;
    }

    struct stat st;
    if (stat(manifest_path, &st) != 0) {
        set_errorf(error, error_size, "cannot stat manifest: %s", strerror(errno));
        return -1;
    }

    if (write_header(output, "META/manifest", &st, 0) != 0) {
        set_error(error, error_size, "cannot write manifest tar header");
        return -1;
    }

    FILE *input = fopen(manifest_path, "rb");
    if (input == NULL) {
        set_errorf(error, error_size, "cannot open manifest: %s", strerror(errno));
        return -1;
    }

    unsigned char buffer[64U * 1024U];
    uint64_t copied = 0U;
    int result = 0;
    for (;;) {
        const size_t read_size = fread(buffer, 1U, sizeof(buffer), input);
        if (read_size > 0U) {
            if (copied > UINT64_MAX - (uint64_t)read_size ||
                write_full(output, buffer, read_size) != 0) {
                set_error(error, error_size, "cannot write package manifest");
                result = -1;
                break;
            }
            copied += (uint64_t)read_size;
        }
        if (read_size < sizeof(buffer)) {
            if (ferror(input) != 0) {
                set_error(error, error_size, "cannot read manifest");
                result = -1;
            }
            break;
        }
    }

    if (result == 0 && copied != size) {
        set_error(error, error_size, "manifest changed while building package");
        result = -1;
    }
    if (fclose(input) != 0 && result == 0) {
        set_error(error, error_size, "cannot close manifest");
        result = -1;
    }
    if (result != 0) {
        return -1;
    }

    if (write_padding(output, size) != 0) {
        set_error(error, error_size, "cannot write manifest padding");
        return -1;
    }
    return 0;
}

static int write_payload_entry(FILE *output, const struct build_entry *entry,
                               char *error, size_t error_size)
{
    struct stat st;
    if (lstat(entry->full_path, &st) != 0) {
        set_errorf(error, error_size, "cannot stat payload entry: %s", strerror(errno));
        return -1;
    }
    if (entry->is_directory != (S_ISDIR(st.st_mode) ? 1 : 0) ||
        (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
        set_errorf(error, error_size, "payload entry changed type: %s", entry->archive_path);
        return -1;
    }

    if (write_header(output, entry->archive_path, &st, entry->is_directory) != 0) {
        set_errorf(error, error_size, "cannot write tar header for: %s", entry->archive_path);
        return -1;
    }

    if (entry->is_directory) {
        return 0;
    }

    const uint64_t size = (uint64_t)st.st_size;
    if (copy_file(output, entry->full_path, size, error, error_size) != 0) {
        return -1;
    }
    if (write_padding(output, size) != 0) {
        set_errorf(error, error_size, "cannot write tar padding for: %s", entry->archive_path);
        return -1;
    }
    return 0;
}

int pux_package_build(const char *manifest_path, const char *payload_dir,
                      const char *output_path, char *error, size_t error_size)
{
    if (manifest_path == NULL || payload_dir == NULL || output_path == NULL) {
        set_error(error, error_size, "invalid package build argument");
        return -1;
    }

    struct pux_package_manifest manifest;
    char manifest_error[512] = {0};
    if (pux_package_manifest_read_file(manifest_path, &manifest, manifest_error,
                                       sizeof(manifest_error)) != 0 ||
        pux_package_manifest_validate(&manifest, manifest_error, sizeof(manifest_error)) != 0) {
        set_errorf(error, error_size, "invalid manifest: %s", manifest_error);
        pux_package_manifest_free(&manifest);
        return -1;
    }
    pux_package_manifest_free(&manifest);

    struct stat payload_stat;
    if (stat(payload_dir, &payload_stat) != 0 || !S_ISDIR(payload_stat.st_mode)) {
        set_errorf(error, error_size, "payload path is not a directory: %s", payload_dir);
        return -1;
    }

    if (strcmp(output_path, manifest_path) == 0 || strcmp(output_path, payload_dir) == 0) {
        set_error(error, error_size, "output path must differ from input paths");
        return -1;
    }

    struct build_entries entries = {0};
    if (add_tree(payload_dir, "", &entries, error, error_size) != 0) {
        free_entries(&entries);
        return -1;
    }
    qsort(entries.items, entries.count, sizeof(entries.items[0]), compare_entries);

    FILE *output = fopen(output_path, "wb");
    if (output == NULL) {
        free_entries(&entries);
        set_errorf(error, error_size, "cannot create package: %s", strerror(errno));
        return -1;
    }

    int result = 0;
    if (write_manifest_entry(output, manifest_path, error, error_size) != 0) {
        result = -1;
    }

    for (size_t i = 0U; result == 0 && i < entries.count; ++i) {
        if (write_payload_entry(output, &entries.items[i], error, error_size) != 0) {
            result = -1;
        }
    }

    if (result == 0 && write_zero_blocks(output) != 0) {
        set_error(error, error_size, "cannot write package end marker");
        result = -1;
    }

    if (fclose(output) != 0 && result == 0) {
        set_error(error, error_size, "cannot close package output");
        result = -1;
    }

    free_entries(&entries);
    if (result != 0) {
        (void)unlink(output_path);
        return -1;
    }
    return 0;
}
