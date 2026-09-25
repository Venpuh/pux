#define _POSIX_C_SOURCE 200809L
#include "pux/signature.h"
#include "pux/sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PUX_SIGNATURE_MAX_INPUT (16U * 1024U * 1024U)
#define PUX_SIGNATURE_MAX_LINE 512U

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

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode_exact(const char *input, unsigned char *output, size_t output_size)
{
    if (strlen(input) != output_size * 2U) return -1;
    for (size_t i = 0U; i < output_size; ++i) {
        const int hi = hex_value(input[i * 2U]);
        const int lo = hex_value(input[i * 2U + 1U]);
        if (hi < 0 || lo < 0) return -1;
        output[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

static void hex_encode(const unsigned char *input, size_t input_size, char *output)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0U; i < input_size; ++i) {
        output[i * 2U] = digits[input[i] >> 4U];
        output[i * 2U + 1U] = digits[input[i] & 0x0FU];
    }
    output[input_size * 2U] = '\0';
}

static int read_text_file(const char *path, char **buffer, size_t *size,
                          size_t max_size, char *error, size_t error_size)
{
    *buffer = NULL;
    *size = 0U;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_errorf(error, error_size, "cannot open file: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || st.st_size < 0L || (unsigned long long)st.st_size > max_size) {
        fclose(file);
        set_error(error, error_size, "file is missing, invalid, or too large");
        return -1;
    }
    const size_t length = (size_t)st.st_size;
    char *data = malloc(length + 1U);
    if (data == NULL) {
        fclose(file);
        set_error(error, error_size, "out of memory while reading file");
        return -1;
    }
    if (length > 0U && fread(data, 1U, length, file) != length) {
        free(data);
        fclose(file);
        set_error(error, error_size, "cannot read file");
        return -1;
    }
    fclose(file);
    data[length] = '\0';
    *buffer = data;
    *size = length;
    return 0;
}

static int read_binary_file(const char *path, unsigned char **buffer, size_t *size,
                            size_t max_size, char *error, size_t error_size)
{
    *buffer = NULL;
    *size = 0U;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_errorf(error, error_size, "cannot open file: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || st.st_size < 0L || (unsigned long long)st.st_size > max_size) {
        fclose(file);
        set_error(error, error_size, "file is missing, invalid, or too large");
        return -1;
    }
    const size_t length = (size_t)st.st_size;
    unsigned char *data = malloc(length == 0U ? 1U : length);
    if (data == NULL) {
        fclose(file);
        set_error(error, error_size, "out of memory while reading file");
        return -1;
    }
    if (length > 0U && fread(data, 1U, length, file) != length) {
        free(data);
        fclose(file);
        set_error(error, error_size, "cannot read file");
        return -1;
    }
    fclose(file);
    *buffer = data;
    *size = length;
    return 0;
}

static int parse_prefixed_hex_line(const char *text, const char *prefix,
                                   char *output, size_t output_size)
{
    const size_t prefix_len = strlen(prefix);
    if (strncmp(text, prefix, prefix_len) != 0) return -1;
    const char *value = text + prefix_len;
    if (strlen(value) + 1U > output_size) return -1;
    memcpy(output, value, strlen(value) + 1U);
    return 0;
}


static int ensure_parent_directory(const char *path, char *error, size_t error_size)
{
    char parent[4096];
    const char *slash = strrchr(path, '/');
    if (slash == NULL) return 0;
    const size_t length = (size_t)(slash - path);
    if (length == 0U) return 0;
    if (length + 1U > sizeof(parent)) {
        set_error(error, error_size, "key path is too long");
        return -1;
    }
    memcpy(parent, path, length);
    parent[length] = '\0';
    if (mkdir(parent, 0700U) != 0 && errno != EEXIST) {
        set_errorf(error, error_size, "cannot create key directory: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (stat(parent, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_error(error, error_size, "key parent path is not a directory");
        return -1;
    }
    return 0;
}

static int write_key_file(const char *path, const char *header,
                          const char *keyid, const char *public_hex,
                          const char *seed_hex, int private_key,
                          char *error, size_t error_size)
{
    const mode_t mode = private_key ? 0600U : 0644U;
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) {
        set_errorf(error, error_size, "cannot create key file: %s", strerror(errno));
        return -1;
    }
    FILE *file = fdopen(fd, "wb");
    if (file == NULL) {
        const int saved_errno = errno;
        close(fd);
        unlink(path);
        set_errorf(error, error_size, "cannot open key file: %s", strerror(saved_errno));
        return -1;
    }

    int ok = 1;
    if (fprintf(file, "%s\nalgorithm=%s\nkeyid=%s\npublic=%s\n",
                header, PUX_SIGNATURE_ALGORITHM, keyid, public_hex) < 0) ok = 0;
    if (private_key != 0 && fprintf(file, "seed=%s\n", seed_hex) < 0) ok = 0;
    if (fflush(file) != 0 || fclose(file) != 0) ok = 0;
    if (ok == 0) {
        unlink(path);
        set_error(error, error_size, "cannot write key file");
        return -1;
    }
    return 0;
}

static int load_private_key(const char *path, EVP_PKEY **pkey,
                            char *error, size_t error_size)
{
    *pkey = NULL;
    char *data = NULL;
    size_t size = 0U;
    if (read_text_file(path, &data, &size, 4096U, error, error_size) != 0) return -1;

    if (strstr(data, "# pux-ed25519-private=1\n") != data) {
        free(data);
        set_error(error, error_size, "invalid private key header");
        return -1;
    }
    char seed_hex[PUX_SIGNATURE_HEX_SIZE] = {0};
    char public_hex[PUX_SIGNATURE_HEX_SIZE] = {0};
    char keyid[PUX_SIGNATURE_KEYID_HEX_SIZE] = {0};
    int have_seed = 0;
    int have_public = 0;
    int have_keyid = 0;
    int have_algorithm = 0;
    char algorithm[64] = {0};
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        if (parse_prefixed_hex_line(line, "seed=", seed_hex, sizeof(seed_hex)) == 0) have_seed = 1;
        else if (parse_prefixed_hex_line(line, "public=", public_hex, sizeof(public_hex)) == 0) have_public = 1;
        else if (parse_prefixed_hex_line(line, "keyid=", keyid, sizeof(keyid)) == 0) have_keyid = 1;
        else if (parse_prefixed_hex_line(line, "algorithm=", algorithm, sizeof(algorithm)) == 0) have_algorithm = 1;
    }
    unsigned char seed[PUX_SIGNATURE_PRIVATE_KEY_SIZE];
    unsigned char expected_public[PUX_SIGNATURE_PUBLIC_KEY_SIZE];
    if (have_seed == 0 || have_public == 0 || have_keyid == 0 || have_algorithm == 0 ||
        strcmp(algorithm, PUX_SIGNATURE_ALGORITHM) != 0 ||
        hex_decode_exact(seed_hex, seed, sizeof(seed)) != 0 ||
        hex_decode_exact(public_hex, expected_public, sizeof(expected_public)) != 0 ||
        strlen(keyid) != 64U) {
        free(data);
        set_error(error, error_size, "invalid private key fields");
        return -1;
    }

    EVP_PKEY *candidate = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL,
                                                        seed, sizeof(seed));
    if (candidate == NULL) {
        free(data);
        set_error(error, error_size, "cannot load Ed25519 private key");
        return -1;
    }
    unsigned char actual_public[PUX_SIGNATURE_PUBLIC_KEY_SIZE];
    size_t actual_len = sizeof(actual_public);
    if (EVP_PKEY_get_raw_public_key(candidate, actual_public, &actual_len) != 1 ||
        actual_len != sizeof(actual_public) || memcmp(actual_public, expected_public, sizeof(actual_public)) != 0) {
        EVP_PKEY_free(candidate);
        free(data);
        set_error(error, error_size, "private key public component mismatch");
        return -1;
    }
    char actual_keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
    struct pux_sha256_ctx sha;
    unsigned char digest[32];
    pux_sha256_init(&sha);
    pux_sha256_update(&sha, actual_public, sizeof(actual_public));
    pux_sha256_final(&sha, digest);
    pux_sha256_hex(digest, actual_keyid);
    if (strncmp(actual_keyid, keyid, 64U) != 0) {
        EVP_PKEY_free(candidate);
        free(data);
        set_error(error, error_size, "private key keyid mismatch");
        return -1;
    }
    free(data);
    *pkey = candidate;
    return 0;
}

