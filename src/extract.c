#define _GNU_SOURCE 1

#include "pux/extract.h"
#include "pux/container.h"
#include "pux/package.h"

#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TAR_BLOCK_SIZE 512U
#define TAR_TYPE_REG '\0'
#define TAR_TYPE_ALT_REG '0'
#define TAR_TYPE_DIR '5'
#define TAR_TYPE_SYM '2'
#define TAR_TYPE_HARD '1'
#define TAR_TYPE_LONGLINK 'K'
#define TAR_TYPE_LONGNAME 'L'
#define TAR_TYPE_PAX 'x'
#define TAR_TYPE_GLOBAL_PAX 'g'
#define TAR_TYPE_GNU_EXT 'S'
#define TAR_NAME_MAX 4095U

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

static size_t field_length(const unsigned char *field, size_t size)
{
    size_t length = 0U;
    while (length < size && field[length] != '\0') {
        ++length;
    }
    return length;
}

static int field_equals(const unsigned char *field, size_t size, const char *value)
{
    const size_t value_len = strlen(value);
    const size_t length = field_length(field, size);
    return length == value_len && memcmp(field, value, value_len) == 0;
}

static int parse_octal(const unsigned char *field, size_t size, uint64_t *value)
{
    size_t i = 0U;
    while (i < size && (field[i] == ' ' || field[i] == '\0')) {
        ++i;
    }
    if (i == size) {
        return -1;
    }

    uint64_t result = 0U;
    int saw_digit = 0;
    for (; i < size; ++i) {
        const unsigned char c = field[i];
        if (c == ' ' || c == '\0') {
            break;
        }
        if (c < '0' || c > '7') {
            return -1;
        }
        saw_digit = 1;
        const uint64_t digit = (uint64_t)(c - '0');
        if (result > (UINT64_MAX - digit) / 8U) {
            return -1;
        }
        result = result * 8U + digit;
    }

    if (!saw_digit) {
        return -1;
    }
    *value = result;
    return 0;
}

static uint64_t checksum(const unsigned char *header)
{
    uint64_t sum = 0U;
    for (size_t i = 0U; i < TAR_BLOCK_SIZE; ++i) {
        if (i >= 148U && i < 156U) {
            sum += (uint64_t)' ';
        } else {
            sum += (uint64_t)header[i];
        }
    }
    return sum;
}

