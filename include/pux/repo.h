#ifndef PUX_REPO_H
#define PUX_REPO_H

#include <stddef.h>
#include <stdio.h>

#include "pux/package.h"
#include "pux/sha256.h"

#define PUX_REPO_INDEX_FORMAT 1U
#define PUX_REPO_INDEX_NAME "index.pux"
#define PUX_REPO_MAX_FILENAME 4096U
#define PUX_REPO_MAX_INDEX_SIZE (16U * 1024U * 1024U)
#define PUX_REPO_MAX_PACKAGES 4096U

struct pux_repo_package {
    char *filename;
    struct pux_package_manifest manifest;
    size_t size;
    char sha256[PUX_SHA256_HEX_SIZE];
};

struct pux_repo_catalog {
    struct pux_repo_package *packages;
    size_t count;
};

void pux_repo_catalog_free(struct pux_repo_catalog *catalog);

int pux_repo_create_index(const char *repository_dir,
                          char *error, size_t error_size);

int pux_repo_load_index(const char *repository_dir,
                        struct pux_repo_catalog *catalog,
                        char *error, size_t error_size);

int pux_repo_validate_index(const char *repository_dir,
                            char *error, size_t error_size);

int pux_repo_verify_package(const char *repository_dir,
                            const char *package_path,
                            char *error, size_t error_size);

int pux_repo_search(const char *repository_dir, const char *term,
                    FILE *output, char *error, size_t error_size);

#endif