static int load_public_key(const char *path, unsigned char public_key[PUX_SIGNATURE_PUBLIC_KEY_SIZE],
                           char expected_keyid[PUX_SIGNATURE_KEYID_HEX_SIZE],
                           char *error, size_t error_size)
{
    char *data = NULL;
    size_t size = 0U;
    if (read_text_file(path, &data, &size, 4096U, error, error_size) != 0) return -1;
    if (strstr(data, "# pux-ed25519-public=1\n") != data) {
        free(data);
        set_error(error, error_size, "invalid public key header");
        return -1;
    }
    char public_hex[PUX_SIGNATURE_HEX_SIZE] = {0};
    int have_public = 0;
    int have_keyid = 0;
    int have_algorithm = 0;
    char algorithm[64] = {0};
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        if (parse_prefixed_hex_line(line, "public=", public_hex, sizeof(public_hex)) == 0) have_public = 1;
        else if (parse_prefixed_hex_line(line, "keyid=", expected_keyid, PUX_SIGNATURE_KEYID_HEX_SIZE) == 0) have_keyid = 1;
        else if (parse_prefixed_hex_line(line, "algorithm=", algorithm, sizeof(algorithm)) == 0) have_algorithm = 1;
    }
    if (have_public == 0 || have_keyid == 0 || have_algorithm == 0 ||
        strcmp(algorithm, PUX_SIGNATURE_ALGORITHM) != 0 || strlen(expected_keyid) != 64U ||
        hex_decode_exact(public_hex, public_key, PUX_SIGNATURE_PUBLIC_KEY_SIZE) != 0) {
        free(data);
        set_error(error, error_size, "invalid public key fields");
        return -1;
    }
    struct pux_sha256_ctx sha;
    unsigned char digest[32];
    char actual_keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
    pux_sha256_init(&sha);
    pux_sha256_update(&sha, public_key, PUX_SIGNATURE_PUBLIC_KEY_SIZE);
    pux_sha256_final(&sha, digest);
    pux_sha256_hex(digest, actual_keyid);
    if (strncmp(actual_keyid, expected_keyid, 64U) != 0) {
        free(data);
        set_error(error, error_size, "public key keyid mismatch");
        return -1;
    }
    free(data);
    return 0;
}

