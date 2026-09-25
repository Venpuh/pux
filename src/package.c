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
    unsigned long parsed = strtoul(value, &end, 10);

    if (errno != 0 || end == value || *end != '\0' || parsed > UINT_MAX) {
        return -1;
    }

    *result = (unsigned)parsed;
    return 0;
}

static char *trim_newline(char *line)
{
    const size_t length = strlen(line);
    if (length > 0U && line[length - 1U] == '\n') {
        line[length - 1U] = '\0';
    }
    return line;
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
    char *equals = strchr(line, '=');
    if (equals == NULL || equals == line) {
        set_error(error, error_size, "invalid manifest record; expected key=value");
        return -1;
    }

    *equals = '\0';
    const char *key = line;
    const char *value = equals + 1;

    if (key[0] == '\0' || !is_valid_token(key)) {
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

int pux_package_manifest_read_file(const char *path,
                                   struct pux_package_manifest *manifest,
                                   char *error, size_t error_size)
{
    if (path == NULL || manifest == NULL) {
        set_error(error, error_size, "invalid parser arguments");
        return -1;
    }

    pux_package_manifest_init(manifest);

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (error != NULL && error_size > 0U) {
            (void)snprintf(error, error_size, "cannot open '%s': %s", path, strerror(errno));
        }
        return -1;
    }

    char line[PUX_PACKAGE_MAX_LINE];
    unsigned line_number = 0U;
    int result = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        ++line_number;

        if (strchr(line, '\n') == NULL && !feof(file)) {
            if (error != NULL && error_size > 0U) {
                (void)snprintf(error, error_size, "manifest line %u is too long", line_number);
            }
            result = -1;
            break;
        }

        trim_newline(line);

        char *cursor = line;
        while (isspace((unsigned char)*cursor) != 0) {
            ++cursor;
        }

        if (*cursor == '\0' || *cursor == '#') {
            continue;
        }

        if (parse_line(manifest, cursor, error, error_size) != 0) {
            if (error == NULL || error_size == 0U || error[0] == '\0') {
                break;
            }
            size_t used = strlen(error);
            if (used + 32U < error_size) {
                (void)snprintf(error + used, error_size - used, " (line %u)", line_number);
            }
            result = -1;
            break;
        }
    }

    if (ferror(file) != 0) {
        set_error(error, error_size, "error while reading manifest");
        result = -1;
    }

    if (fclose(file) != 0 && result == 0) {
        set_error(error, error_size, "failed to close manifest file");
        result = -1;
    }

    if (result != 0) {
        pux_package_manifest_free(manifest);
    }

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
        if (error != NULL && error_size > 0U) {
            (void)snprintf(error, error_size, "unsupported manifest format: %u", manifest->format);
        }
        return -1;
    }

    if (manifest->name == NULL || !is_valid_token(manifest->name)) {
        set_error(error, error_size, "manifest name is missing or invalid");
        return -1;
    }

    if (manifest->version == NULL || !is_valid_token(manifest->version)) {
        set_error(error, error_size, "manifest version is missing or invalid");
        return -1;
    }

    if (manifest->release == 0U) {
        set_error(error, error_size, "manifest release is missing or invalid");
        return -1;
    }

    if (manifest->arch == NULL || !is_valid_token(manifest->arch)) {
        set_error(error, error_size, "manifest arch is missing or invalid");
        return -1;
    }

    if (manifest->description == NULL || manifest->description[0] == '\0') {
        set_error(error, error_size, "manifest description is missing");
        return -1;
    }

    if (manifest->license == NULL || manifest->license[0] == '\0') {
        set_error(error, error_size, "manifest license is missing");
        return -1;
    }

    return 0;
}

static void print_list(const char *label, const struct pux_package_string_list *list)
{
    for (size_t i = 0U; i < list->count; ++i) {
        printf("%s=%s\n", label, list->items[i]);
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
