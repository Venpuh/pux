#include "pux/container.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    size_t length = strlen(path);
    if (length >= 4096U) {
        return 0;
    }

    /* Directory members conventionally end with '/'; it is not a path component. */
    if (length > 0U && path[length - 1U] == '/') {
        --length;
    }
    if (length == 0U || path[0] == '/') {
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

static int normalize_directory_suffix(const char *path, char *normalized, size_t size)
{
    const size_t length = strlen(path);
    if (length + 1U > size) {
        return -1;
    }
    memcpy(normalized, path, length + 1U);
    if (length > 0U && normalized[length - 1U] == '/') {
        normalized[length - 1U] = '\0';
    }
    return 0;
}

static int is_allowed_path(const char *path)
{
    char normalized[4096];
    if (normalize_directory_suffix(path, normalized, sizeof(normalized)) != 0) {
        return 0;
    }

    if (strcmp(normalized, "META") == 0 || strcmp(normalized, "META/manifest") == 0 ||
        strcmp(normalized, "payload") == 0 || strncmp(normalized, "payload/", 8U) == 0) {
        return safe_relative_path(path);
    }
    return 0;
}

static int skip_bytes(FILE *file, uint64_t size)
{
    if (size > (uint64_t)LONG_MAX) {
        return -1;
    }
    return fseek(file, (long)size, SEEK_CUR);
}

static int skip_padding(FILE *file, uint64_t size)
{
    const uint64_t remainder = size % TAR_BLOCK_SIZE;
    if (remainder == 0U) {
        return 0;
    }
    return skip_bytes(file, TAR_BLOCK_SIZE - remainder);
}

static int read_string_field(const unsigned char *header, size_t offset, size_t size,
                             char *destination, size_t destination_size)
{
    const size_t length = field_length(header + offset, size);
    if (length == 0U || length >= destination_size) {
        return -1;
    }
    memcpy(destination, header + offset, length);
    destination[length] = '\0';
    return 0;
}

static int read_manifest_payload(FILE *file, uint64_t size,
                                 unsigned char **buffer, size_t *buffer_size,
                                 char *error, size_t error_size)
{
    if (size == 0U || size > (uint64_t)PUX_PACKAGE_MAX_MANIFEST_SIZE ||
        size > (uint64_t)SIZE_MAX) {
        set_error(error, error_size, "META/manifest has invalid size");
        return -1;
    }

    const size_t length = (size_t)size;
    unsigned char *data = malloc(length);
    if (data == NULL) {
        set_error(error, error_size, "out of memory while reading META/manifest");
        return -1;
    }

    if (fread(data, 1U, length, file) != length) {
        free(data);
        set_error(error, error_size, "truncated META/manifest");
        return -1;
    }

    *buffer = data;
    *buffer_size = length;
    return 0;
}

int pux_package_archive_validate(const char *path,
                                 struct pux_package_manifest *manifest,
                                 char *error, size_t error_size)
{
    if (path == NULL || manifest == NULL) {
        set_error(error, error_size, "invalid package archive argument");
        return -1;
    }

    pux_package_manifest_init(manifest);

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_errorf(error, error_size, "cannot open package: %s", strerror(errno));
        return -1;
    }

    unsigned char header[TAR_BLOCK_SIZE];
    unsigned char *manifest_data = NULL;
    size_t manifest_size = 0U;
    int saw_manifest = 0;
    int saw_end = 0;
    uint64_t member_count = 0U;

    for (;;) {
        if (fread(header, 1U, TAR_BLOCK_SIZE, file) != TAR_BLOCK_SIZE) {
            set_error(error, error_size, "truncated tar header");
            goto fail;
        }

        if (is_zero_block(header)) {
            saw_end = 1;
            break;
        }

        if (!field_equals(header + 257U, 6U, "ustar")) {
            set_error(error, error_size, "unsupported tar format; expected ustar");
            goto fail;
        }

        uint64_t stored_checksum = 0U;
        if (parse_octal(header + 148U, 8U, &stored_checksum) != 0 ||
            stored_checksum != checksum(header)) {
            set_error(error, error_size, "tar header checksum mismatch");
            goto fail;
        }

        char name[4096];
        if (read_string_field(header, 0U, 100U, name, sizeof(name)) != 0) {
            set_error(error, error_size, "invalid tar member name");
            goto fail;
        }

        uint64_t size = 0U;
        if (parse_octal(header + 124U, 12U, &size) != 0) {
            set_errorf(error, error_size, "invalid size for tar member: %s", name);
            goto fail;
        }

        const char type = (char)header[156U];
        if (type == TAR_TYPE_PAX || type == TAR_TYPE_GLOBAL_PAX ||
            type == TAR_TYPE_LONGLINK || type == TAR_TYPE_LONGNAME || type == TAR_TYPE_GNU_EXT) {
            set_error(error, error_size, "unsupported tar extension header");
            goto fail;
        }

        if (!is_allowed_path(name)) {
            set_errorf(error, error_size, "unsafe or unsupported package path: %s", name);
            goto fail;
        }

        if (type == TAR_TYPE_SYM || type == TAR_TYPE_HARD) {
            set_errorf(error, error_size, "links are not supported in package archive: %s", name);
            goto fail;
        }

        if (type != TAR_TYPE_REG && type != TAR_TYPE_ALT_REG && type != TAR_TYPE_DIR) {
            set_errorf(error, error_size, "unsupported tar member type: %s", name);
            goto fail;
        }

        if (strcmp(name, "META/manifest") == 0) {
            if (type != TAR_TYPE_REG && type != TAR_TYPE_ALT_REG) {
                set_error(error, error_size, "META/manifest must be a regular file");
                goto fail;
            }
            if (saw_manifest) {
                set_error(error, error_size, "duplicate META/manifest");
                goto fail;
            }
            if (read_manifest_payload(file, size, &manifest_data, &manifest_size,
                                      error, error_size) != 0) {
                goto fail;
            }
            saw_manifest = 1;
        } else {
            if (skip_bytes(file, size) != 0) {
                set_errorf(error, error_size, "truncated tar member: %s", name);
                goto fail;
            }
        }

        if (skip_padding(file, size) != 0) {
            set_errorf(error, error_size, "invalid tar padding after member: %s", name);
            goto fail;
        }

        ++member_count;
        if (member_count > 1000000U) {
            set_error(error, error_size, "too many archive members");
            goto fail;
        }
    }

    if (!saw_end) {
        set_error(error, error_size, "tar archive has no end marker");
        goto fail;
    }

    /* POSIX tar uses two consecutive zero blocks. */
    if (fread(header, 1U, TAR_BLOCK_SIZE, file) != TAR_BLOCK_SIZE || !is_zero_block(header)) {
        set_error(error, error_size, "tar archive is missing its second end marker");
        goto fail;
    }

    if (!saw_manifest) {
        set_error(error, error_size, "package is missing META/manifest");
        goto fail;
    }

    if (pux_package_manifest_read_buffer(manifest_data, manifest_size, manifest,
                                         error, error_size) != 0 ||
        pux_package_manifest_validate(manifest, error, error_size) != 0) {
        goto fail;
    }

    free(manifest_data);
    fclose(file);
    return 0;

fail:
    free(manifest_data);
    fclose(file);
    pux_package_manifest_free(manifest);
    return -1;
}

int pux_package_archive_info(const char *path,
                             struct pux_package_manifest *manifest,
                             char *error, size_t error_size)
{
    return pux_package_archive_validate(path, manifest, error, error_size);
}