static int load_signature(const char *path, unsigned char signature[PUX_SIGNATURE_BYTES],
                          char keyid[PUX_SIGNATURE_KEYID_HEX_SIZE],
                          char *error, size_t error_size)
{
    char *data = NULL;
    size_t size = 0U;
    if (read_text_file(path, &data, &size, 4096U, error, error_size) != 0) return -1;
    if (strstr(data, "# pux-ed25519-signature=1\n") != data) {
        free(data);
        set_error(error, error_size, "invalid signature header");
        return -1;
    }
    char signature_hex[PUX_SIGNATURE_HEX_SIZE] = {0};
    int have_signature = 0;
    int have_keyid = 0;
    int have_algorithm = 0;
    char algorithm[64] = {0};
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        if (parse_prefixed_hex_line(line, "signature=", signature_hex, sizeof(signature_hex)) == 0) have_signature = 1;
        else if (parse_prefixed_hex_line(line, "keyid=", keyid, PUX_SIGNATURE_KEYID_HEX_SIZE) == 0) have_keyid = 1;
        else if (parse_prefixed_hex_line(line, "algorithm=", algorithm, sizeof(algorithm)) == 0) have_algorithm = 1;
    }
    if (have_signature == 0 || have_keyid == 0 || have_algorithm == 0 ||
        strcmp(algorithm, PUX_SIGNATURE_ALGORITHM) != 0 || strlen(keyid) != 64U ||
        hex_decode_exact(signature_hex, signature, PUX_SIGNATURE_BYTES) != 0) {
        free(data);
        set_error(error, error_size, "invalid signature fields");
        return -1;
    }
    free(data);
    return 0;
}