static int is_zero_block(const unsigned char *block)
{
    for (size_t i = 0U; i < TAR_BLOCK_SIZE; ++i) {
        if (block[i] != 0U) {
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
    if (length > TAR_NAME_MAX) {
        return 0;
    }

    size_t component_start = 0U;
    size_t effective_length = length;
    if (effective_length > 0U && path[effective_length - 1U] == '/') {
        --effective_length;
    }
    if (effective_length == 0U) {
        return 0;
    }

    for (size_t i = 0U; i <= effective_length; ++i) {
        if (i < effective_length && path[i] != '/') {
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

static int is_allowed_path(const char *path)
{
    char normalized[TAR_NAME_MAX + 1U];
    const size_t length = strlen(path);
    if (length > TAR_NAME_MAX) {
        return 0;
    }
    memcpy(normalized, path, length + 1U);
    if (length > 0U && normalized[length - 1U] == '/') {
        normalized[length - 1U] = '\0';
    }

    if (strcmp(normalized, "META") == 0 || strcmp(normalized, "META/manifest") == 0 ||
        strcmp(normalized, "payload") == 0 || strncmp(normalized, "payload/", 8U) == 0) {
        return safe_relative_path(path);
    }
    return 0;
}

static int read_link_target(const unsigned char *header, char *target, size_t target_size)
{
    const size_t length = field_length(header + 157U, 100U);
    if (length == 0U || length >= target_size) return -1;
    memcpy(target, header + 157U, length);
    target[length] = '\0';
    for (size_t i = 0U; i < length; ++i) {
        const unsigned char c = (unsigned char)target[i];
        if (c < 0x20U || c == 0x7fU) return -1;
    }
    return 0;
}

static int read_header(FILE *file, unsigned char *header,
                       char *name, size_t name_size,
                       uint64_t *size, char *type,
                       char *error, size_t error_size)
{
    if (fread(header, 1U, TAR_BLOCK_SIZE, file) != TAR_BLOCK_SIZE) {
        set_error(error, error_size, "truncated tar header");
        return -1;
    }
    if (is_zero_block(header)) {
        return 1;
    }
    if (!field_equals(header + 257U, 6U, "ustar")) {
        set_error(error, error_size, "unsupported tar format; expected ustar");
        return -1;
    }

    uint64_t stored_checksum = 0U;
    if (parse_octal(header + 148U, 8U, &stored_checksum) != 0 ||
        stored_checksum != checksum(header)) {
        set_error(error, error_size, "tar header checksum mismatch");
        return -1;
    }

    const size_t name_length = field_length(header, 100U);
    if (name_length == 0U || name_length >= name_size) {
        set_error(error, error_size, "invalid tar member name");
        return -1;
    }
    memcpy(name, header, name_length);
    name[name_length] = '\0';

    if (parse_octal(header + 124U, 12U, size) != 0) {
        set_errorf(error, error_size, "invalid size for tar member: %s", name);
        return -1;
    }

    *type = (char)header[156U];
    if (*type == TAR_TYPE_PAX || *type == TAR_TYPE_GLOBAL_PAX ||
        *type == TAR_TYPE_LONGLINK || *type == TAR_TYPE_LONGNAME || *type == TAR_TYPE_GNU_EXT) {
        set_error(error, error_size, "unsupported tar extension header");
        return -1;
    }

    if (!is_allowed_path(name)) {
        set_errorf(error, error_size, "unsafe or unsupported package path: %s", name);
        return -1;
    }
    if (*type == TAR_TYPE_HARD) {
        set_errorf(error, error_size, "hard links are not supported in package archive: %s", name);
        return -1;
    }
    if (*type == TAR_TYPE_SYM) {
        char target[101];
        if (*size != 0U || read_link_target(header, target, sizeof(target)) != 0) {
            set_errorf(error, error_size, "invalid symbolic link target: %s", name);
            return -1;
        }
    }
    if (*type != TAR_TYPE_REG && *type != TAR_TYPE_ALT_REG && *type != TAR_TYPE_DIR && *type != TAR_TYPE_SYM) {
        set_errorf(error, error_size, "unsupported tar member type: %s", name);
        return -1;
    }

    return 0;
}

static int skip_bytes(FILE *file, uint64_t size)
{
    if (size > (uint64_t)LONG_MAX || fseek(file, (long)size, SEEK_CUR) != 0) {
        return -1;
    }
    return 0;
}

static int skip_padding(FILE *file, uint64_t size)
{
    const uint64_t remainder = size % TAR_BLOCK_SIZE;
    if (remainder == 0U) {
        return 0;
    }
    return skip_bytes(file, TAR_BLOCK_SIZE - remainder);
}

static mode_t safe_mode(const unsigned char *header)
{
    uint64_t parsed = 0U;
    if (parse_octal(header + 100U, 8U, &parsed) != 0) {
        return 0644U;
    }
    /* Do not create setuid/setgid files while the package format has no trust policy yet. */
    return (mode_t)(parsed & 07777U & ~(uint64_t)(S_ISUID | S_ISGID));
}

static int open_destination(const char *path, int *fd, char *error, size_t error_size)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno != ENOENT) {
            set_errorf(error, error_size, "cannot inspect extraction destination: %s", strerror(errno));
            return -1;
        }
        if (mkdir(path, 0755) != 0) {
            set_errorf(error, error_size, "cannot create extraction destination: %s", strerror(errno));
            return -1;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        set_error(error, error_size, "extraction destination is not a directory");
        return -1;
    }

    const int opened = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (opened < 0) {
        set_errorf(error, error_size, "cannot open extraction destination: %s", strerror(errno));
        return -1;
    }
    *fd = opened;
    return 0;
}

static int open_parent_dir(int root_fd, const char *relative, int *parent_fd,
                           char *leaf, size_t leaf_size,
                           char *error, size_t error_size)
{
    if (relative == NULL || relative[0] == '\0') {
        set_error(error, error_size, "invalid empty payload path");
        return -1;
    }

    const char *last_slash = strrchr(relative, '/');
    const char *leaf_start = last_slash == NULL ? relative : last_slash + 1;
    if (*leaf_start == '\0' || strlen(leaf_start) + 1U > leaf_size) {
        set_error(error, error_size, "payload path component is too long");
        return -1;
    }
    memcpy(leaf, leaf_start, strlen(leaf_start) + 1U);

    const size_t parent_length = last_slash == NULL ? 0U : (size_t)(last_slash - relative);
    if (parent_length == 0U) {
        *parent_fd = dup(root_fd);
        if (*parent_fd < 0) {
            set_errorf(error, error_size, "cannot duplicate extraction root: %s", strerror(errno));
            return -1;
        }
        return 0;
    }
    if (parent_length > TAR_NAME_MAX) {
        set_error(error, error_size, "payload parent path is too long");
        return -1;
    }

    char parent_path[TAR_NAME_MAX + 1U];
    memcpy(parent_path, relative, parent_length);
    parent_path[parent_length] = '\0';

    int current = dup(root_fd);
    if (current < 0) {
        set_errorf(error, error_size, "cannot duplicate extraction root: %s", strerror(errno));
        return -1;
    }

    char *cursor = parent_path;
    while (*cursor != '\0') {
        char *slash = strchr(cursor, '/');
        if (slash != NULL) {
            *slash = '\0';
        }

        int next = openat(current, cursor, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT) {
            if (mkdirat(current, cursor, 0755) != 0 && errno != EEXIST) {
                set_errorf(error, error_size, "cannot create payload directory: %s", strerror(errno));
                close(current);
                return -1;
            }
            next = openat(current, cursor, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) {
            set_errorf(error, error_size, "cannot open payload directory: %s", strerror(errno));
            close(current);
            return -1;
        }
        close(current);
        current = next;

        if (slash == NULL) {
            break;
        }
        cursor = slash + 1;
    }

    *parent_fd = current;
    return 0;
}

static int copy_member(FILE *input, int output_fd, uint64_t size,
                       char *error, size_t error_size)
{
    unsigned char buffer[64U * 1024U];
    uint64_t remaining = size;
    while (remaining > 0U) {
        const size_t wanted = remaining > (uint64_t)sizeof(buffer) ?
            sizeof(buffer) : (size_t)remaining;
        const size_t got = fread(buffer, 1U, wanted, input);
        if (got != wanted) {
            set_error(error, error_size, "truncated tar payload");
            return -1;
        }
        size_t written_total = 0U;
        while (written_total < got) {
            const ssize_t written = write(output_fd, buffer + written_total, got - written_total);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                set_errorf(error, error_size, "cannot write extracted file: %s", strerror(errno));
                return -1;
            }
            if (written == 0) {
                set_error(error, error_size, "cannot write extracted file: short write");
                return -1;
            }
            written_total += (size_t)written;
        }
        remaining -= (uint64_t)got;
    }
    return 0;
}

static int extract_directory(int root_fd, const char *relative, mode_t mode,
                             char *error, size_t error_size)
{
    int parent_fd = -1;
    char leaf[NAME_MAX + 1U];
    if (open_parent_dir(root_fd, relative, &parent_fd, leaf, sizeof(leaf), error, error_size) != 0) {
        return -1;
    }

    int directory_fd = openat(parent_fd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int created = 0;
    if (directory_fd < 0 && errno == ENOENT) {
        if (mkdirat(parent_fd, leaf, mode) != 0) {
            set_errorf(error, error_size, "cannot create extracted directory: %s", strerror(errno));
            close(parent_fd);
            return -1;
        }
        directory_fd = openat(parent_fd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        created = 1;
    }
    if (directory_fd < 0) {
        set_errorf(error, error_size, "cannot open extracted directory: %s", strerror(errno));
        close(parent_fd);
        return -1;
    }

    if (created && fchmod(directory_fd, mode) != 0) {
        set_errorf(error, error_size, "cannot set extracted directory mode: %s", strerror(errno));
        close(directory_fd);
        close(parent_fd);
        return -1;
    }

    close(directory_fd);
    close(parent_fd);
    return 0;
}

static int extract_regular_file(FILE *input, int root_fd, const char *relative,
                                const unsigned char *header, uint64_t size,
                                char *error, size_t error_size)
{
    int parent_fd = -1;
    char leaf[NAME_MAX + 1U];
    if (open_parent_dir(root_fd, relative, &parent_fd, leaf, sizeof(leaf), error, error_size) != 0) {
        return -1;
    }

    const mode_t mode = safe_mode(header);
    const int output_fd = openat(parent_fd, leaf,
                                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                 mode);
    if (output_fd < 0) {
        set_errorf(error, error_size, "cannot create extracted file: %s", strerror(errno));
        close(parent_fd);
        return -1;
    }

    int result = copy_member(input, output_fd, size, error, error_size);
    if (result == 0 && fchmod(output_fd, mode) != 0) {
        set_errorf(error, error_size, "cannot set extracted file mode: %s", strerror(errno));
        result = -1;
    }
    if (close(output_fd) != 0 && result == 0) {
        set_errorf(error, error_size, "cannot close extracted file: %s", strerror(errno));
        result = -1;
    }

    if (result != 0) {
        (void)unlinkat(parent_fd, leaf, 0);
    }
    close(parent_fd);
    return result;
}

static int extract_symlink(int root_fd, const char *relative, const unsigned char *header,
                           char *error, size_t error_size)
{
    char target[101];
    if (read_link_target(header, target, sizeof(target)) != 0) {
        set_errorf(error, error_size, "invalid symbolic link target: %s", relative);
        return -1;
    }
    int parent_fd = -1;
    char leaf[NAME_MAX + 1U];
    if (open_parent_dir(root_fd, relative, &parent_fd, leaf, sizeof(leaf), error, error_size) != 0) {
        return -1;
    }
    if (symlinkat(target, parent_fd, leaf) != 0) {
        set_errorf(error, error_size, "cannot create extracted symbolic link: %s", strerror(errno));
        close(parent_fd);
        return -1;
    }
    if (close(parent_fd) != 0) {
        set_errorf(error, error_size, "cannot close extraction directory: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int pux_package_archive_extract(const char *package_path,
                                const char *destination,
                                char *error, size_t error_size)
{
    if (package_path == NULL || destination == NULL || destination[0] == '\0') {
        set_error(error, error_size, "invalid package extraction argument");
        return -1;
    }

    struct pux_package_manifest manifest;
    char validation_error[512] = {0};
    if (pux_package_archive_validate(package_path, &manifest,
                                      validation_error, sizeof(validation_error)) != 0) {
        set_errorf(error, error_size, "package validation failed: %s", validation_error);
        return -1;
    }
    pux_package_manifest_free(&manifest);

    FILE *file = fopen(package_path, "rb");
    if (file == NULL) {
        set_errorf(error, error_size, "cannot open package: %s", strerror(errno));
        return -1;
    }

    int root_fd = -1;
    if (open_destination(destination, &root_fd, error, error_size) != 0) {
        fclose(file);
        return -1;
    }

    unsigned char header[TAR_BLOCK_SIZE];
    char name[TAR_NAME_MAX + 1U];
    uint64_t member_size = 0U;
    char type = '\0';
    int result = 0;

    for (;;) {
        const int header_result = read_header(file, header, name, sizeof(name), &member_size,
                                              &type, error, error_size);
        if (header_result == 1) {
            break;
        }
        if (header_result != 0) {
            result = -1;
            break;
        }

        if (strcmp(name, "META") == 0 || strcmp(name, "META/manifest") == 0 ||
            strcmp(name, "payload") == 0) {
            if (skip_bytes(file, member_size) != 0 || skip_padding(file, member_size) != 0) {
                set_error(error, error_size, "invalid metadata tar member");
                result = -1;
                break;
            }
            continue;
        }

        if (strncmp(name, "payload/", 8U) != 0) {
            set_errorf(error, error_size, "unsafe or unsupported package path: %s", name);
            result = -1;
            break;
        }

        char *relative = name + 8U;
        if (*relative == '\0') {
            set_error(error, error_size, "invalid empty payload path");
            result = -1;
            break;
        }
        const size_t relative_length = strlen(relative);
        if (relative[relative_length - 1U] == '/') {
            relative[relative_length - 1U] = '\0';
        }
        if (!safe_relative_path(relative)) {
            set_errorf(error, error_size, "unsafe payload path: %s", relative);
            result = -1;
            break;
        }

        if (type == TAR_TYPE_DIR) {
            if (member_size != 0U) {
                set_errorf(error, error_size, "directory member has non-zero size: %s", name);
                result = -1;
                break;
            }
            if (extract_directory(root_fd, relative, safe_mode(header), error, error_size) != 0) {
                result = -1;
                break;
            }
        } else if (type == TAR_TYPE_SYM) {
            if (member_size != 0U || extract_symlink(root_fd, relative, header, error, error_size) != 0) {
                result = -1;
                break;
            }
        } else {
            if (extract_regular_file(file, root_fd, relative, header, member_size,
                                     error, error_size) != 0) {
                result = -1;
                break;
            }
        }

        if (type == TAR_TYPE_DIR) {
            if (skip_bytes(file, member_size) != 0) {
                set_errorf(error, error_size, "invalid directory member: %s", name);
                result = -1;
                break;
            }
        }
        if (skip_padding(file, member_size) != 0) {
            set_errorf(error, error_size, "invalid tar padding after member: %s", name);
            result = -1;
            break;
        }
    }

    if (result == 0) {
        if (fread(header, 1U, TAR_BLOCK_SIZE, file) != TAR_BLOCK_SIZE || !is_zero_block(header)) {
            set_error(error, error_size, "tar archive is missing its second end marker");
            result = -1;
        }
    }

    close(root_fd);
    fclose(file);
    return result;
}
