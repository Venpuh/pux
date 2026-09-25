#define _POSIX_C_SOURCE 200809L
#include "pux/update.h"
#include "pux/repo.h"
#include "pux/signature.h"
#include "pux/trust.h"
#include "pux/transport.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <unistd.h>

#define PUX_REMOTE_INDEX_MAX_SIZE (16U * 1024U * 1024U)
#define PUX_REMOTE_SIGNATURE_MAX_SIZE (64U * 1024U)

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0U) (void)snprintf(error, error_size, "%s", message);
}

static void set_errorf(char *error, size_t error_size, const char *fmt, const char *value)
{
    if (error != NULL && error_size > 0U) (void)snprintf(error, error_size, fmt, value);
}

static int join_path(const char *base, const char *name, char *output, size_t output_size)
{
    const size_t base_len = strlen(base);
    const size_t name_len = strlen(name);
    const int sep = base_len > 0U && base[base_len - 1U] != '/';
    if (base_len > SIZE_MAX - name_len - (sep ? 1U : 0U) - 1U) return -1;
    const size_t total = base_len + name_len + (sep ? 1U : 0U);
    if (total + 1U > output_size) return -1;
    memcpy(output, base, base_len);
    size_t offset = base_len;
    if (sep != 0) output[offset++] = '/';
    memcpy(output + offset, name, name_len + 1U);
    return 0;
}

static int make_directory(const char *path, mode_t mode, char *error, size_t error_size)
{
    if (mkdir(path, mode) == 0) return 0;
    if (errno != EEXIST) {
        set_errorf(error, error_size, "cannot create repository directory: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_error(error, error_size, "repository path is not a directory");
        return -1;
    }
    return 0;
}

static int make_temp_directory(const char *repository_dir, char *path, size_t path_size,
                               char *error, size_t error_size)
{
    if (join_path(repository_dir, ".pux-update-XXXXXX", path, path_size) != 0) {
        set_error(error, error_size, "update staging path is too long");
        return -1;
    }
    if (mkdtemp(path) == NULL) {
        set_errorf(error, error_size, "cannot create update staging directory: %s", strerror(errno));
        return -1;
    }
    if (chmod(path, 0700U) != 0) {
        rmdir(path);
        set_error(error, error_size, "cannot protect update staging directory");
        return -1;
    }
    return 0;
}

static void cleanup_stage(const char *stage)
{
    if (stage == NULL || stage[0] == '\0') return;
    char path[PATH_MAX];
    if (join_path(stage, PUX_REPO_INDEX_NAME, path, sizeof(path)) == 0) unlink(path);
    if (join_path(stage, "index.pux.sig", path, sizeof(path)) == 0) unlink(path);
    (void)rmdir(stage);
}

static int build_remote_url(const char *base, const char *name, char *output, size_t output_size)
{
    if (base == NULL || name == NULL) return -1;
    if (strchr(base, '\n') != NULL || strchr(base, '\r') != NULL) return -1;
    const size_t base_len = strlen(base);
    const size_t name_len = strlen(name);
    const int sep = base_len > 0U && base[base_len - 1U] != '/';
    if (base_len > SIZE_MAX - name_len - (sep ? 1U : 0U) - 1U) return -1;
    const size_t total = base_len + name_len + (sep ? 1U : 0U);
    if (total + 1U > output_size) return -1;
    memcpy(output, base, base_len);
    size_t offset = base_len;
    if (sep != 0) output[offset++] = '/';
    memcpy(output + offset, name, name_len + 1U);
    return 0;
}

static int validate_staging_index(const char *stage,
                                  char *error, size_t error_size)
{
    struct pux_repo_catalog catalog = {0};
    if (pux_repo_load_index(stage, &catalog, error, error_size) != 0) return -1;
    pux_repo_catalog_free(&catalog);
    return 0;
}

static int fsync_path(const char *path, char *error, size_t error_size)
{
    const int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        set_errorf(error, error_size, "cannot open repository directory for sync: %s", strerror(errno));
        return -1;
    }
    const int result = fsync(fd);
    const int saved_errno = errno;
    close(fd);
    if (result != 0) {
        set_errorf(error, error_size, "cannot sync repository directory: %s", strerror(saved_errno));
        return -1;
    }
    return 0;
}

static int make_backup_path(const char *repository_dir, const char *suffix,
                            char *path, size_t path_size,
                            char *error, size_t error_size)
{
    char pattern[PATH_MAX];
    if (snprintf(pattern, sizeof(pattern), "%s/.pux-%s-XXXXXX", repository_dir, suffix) < 0) {
        set_error(error, error_size, "backup path is too long");
        return -1;
    }
    const int n = snprintf(path, path_size, "%s", pattern);
    if (n < 0 || (size_t)n >= path_size) {
        set_error(error, error_size, "backup path is too long");
        return -1;
    }
    const int fd = mkstemp(path);
    if (fd < 0) {
        set_errorf(error, error_size, "cannot create repository backup path: %s", strerror(errno));
        return -1;
    }
    close(fd);
    unlink(path);
    return 0;
}

