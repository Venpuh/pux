#define _POSIX_C_SOURCE 200809L
#include "pux/trust.h"
#include "pux/repo.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PUX_TRUST_MAX_KEYS 10000U
#define PUX_TRUST_KEY_NAME_SUFFIX ".pub"
#define PUX_TRUST_KEY_FILE_MAX 4096U

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
}

static void set_errorf(char *error, size_t error_size, const char *fmt, const char *value)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, fmt, value);
}

static int is_hex_keyid(const char *keyid)
{
    if (keyid == NULL || strlen(keyid) != 64U) return 0;
    for (size_t i = 0U; i < 64U; ++i) {
        const char c = keyid[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

static int make_dir(const char *path, mode_t mode, char *error, size_t error_size)
{
    if (mkdir(path, mode) == 0) return 0;
    if (errno != EEXIST) {
        set_errorf(error, error_size, "cannot create trusted-key directory: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_error(error, error_size, "trusted-key root is not a directory");
        return -1;
    }
    return 0;
}

static int ensure_root(const char *root, char *error, size_t error_size)
{
    if (root == NULL || root[0] == '\0') {
        set_error(error, error_size, "trusted-key root is empty");
        return -1;
    }
    char parent[PATH_MAX];
    const size_t length = strlen(root);
    if (length >= sizeof(parent)) {
        set_error(error, error_size, "trusted-key root path is too long");
        return -1;
    }
    memcpy(parent, root, length + 1U);
    char *slash = strrchr(parent, '/');
    if (slash != NULL && slash != parent) {
        *slash = '\0';
        if (access(parent, F_OK) != 0) {
            if (mkdir(parent, 0755U) != 0 && errno != EEXIST) {
                set_errorf(error, error_size, "cannot create parent directory: %s", strerror(errno));
                return -1;
            }
        }
    }
    return make_dir(root, 0755U, error, error_size);
}

static int build_key_path(const char *root, const char *keyid, char *path, size_t path_size)
{
    if (!is_hex_keyid(keyid)) return -1;
    const int n = snprintf(path, path_size, "%s/%s.pub", root, keyid);
    return n < 0 || (size_t)n >= path_size ? -1 : 0;
}

static int read_file(const char *path, unsigned char **data, size_t *size,
                     char *error, size_t error_size)
{
    *data = NULL;
    *size = 0U;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_errorf(error, error_size, "cannot open public key: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || st.st_size < 0L || (unsigned long long)st.st_size > PUX_TRUST_KEY_FILE_MAX) {
        fclose(file);
        set_error(error, error_size, "public key file is invalid or too large");
        return -1;
    }
    const size_t length = (size_t)st.st_size;
    unsigned char *buffer = malloc(length == 0U ? 1U : length);
    if (buffer == NULL) {
        fclose(file);
        set_error(error, error_size, "out of memory while reading public key");
        return -1;
    }
    if (length > 0U && fread(buffer, 1U, length, file) != length) {
        free(buffer);
        fclose(file);
        set_error(error, error_size, "cannot read public key");
        return -1;
    }
    if (fclose(file) != 0) {
        free(buffer);
        set_error(error, error_size, "cannot close public key");
        return -1;
    }
    *data = buffer;
    *size = length;
    return 0;
}

int pux_trust_add_key(const char *trusted_root,
                      const char *public_key_path,
                      char output_keyid[PUX_SIGNATURE_KEYID_HEX_SIZE],
                      char *error, size_t error_size)
{
    if (ensure_root(trusted_root, error, error_size) != 0) return -1;
    char keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
    if (pux_signature_keyid(public_key_path, keyid, error, error_size) != 0) return -1;

    unsigned char *data = NULL;
    size_t size = 0U;
    if (read_file(public_key_path, &data, &size, error, error_size) != 0) return -1;

    char destination[PATH_MAX];
    if (build_key_path(trusted_root, keyid, destination, sizeof(destination)) != 0) {
        free(data);
        set_error(error, error_size, "trusted-key destination path is too long");
        return -1;
    }
    int fd = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644U);
    if (fd < 0) {
        const int saved_errno = errno;
        free(data);
        if (saved_errno == EEXIST) set_error(error, error_size, "key is already trusted");
        else set_errorf(error, error_size, "cannot install trusted key: %s", strerror(saved_errno));
        return -1;
    }
    size_t offset = 0U;
    int ok = 1;
    while (offset < size) {
        const ssize_t written = write(fd, data + offset, size - offset);
        if (written <= 0) { ok = 0; break; }
        offset += (size_t)written;
    }
    if (fchmod(fd, 0644U) != 0) ok = 0;
    if (close(fd) != 0) ok = 0;
    if (ok == 0) {
        unlink(destination);
        free(data);
        set_error(error, error_size, "cannot write trusted key");
        return -1;
    }
    free(data);
    if (output_keyid != NULL) memcpy(output_keyid, keyid, sizeof(keyid));
    return 0;
}

int pux_trust_remove_key(const char *trusted_root,
                         const char *keyid,
                         char *error, size_t error_size)
{
    if (!is_hex_keyid(keyid)) {
        set_error(error, error_size, "keyid must be 64 lowercase hexadecimal characters");
        return -1;
    }
    char path[PATH_MAX];
    if (build_key_path(trusted_root, keyid, path, sizeof(path)) != 0) {
        set_error(error, error_size, "trusted-key path is too long");
        return -1;
    }
    if (unlink(path) != 0) {
        set_errorf(error, error_size, "cannot remove trusted key: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int name_compare(const void *left, const void *right)
{
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

int pux_trust_list_keys(const char *trusted_root,
                       FILE *output,
                       char *error, size_t error_size)
{
    if (output == NULL) {
        set_error(error, error_size, "trusted-key output stream is required");
        return -1;
    }
    DIR *dir = opendir(trusted_root);
    if (dir == NULL) {
        if (errno == ENOENT) return 0;
        set_errorf(error, error_size, "cannot open trusted-key directory: %s", strerror(errno));
        return -1;
    }
    char **names = NULL;
    size_t count = 0U;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const size_t len = strlen(entry->d_name);
        if (len != 68U || strcmp(entry->d_name + 64U, ".pub") != 0) continue;
        char keyid_check[65];
        memcpy(keyid_check, entry->d_name, 64U);
        keyid_check[64] = '\0';
        if (!is_hex_keyid(keyid_check)) continue;
        if (count >= PUX_TRUST_MAX_KEYS) {
            closedir(dir);
            for (size_t i = 0U; i < count; ++i) free(names[i]);
            free(names);
            set_error(error, error_size, "too many trusted keys");
            return -1;
        }
        char **next = realloc(names, (count + 1U) * sizeof(*next));
        if (next == NULL) {
            closedir(dir);
            for (size_t i = 0U; i < count; ++i) free(names[i]);
            free(names);
            set_error(error, error_size, "out of memory while listing trusted keys");
            return -1;
        }
        names = next;
        names[count] = strdup(entry->d_name);
        if (names[count] == NULL) {
            closedir(dir);
            for (size_t i = 0U; i < count; ++i) free(names[i]);
            free(names);
            set_error(error, error_size, "out of memory while listing trusted keys");
            return -1;
        }
        count++;
    }
    if (closedir(dir) != 0) {
        for (size_t i = 0U; i < count; ++i) free(names[i]);
        free(names);
        set_error(error, error_size, "cannot close trusted-key directory");
        return -1;
    }
    qsort(names, count, sizeof(*names), name_compare);
    for (size_t i = 0U; i < count; ++i) {
        names[i][64] = '\0';
        if (fprintf(output, "%s\n", names[i]) < 0) {
            for (size_t j = 0U; j < count; ++j) free(names[j]);
            free(names);
            set_error(error, error_size, "cannot write trusted-key list");
            return -1;
        }
    }
    for (size_t i = 0U; i < count; ++i) free(names[i]);
    free(names);
    return 0;
}

int pux_trust_verify_repository(const char *trusted_root,
                                const char *repository_dir,
                                char output_keyid[PUX_SIGNATURE_KEYID_HEX_SIZE],
                                char *error, size_t error_size)
{
    if (trusted_root == NULL || repository_dir == NULL) {
        set_error(error, error_size, "trusted-key root and repository are required");
        return -1;
    }
    char signature_path[PATH_MAX];
    int n = snprintf(signature_path, sizeof(signature_path), "%s/%s.sig", repository_dir, PUX_REPO_INDEX_NAME);
    if (n < 0 || (size_t)n >= sizeof(signature_path)) {
        set_error(error, error_size, "repository signature path is too long");
        return -1;
    }
    char index_path[PATH_MAX];
    n = snprintf(index_path, sizeof(index_path), "%s/%s", repository_dir, PUX_REPO_INDEX_NAME);
    if (n < 0 || (size_t)n >= sizeof(index_path)) {
        set_error(error, error_size, "repository index path is too long");
        return -1;
    }
    char keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
    if (pux_signature_file_keyid(signature_path, keyid, error, error_size) != 0) return -1;
    if (!is_hex_keyid(keyid)) {
        set_error(error, error_size, "repository signature contains invalid keyid");
        return -1;
    }
    char public_path[PATH_MAX];
    if (build_key_path(trusted_root, keyid, public_path, sizeof(public_path)) != 0) {
        set_error(error, error_size, "trusted-key path is too long");
        return -1;
    }
    if (access(public_path, R_OK) != 0) {
        set_error(error, error_size, "repository signing key is not trusted");
        return -1;
    }
    if (pux_signature_verify_file(index_path, signature_path, public_path, error, error_size) != 0) return -1;
    if (output_keyid != NULL) memcpy(output_keyid, keyid, sizeof(keyid));
    return 0;
}
