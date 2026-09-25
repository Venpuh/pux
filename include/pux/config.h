#ifndef PUX_CONFIG_H
#define PUX_CONFIG_H

#include <stddef.h>
#include <stdio.h>

#define PUX_CONFIG_DEFAULT_ROOT "/etc/pux/repos.d"
#define PUX_CACHE_DEFAULT_ROOT "/var/cache/pux/repos"
#define PUX_CONFIG_MAX_NAME 64U
#define PUX_CONFIG_MAX_PRIORITY 1000000
#define PUX_CONFIG_MAX_REPOSITORIES 128U
#define PUX_CONFIG_MAX_URL 4096U

struct pux_repo_config_entry {
    char *name;
    char *url;
    char *cache_dir;
    int priority;
    int enabled;
    int require_signature;
};

struct pux_repo_config_list {
    struct pux_repo_config_entry *items;
    size_t count;
};

const char *pux_repo_config_root(void);
const char *pux_repo_cache_root(void);

void pux_repo_config_list_free(struct pux_repo_config_list *list);

int pux_repo_config_name_valid(const char *name);

int pux_repo_config_load_all(const char *config_root,
                             const char *cache_root,
                             struct pux_repo_config_list *list,
                             char *error, size_t error_size);

int pux_repo_config_add(const char *config_root, const char *cache_root,
                        const char *name, const char *url, int priority,
                        int enabled, int require_signature,
                        char *error, size_t error_size);

int pux_repo_config_remove(const char *config_root, const char *name,
                           char *error, size_t error_size);

int pux_repo_config_print(const struct pux_repo_config_list *list,
                          FILE *output);

#endif
