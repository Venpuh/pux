#include "pux/package.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error == NULL || error_size == 0U) {
        return;
    }

    (void)snprintf(error, error_size, "%s", message);
}

static void set_errorf(char *error, size_t error_size, const char *format,
                       const char *value)
{
    if (error == NULL || error_size == 0U) {
        return;
    }

    (void)snprintf(error, error_size, format, value);
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

static int append_string(struct pux_package_string_list *list, const char *value)
{
    if (list->count >= PUX_PACKAGE_MAX_LIST_ITEMS) {
        return -1;
    }

    char *copy = duplicate_string(value);
    if (copy == NULL) {
        return -1;
    }

    char **items = realloc(list->items, (list->count + 1U) * sizeof(*items));
    if (items == NULL) {
        free(copy);
        return -1;
    }

    list->items = items;
    list->items[list->count] = copy;
    list->count++;
    return 0;
}

static int is_valid_token(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return 0;
    }

    if (!isalnum((unsigned char)value[0])) {
        return 0;
    }

    for (size_t i = 1U; value[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)value[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '+' || c == ':')) {
            return 0;
        }
    }

    return 1;
}

static int is_valid_scalar_value(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return 0;
    }

    for (size_t i = 0U; value[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)value[i];
        if (c < 0x20U || c == 0x7fU) {
            return 0;
        }
    }

    return 1;
}

static int parse_unsigned(const char *value, unsigned *result)
{
    if (value == NULL || value[0] == '\0') {
        return -1;
    }

    errno = 0;
    char *end = NULL;
    const unsigned long parsed = strtoul(value, &end, 10);

    if (errno != 0 || end == value || *end != '\0' || parsed > UINT_MAX) {
        return -1;
    }

    *result = (unsigned)parsed;
    return 0;
}

static int set_scalar(char **destination, const char *value)
{
    if (*destination != NULL) {
        return -1;
    }

    *destination = duplicate_string(value);
    return *destination == NULL ? -1 : 0;
}

void pux_package_manifest_init(struct pux_package_manifest *manifest)
{
    if (manifest == NULL) {
        return;
    }
    memset(manifest, 0, sizeof(*manifest));
}