int pux_signature_keygen(const char *private_key_path,
                         const char *public_key_path,
                         char *error, size_t error_size)
{
    if (private_key_path == NULL || public_key_path == NULL || strcmp(private_key_path, public_key_path) == 0) {
        set_error(error, error_size, "private and public key paths must be different");
        return -1;
    }
    if (ensure_parent_directory(private_key_path, error, error_size) != 0 ||
        ensure_parent_directory(public_key_path, error, error_size) != 0) {
        return -1;
    }
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    EVP_PKEY *pkey = NULL;
    if (ctx == NULL || EVP_PKEY_keygen_init(ctx) != 1 || EVP_PKEY_keygen(ctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        set_error(error, error_size, "cannot generate Ed25519 keypair");
        return -1;
    }
    unsigned char seed[PUX_SIGNATURE_PRIVATE_KEY_SIZE];
    unsigned char public_key[PUX_SIGNATURE_PUBLIC_KEY_SIZE];
    size_t seed_len = sizeof(seed);
    size_t public_len = sizeof(public_key);
    int result = EVP_PKEY_get_raw_private_key(pkey, seed, &seed_len) == 1 &&
                 EVP_PKEY_get_raw_public_key(pkey, public_key, &public_len) == 1 &&
                 seed_len == sizeof(seed) && public_len == sizeof(public_key);
    if (result == 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        set_error(error, error_size, "cannot export Ed25519 keypair");
        return -1;
    }
    struct pux_sha256_ctx sha;
    unsigned char digest[32];
    char keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
    char public_hex[PUX_SIGNATURE_HEX_SIZE];
    char seed_hex[PUX_SIGNATURE_HEX_SIZE];
    pux_sha256_init(&sha);
    pux_sha256_update(&sha, public_key, sizeof(public_key));
    pux_sha256_final(&sha, digest);
    pux_sha256_hex(digest, keyid);
    hex_encode(public_key, sizeof(public_key), public_hex);
    hex_encode(seed, sizeof(seed), seed_hex);

    const int write_result =
        write_key_file(private_key_path, "# pux-ed25519-private=1", keyid, public_hex, seed_hex, 1,
                       error, error_size) == 0 &&
        write_key_file(public_key_path, "# pux-ed25519-public=1", keyid, public_hex, NULL, 0,
                       error, error_size) == 0;
    if (write_result == 0) {
        unlink(private_key_path);
        unlink(public_key_path);
        OPENSSL_cleanse(seed, sizeof(seed));
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return -1;
    }
    OPENSSL_cleanse(seed, sizeof(seed));
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return 0;
}

int pux_signature_keyid(const char *public_key_path,
                        char output[PUX_SIGNATURE_KEYID_HEX_SIZE],
                        char *error, size_t error_size)
{
    unsigned char public_key[PUX_SIGNATURE_PUBLIC_KEY_SIZE];
    char keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
    if (load_public_key(public_key_path, public_key, keyid, error, error_size) != 0) return -1;
    memcpy(output, keyid, sizeof(keyid));
    return 0;
}

static int read_signature_input(const char *path, unsigned char **data, size_t *size,
                                char *error, size_t error_size)
{
    return read_binary_file(path, data, size, PUX_SIGNATURE_MAX_INPUT, error, error_size);
}

int pux_signature_sign_file(const char *input_path,
                            const char *private_key_path,
                            const char *signature_path,
                            char *error, size_t error_size)
{
    unsigned char *input = NULL;
    size_t input_size = 0U;
    if (read_signature_input(input_path, &input, &input_size, error, error_size) != 0) return -1;
    EVP_PKEY *pkey = NULL;
    if (load_private_key(private_key_path, &pkey, error, error_size) != 0) {
        free(input);
        return -1;
    }
    unsigned char signature[PUX_SIGNATURE_BYTES];
    size_t signature_len = sizeof(signature);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int result = ctx != NULL &&
                 EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey) == 1 &&
                 EVP_DigestSign(ctx, signature, &signature_len, input, input_size) == 1 &&
                 signature_len == sizeof(signature);
    EVP_MD_CTX_free(ctx);
    if (result == 0) {
        EVP_PKEY_free(pkey);
        free(input);
        set_error(error, error_size, "cannot sign repository index with Ed25519");
        return -1;
    }

    unsigned char public_key[PUX_SIGNATURE_PUBLIC_KEY_SIZE];
    size_t public_len = sizeof(public_key);
    if (EVP_PKEY_get_raw_public_key(pkey, public_key, &public_len) != 1 || public_len != sizeof(public_key)) {
        EVP_PKEY_free(pkey);
        free(input);
        set_error(error, error_size, "cannot read signing public key");
        return -1;
    }
    struct pux_sha256_ctx sha;
    unsigned char digest[32];
    char keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
    char signature_hex[PUX_SIGNATURE_HEX_SIZE];
    pux_sha256_init(&sha);
    pux_sha256_update(&sha, public_key, sizeof(public_key));
    pux_sha256_final(&sha, digest);
    pux_sha256_hex(digest, keyid);
    hex_encode(signature, sizeof(signature), signature_hex);

    FILE *file = fopen(signature_path, "wb");
    if (file == NULL) {
        EVP_PKEY_free(pkey);
        free(input);
        set_errorf(error, error_size, "cannot create signature file: %s", strerror(errno));
        return -1;
    }
    int ok = fprintf(file, "# pux-ed25519-signature=1\nalgorithm=%s\nkeyid=%s\nsignature=%s\n",
                     PUX_SIGNATURE_ALGORITHM, keyid, signature_hex) >= 0;
    if (fflush(file) != 0 || fclose(file) != 0) ok = 0;
    EVP_PKEY_free(pkey);
    free(input);
    if (ok == 0) {
        unlink(signature_path);
        set_error(error, error_size, "cannot write signature file");
        return -1;
    }
    return 0;
}

