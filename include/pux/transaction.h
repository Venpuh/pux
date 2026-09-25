#ifndef PUX_TRANSACTION_H
#define PUX_TRANSACTION_H

#include <stddef.h>

#define PUX_ROOT_DEFAULT "/"
#define PUX_TXN_MAX_PATH 4096U
#define PUX_TXN_MAX_FILES 4096U
#define PUX_TXN_MAX_STAGING_TEMPLATE 4096U

int pux_install_package(const char *package_path,
                        const char *root, const char *db_root,
                        char *error, size_t error_size);

int pux_remove_package(const char *name,
                       const char *root, const char *db_root,
                       char *error, size_t error_size);

int pux_upgrade_package(const char *package_path,
                        const char *root, const char *db_root,
                        char *error, size_t error_size);

#endif