static void free_string_list(struct pux_package_string_list *list)
{
    for (size_t i = 0U; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0U;
}

void pux_package_manifest_free(struct pux_package_manifest *manifest)
{
    if (manifest == NULL) {
        return;
    }

    free(manifest->name);
    free(manifest->version);
    free(manifest->arch);
    free(manifest->description);
    free(manifest->license);

    free_string_list(&manifest->depends);
    free_string_list(&manifest->provides);
    free_string_list(&manifest->conflicts);
    free_string_list(&manifest->replaces);

    pux_package_manifest_init(manifest);
}

static int parse_line(struct pux_package_manifest *manifest, char *line,
                      char *error, size_t error_size)
{
    const size_t length = strlen(line);
    if (length > 0U && line[length - 1U] == '\r') {
        line[length - 1U] = '\0';
    }

    if (line[0] == '\0' || line[0] == '#') {
        return 0;
    }

    char *equals = strchr(line, '=');
    if (equals == NULL || equals == line) {
        set_error(error, error_size, "invalid manifest record; expected key=value");
        return -1;
    }

    *equals = '\0';
    const char *key = line;
    const char *value = equals + 1;

    if (!is_valid_token(key)) {
        set_errorf(error, error_size, "invalid manifest key: %s", key);
        return -1;
    }

    if (!is_valid_scalar_value(value)) {
        set_errorf(error, error_size, "invalid manifest value for key: %s", key);
        return -1;
    }

    if (strcmp(key, "format") == 0) {
        unsigned parsed = 0U;
        if (manifest->format != 0U || parse_unsigned(value, &parsed) != 0) {
            set_error(error, error_size, "invalid or duplicate format field");
            return -1;
        }
        manifest->format = parsed;
    } else if (strcmp(key, "name") == 0) {
        if (!is_valid_token(value) || set_scalar(&manifest->name, value) != 0) {
            set_error(error, error_size, "invalid or duplicate name field");
            return -1;
        }
    } else if (strcmp(key, "version") == 0) {
        if (!is_valid_token(value) || set_scalar(&manifest->version, value) != 0) {
            set_error(error, error_size, "invalid or duplicate version field");
            return -1;
        }
    } else if (strcmp(key, "release") == 0) {
        unsigned parsed = 0U;
        if (manifest->release != 0U || parse_unsigned(value, &parsed) != 0 || parsed == 0U) {
            set_error(error, error_size, "invalid or duplicate release field");
            return -1;
        }
        manifest->release = parsed;
    } else if (strcmp(key, "arch") == 0) {
        if (!is_valid_token(value) || set_scalar(&manifest->arch, value) != 0) {
            set_error(error, error_size, "invalid or duplicate arch field");
            return -1;
        }
    } else if (strcmp(key, "description") == 0) {
        if (set_scalar(&manifest->description, value) != 0) {
            set_error(error, error_size, "invalid or duplicate description field");
            return -1;
        }
    } else if (strcmp(key, "license") == 0) {
        if (set_scalar(&manifest->license, value) != 0) {
            set_error(error, error_size, "invalid or duplicate license field");
            return -1;
        }
    } else if (strcmp(key, "depends") == 0) {
        if (append_string(&manifest->depends, value) != 0) {
            set_error(error, error_size, "too many or invalid depends fields");
            return -1;
        }
    } else if (strcmp(key, "provides") == 0) {
        if (append_string(&manifest->provides, value) != 0) {
            set_error(error, error_size, "too many or invalid provides fields");
            return -1;
        }
    } else if (strcmp(key, "conflicts") == 0) {
        if (append_string(&manifest->conflicts, value) != 0) {
            set_error(error, error_size, "too many or invalid conflicts fields");
            return -1;
        }
    } else if (strcmp(key, "replaces") == 0) {
        if (append_string(&manifest->replaces, value) != 0) {
            set_error(error, error_size, "too many or invalid replaces fields");
            return -1;
        }
    } else {
        set_errorf(error, error_size, "unknown manifest field: %s", key);
        return -1;
    }

    return 0;
}

int pux_package_manifest_read_buffer(const unsigned char *buffer, size_t size,
                                     struct pux_package_manifest *manifest,
                                     char *error, size_t error_size)
{
    if (manifest == NULL || (buffer == NULL && size != 0U)) {
        set_error(error, error_size, "invalid manifest buffer argument");
        return -1;
    }

    pux_package_manifest_init(manifest);

    if (size == 0U || size > PUX_PACKAGE_MAX_MANIFEST_SIZE) {
        set_error(error, error_size, "manifest size is invalid");
        return -1;
    }

    char *text = malloc(size + 1U);
    if (text == NULL) {
        set_error(error, error_size, "out of memory while reading manifest");
        return -1;
    }

    memcpy(text, buffer, size);
    text[size] = '\0';

    char *cursor = text;
    while (*cursor != '\0') {
        char *line_end = strchr(cursor, '\n');
        if (line_end != NULL) {
            *line_end = '\0';
        }

        if (strlen(cursor) >= PUX_PACKAGE_MAX_LINE) {
            set_error(error, error_size, "manifest line is too long");
            free(text);
            pux_package_manifest_free(manifest);
            return -1;
        }

        if (parse_line(manifest, cursor, error, error_size) != 0) {
            free(text);
            pux_package_manifest_free(manifest);
            return -1;
        }

        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }

    free(text);
    return 0;
}

int pux_package_manifest_read_file(const char *path,
                                   struct pux_package_manifest *manifest,
                                   char *error, size_t error_size)
{
    if (path == NULL || manifest == NULL) {
        set_error(error, error_size, "invalid manifest path argument");
        return -1;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_errorf(error, error_size, "cannot open manifest: %s", strerror(errno));
        return -1;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        set_error(error, error_size, "cannot seek manifest");
        return -1;
    }

    const long end = ftell(file);
    if (end < 0L || (unsigned long)end > (unsigned long)PUX_PACKAGE_MAX_MANIFEST_SIZE) {
        fclose(file);
        set_error(error, error_size, "manifest is too large");
        return -1;
    }

    if (fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        set_error(error, error_size, "cannot rewind manifest");
        return -1;
    }

    const size_t size = (size_t)end;
    unsigned char *buffer = malloc(size == 0U ? 1U : size);
    if (buffer == NULL) {
        fclose(file);
        set_error(error, error_size, "out of memory while reading manifest");
        return -1;
    }

    const size_t read_size = fread(buffer, 1U, size, file);
    const int read_failed = ferror(file) != 0;
    fclose(file);

    if (read_failed || read_size != size) {
        free(buffer);
        set_error(error, error_size, "cannot read manifest");
        return -1;
    }

    const int result = pux_package_manifest_read_buffer(buffer, size, manifest,
                                                        error, error_size);
    free(buffer);
    return result;
}

int pux_package_manifest_validate(const struct pux_package_manifest *manifest,
                                  char *error, size_t error_size)
{
    if (manifest == NULL) {
        set_error(error, error_size, "manifest is null");
        return -1;
    }

    if (manifest->format != PUX_PACKAGE_FORMAT) {
        set_error(error, error_size, "unsupported manifest format");
        return -1;
    }
    if (!is_valid_token(manifest->name) || !is_valid_token(manifest->version) ||
        manifest->release == 0U || !is_valid_token(manifest->arch) ||
        !is_valid_scalar_value(manifest->description) ||
        !is_valid_scalar_value(manifest->license)) {
        set_error(error, error_size, "manifest is missing or has invalid required fields");
        return -1;
    }

    for (size_t i = 0U; i < manifest->depends.count; ++i) {
        if (!is_valid_scalar_value(manifest->depends.items[i])) {
            set_error(error, error_size, "invalid dependency expression");
            return -1;
        }
    }

    for (size_t i = 0U; i < manifest->provides.count; ++i) {
        if (!is_valid_scalar_value(manifest->provides.items[i])) {
            set_error(error, error_size, "invalid provides entry");
            return -1;
        }
    }
    for (size_t i = 0U; i < manifest->conflicts.count; ++i) {
        if (!is_valid_scalar_value(manifest->conflicts.items[i])) {
            set_error(error, error_size, "invalid conflicts entry");
            return -1;
        }
    }
    for (size_t i = 0U; i < manifest->replaces.count; ++i) {
        if (!is_valid_scalar_value(manifest->replaces.items[i])) {
            set_error(error, error_size, "invalid replaces entry");
            return -1;
        }
    }

    return 0;
}

static void print_list(const char *key, const struct pux_package_string_list *list)
{
    for (size_t i = 0U; i < list->count; ++i) {
        printf("%s=%s\n", key, list->items[i]);
    }
}

void pux_package_manifest_print(const struct pux_package_manifest *manifest)
{
    printf("format=%u\n", manifest->format);
    printf("name=%s\n", manifest->name);
    printf("version=%s\n", manifest->version);
    printf("release=%u\n", manifest->release);
    printf("arch=%s\n", manifest->arch);
    printf("description=%s\n", manifest->description);
    printf("license=%s\n", manifest->license);
    print_list("depends", &manifest->depends);
    print_list("provides", &manifest->provides);
    print_list("conflicts", &manifest->conflicts);
    print_list("replaces", &manifest->replaces);
}

int pux_package_manifest_write_stream(const struct pux_package_manifest *manifest, FILE *stream)
{
    if (manifest == NULL || stream == NULL) {
        return -1;
    }
    if (pux_package_manifest_validate(manifest, NULL, 0U) != 0) {
        return -1;
    }

    if (fprintf(stream, "format=%u\n", manifest->format) < 0 ||
        fprintf(stream, "name=%s\n", manifest->name) < 0 ||
        fprintf(stream, "version=%s\n", manifest->version) < 0 ||
        fprintf(stream, "release=%u\n", manifest->release) < 0 ||
        fprintf(stream, "arch=%s\n", manifest->arch) < 0 ||
        fprintf(stream, "description=%s\n", manifest->description) < 0 ||
        fprintf(stream, "license=%s\n", manifest->license) < 0) {
        return -1;
    }
    for (size_t i = 0U; i < manifest->depends.count; ++i) {
        if (fprintf(stream, "depends=%s\n", manifest->depends.items[i]) < 0) return -1;
    }
    for (size_t i = 0U; i < manifest->provides.count; ++i) {
        if (fprintf(stream, "provides=%s\n", manifest->provides.items[i]) < 0) return -1;
    }
    for (size_t i = 0U; i < manifest->conflicts.count; ++i) {
        if (fprintf(stream, "conflicts=%s\n", manifest->conflicts.items[i]) < 0) return -1;
    }
    for (size_t i = 0U; i < manifest->replaces.count; ++i) {
        if (fprintf(stream, "replaces=%s\n", manifest->replaces.items[i]) < 0) return -1;
    }
    return 0;
}

