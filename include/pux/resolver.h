#ifndef PUX_RESOLVER_H
#define PUX_RESOLVER_H

#include <stddef.h>

#include "pux/package.h"

#define PUX_RESOLVER_MAX_CANDIDATES 4096U
#define PUX_RESOLVER_MAX_STEPS 65536U
#define PUX_RESOLVER_MAX_PATH 4096U

struct pux_resolve_plan {
    char **package_paths;
    size_t count;
};

void pux_resolve_plan_free(struct pux_resolve_plan *plan);

int pux_resolve_package_plan(const char *package_name,
                             const char *repository_dir,
                             struct pux_resolve_plan *plan,
                             char *error, size_t error_size);

int pux_resolve_package(const char *package_name,
                        const char *repository_dir,
                        FILE *output,
                        char *error, size_t error_size);

#endif