static int replace_metadata(const char *repository_dir,
                            const char *stage,
                            int have_signature,
                            char *error, size_t error_size)
{
    char old_index[PATH_MAX];
    char old_signature[PATH_MAX];
    char stage_index[PATH_MAX];
    char stage_signature[PATH_MAX];
    char backup_index[PATH_MAX] = {0};
    char backup_signature[PATH_MAX] = {0};
    if (join_path(repository_dir, PUX_REPO_INDEX_NAME, old_index, sizeof(old_index)) != 0 ||
        join_path(repository_dir, "index.pux.sig", old_signature, sizeof(old_signature)) != 0 ||
        join_path(stage, PUX_REPO_INDEX_NAME, stage_index, sizeof(stage_index)) != 0 ||
        join_path(stage, "index.pux.sig", stage_signature, sizeof(stage_signature)) != 0) {
        set_error(error, error_size, "metadata path is too long");
        return -1;
    }

    const int old_index_exists = access(old_index, F_OK) == 0;
    const int old_signature_exists = access(old_signature, F_OK) == 0;
    if (old_index_exists != 0) {
        if (make_backup_path(repository_dir, "index-backup", backup_index, sizeof(backup_index), error, error_size) != 0) return -1;
        if (rename(old_index, backup_index) != 0) {
            set_errorf(error, error_size, "cannot stage old repository index: %s", strerror(errno));
            unlink(backup_index);
            return -1;
        }
    }
    if (old_signature_exists != 0) {
        if (make_backup_path(repository_dir, "signature-backup", backup_signature, sizeof(backup_signature), error, error_size) != 0) {
            if (backup_index[0] != '\0') (void)rename(backup_index, old_index);
            return -1;
        }
        if (rename(old_signature, backup_signature) != 0) {
            set_errorf(error, error_size, "cannot stage old repository signature: %s", strerror(errno));
            unlink(backup_signature);
            if (backup_index[0] != '\0') (void)rename(backup_index, old_index);
            return -1;
        }
    }

    int committed_index = 0;
    int committed_signature = 0;
    if (rename(stage_index, old_index) != 0) {
        set_errorf(error, error_size, "cannot install repository index: %s", strerror(errno));
        goto rollback;
    }
    committed_index = 1;

    if (have_signature != 0) {
        if (rename(stage_signature, old_signature) != 0) {
            set_errorf(error, error_size, "cannot install repository signature: %s", strerror(errno));
            goto rollback;
        }
        committed_signature = 1;
    }

    if (fsync_path(repository_dir, error, error_size) != 0) goto rollback;
    if (backup_index[0] != '\0') unlink(backup_index);
    if (backup_signature[0] != '\0') unlink(backup_signature);
    return 0;

rollback:
    if (committed_signature != 0) unlink(old_signature);
    if (committed_index != 0) unlink(old_index);
    if (backup_index[0] != '\0') (void)rename(backup_index, old_index);
    if (backup_signature[0] != '\0') (void)rename(backup_signature, old_signature);
    return -1;
}

int pux_repo_update(const char *repository_url,
                    const char *repository_dir,
                    const char *trusted_keys_root,
                    int require_signed,
                    char *error,
                    size_t error_size)
{
    if (repository_url == NULL || repository_url[0] == '\0' || repository_dir == NULL || repository_dir[0] == '\0') {
        set_error(error, error_size, "repository URL and local repository directory are required");
        return -1;
    }
    if (strncmp(repository_url, "http://", 7U) != 0 && strncmp(repository_url, "https://", 8U) != 0) {
        set_error(error, error_size, "repository URL must use http:// or https://");
        return -1;
    }
    if (make_directory(repository_dir, 0755U, error, error_size) != 0) return -1;

    char stage[PATH_MAX];
    if (make_temp_directory(repository_dir, stage, sizeof(stage), error, error_size) != 0) return -1;

    char index_path[PATH_MAX];
    char signature_path[PATH_MAX];
    if (join_path(stage, PUX_REPO_INDEX_NAME, index_path, sizeof(index_path)) != 0 ||
        join_path(stage, "index.pux.sig", signature_path, sizeof(signature_path)) != 0) {
        cleanup_stage(stage);
        set_error(error, error_size, "update staging path is too long");
        return -1;
    }

    char url[PATH_MAX * 2U];
    long status = 0L;
    if (build_remote_url(repository_url, PUX_REPO_INDEX_NAME, url, sizeof(url)) != 0 ||
        pux_transport_download(url, index_path, PUX_REMOTE_INDEX_MAX_SIZE, &status, error, error_size) != 0) {
        cleanup_stage(stage);
        return -1;
    }
    if (status != 200L) {
        cleanup_stage(stage);
        (void)snprintf(error, error_size, "repository index download returned HTTP %ld", status);
        return -1;
    }

    if (validate_staging_index(stage, error, error_size) != 0) {
        cleanup_stage(stage);
        return -1;
    }

    int have_signature = 0;
    if (build_remote_url(repository_url, "index.pux.sig", url, sizeof(url)) != 0) {
        cleanup_stage(stage);
        set_error(error, error_size, "repository signature URL is too long");
        return -1;
    }
    status = 0L;
    if (pux_transport_download(url, signature_path, PUX_REMOTE_SIGNATURE_MAX_SIZE, &status, error, error_size) != 0) {
        cleanup_stage(stage);
        return -1;
    }
    if (status == 200L) {
        have_signature = 1;
    } else if (status == 404L) {
        unlink(signature_path);
        if (require_signed != 0) {
            cleanup_stage(stage);
            set_error(error, error_size, "repository is unsigned but signed repositories are required");
            return -1;
        }
    } else {
        cleanup_stage(stage);
        (void)snprintf(error, error_size, "repository signature download returned HTTP %ld", status);
        return -1;
    }

    if (have_signature != 0 && require_signed != 0) {
        char keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
        if (pux_trust_verify_repository(trusted_keys_root, stage, keyid, error, error_size) != 0) {
            cleanup_stage(stage);
            return -1;
        }
    }

    if (replace_metadata(repository_dir, stage, have_signature, error, error_size) != 0) {
        cleanup_stage(stage);
        return -1;
    }
    cleanup_stage(stage);
    return 0;
}