int pux_signature_verify_file(const char *input_path,
                              const char *signature_path,
                              const char *public_key_path,
                              char *error, size_t error_size)
{
    unsigned char *input = NULL;
    size_t input_size = 0U;
    if (read_signature_input(input_path, &input, &input_size, error, error_size) != 0) return -1;
    unsigned char public_key[PUX_SIGNATURE_PUBLIC_KEY_SIZE];
    char public_keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
    if (load_public_key(public_key_path, public_key, public_keyid, error, error_size) != 0) {
        free(input);
        return -1;
    }
    unsigned char signature[PUX_SIGNATURE_BYTES];
    char signature_keyid[PUX_SIGNATURE_KEYID_HEX_SIZE];
    if (load_signature(signature_path, signature, signature_keyid, error, error_size) != 0) {
        free(input);
        return -1;
    }
    if (strncmp(signature_keyid, public_keyid, 64U) != 0) {
        free(input);
        set_error(error, error_size, "signature keyid does not match trusted public key");
        return -1;
    }
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                                  public_key, sizeof(public_key));
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    const int result = pkey != NULL && ctx != NULL &&
                       EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey) == 1 &&
                       EVP_DigestVerify(ctx, signature, sizeof(signature), input, input_size) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    free(input);
    if (result != 1) {
        set_error(error, error_size, "repository signature verification failed");
        return -1;
    }
    return 0;
}
