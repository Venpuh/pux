#ifndef PUX_DB_H
#define PUX_DB_H

#include <stddef.h>
#include <stdio.h>

#include "pux/package.h"

#define PUX_DB_DEFAULT_ROOT "/var/lib/pux/packages"
#define PUX_DB_MAX_FILE_LIST 4096U
#define PUX_DB_MAX_RECORD_SIZE (16U * 1024U * 1024U)
#define PUX_DB_RECORD_HEADER "# pux-record=1"
#define PUX_DB_FILES_MARKER "# files"

struct pux_db_file_entry {
    char type;
    char *path;
    char *target;
};

struct pux_db_file_list {
    struct pux_db_file_entry *items;
    size_t count;
};

void pux_db_file_list_free(struct pux_db_file_list *files);

int pux_db_read_file_list(const char *path,
                          struct pux_db_file_list *files,
                          char *error, size_t error_size);

int pux_db_register_package(const char *db_root,
                            const struct pux_package_manifest *manifest,
                            const struct pux_db_file_list *files,
                            char *error, size_t error_size);

int pux_db_read_package(const char *db_root, const char *name,
                        struct pux_package_manifest *manifest,
                        struct pux_db_file_list *files,
                        char *error, size_t error_size);

int pux_db_unregister_package(const char *db_root, const char *name,
                              char *error, size_t error_size);

int pux_db_list_packages(const char *db_root, FILE *output,
                         char *error, size_t error_size);

int pux_db_dependency_satisfied(const char *db_root, const char *expression,
                                int *satisfied,
                                char *error, size_t error_size);

int pux_db_find_reverse_dependency(const char *db_root,
                                   const struct pux_package_manifest *target,
                                   char *dependent, size_t dependent_size,
                                   int *found,
                                   char *error, size_t error_size);

int pux_db_find_owner(const char *db_root, const char *path,
                      char *owner, size_t owner_size, int *owned,
                      char *error, size_t error_size);

int pux_db_find_other_owner(const char *db_root, const char *path,
                            const char *excluded_name,
                            char *owner, size_t owner_size, int *owned,
                            char *error, size_t error_size);

#endif
