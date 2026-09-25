#ifndef PUX_PACKAGE_H
#define PUX_PACKAGE_H

#include <stddef.h>
#include <stdio.h>

#define PUX_PACKAGE_FORMAT 1U
#define PUX_PACKAGE_MAX_LINE 4096U
#define PUX_PACKAGE_MAX_LIST_ITEMS 256U
#define PUX_PACKAGE_MAX_MANIFEST_SIZE (1024U * 1024U)

struct pux_package_string_list {
    char **items;
    size_t count;
};

struct pux_package_manifest {
    unsigned format;
    char *name;
    char *version;
    unsigned release;
    char *arch;
    char *description;
    char *license;
    struct pux_package_string_list depends;
    struct pux_package_string_list provides;
    struct pux_package_string_list conflicts;
    struct pux_package_string_list replaces;
};

void pux_package_manifest_init(struct pux_package_manifest *manifest);
void pux_package_manifest_free(struct pux_package_manifest *manifest);
int pux_package_manifest_read_buffer(const unsigned char *buffer, size_t size,
                                     struct pux_package_manifest *manifest,
                                     char *error, size_t error_size);
int pux_package_manifest_read_file(const char *path,
                                   struct pux_package_manifest *manifest,
                                   char *error, size_t error_size);
int pux_package_manifest_validate(const struct pux_package_manifest *manifest,
                                  char *error, size_t error_size);
void pux_package_manifest_print(const struct pux_package_manifest *manifest);
int pux_package_manifest_write_stream(const struct pux_package_manifest *manifest,
                                      FILE *stream);

#endif
