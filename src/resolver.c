#include "pux/resolver.h"

#include "pux/package.h"
#include "pux/container.h"
#include "pux/repo.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PUX_RESOLVER_MAX_CANDIDATES 4096U
#define PUX_RESOLVER_MAX_STEPS 65536U
#define PUX_RESOLVER_MAX_PATH 4096U

enum requirement_operator {
    REQ_ANY = 0,
    REQ_EQ,
    REQ_LT,
    REQ_LE,
    REQ_GT,
    REQ_GE
};

struct requirement {
    char *name;
    char *version;
    enum requirement_operator op;
};

struct candidate {
    struct pux_package_manifest manifest;
    char *path;
};

struct candidate_set {
    struct candidate *items;
    size_t count;
};

struct solve_state {
    const struct candidate_set *candidates;
    unsigned char *selected;
    size_t *plan;
    size_t plan_count;
    size_t plan_capacity;
    const char *target_arch;
    size_t steps;
};

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error == NULL || error_size == 0U) {
        return;
    }
    (void)snprintf(error, error_size, "%s", message);
}

static void set_errorf(char *error, size_t error_size, const char *format, const char *value)
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

static char *duplicate_range(const char *start, size_t length)
{
    char *copy = malloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, start, length);
    copy[length] = '\0';
    return copy;
}

static int has_suffix(const char *name, const char *suffix)
{
    const size_t length = strlen(name);
    const size_t suffix_length = strlen(suffix);
    return length > suffix_length && strcmp(name + length - suffix_length, suffix) == 0;
}

static int is_repository_entry_filename(const char *name)
{
    return has_suffix(name, ".pux") ||
           has_suffix(name, ".pux.manifest") ||
           has_suffix(name, ".manifest");
}

static int join_path(const char *base, const char *name, char *output, size_t output_size)
{
    const size_t base_len = strlen(base);
    const size_t name_len = strlen(name);
    const int separator = base_len != 0U && base[base_len - 1U] != '/';
    const size_t total = base_len + (size_t)separator + name_len + 1U;

    if (total > output_size) {
        return -1;
    }

    memcpy(output, base, base_len);
    size_t offset = base_len;
    if (separator != 0) {
        output[offset++] = '/';
    }
    memcpy(output + offset, name, name_len);
    output[offset + name_len] = '\0';
    return 0;
}

static void candidate_set_free(struct candidate_set *set)
{
    if (set == NULL) {
        return;
    }
    for (size_t i = 0U; i < set->count; ++i) {
        pux_package_manifest_free(&set->items[i].manifest);
        free(set->items[i].path);
    }
    free(set->items);
    set->items = NULL;
    set->count = 0U;
}

static int candidate_append(struct candidate_set *set,
                            const struct pux_package_manifest *manifest,
                            const char *path)
{
    if (set->count >= PUX_RESOLVER_MAX_CANDIDATES) {
        return -1;
    }

    struct candidate *items = realloc(set->items, (set->count + 1U) * sizeof(*items));
    if (items == NULL) {
        return -1;
    }
    set->items = items;

    struct candidate *candidate = &set->items[set->count];
    memset(candidate, 0, sizeof(*candidate));

    char error[256] = {0};
    /* Re-parse the canonical manifest through an in-memory serialization rather
     * than exposing internal package.c allocation details. */
    FILE *tmp = tmpfile();
    if (tmp == NULL) {
        return -1;
    }
    if (pux_package_manifest_write_stream(manifest, tmp) != 0 || fflush(tmp) != 0 ||
        fseek(tmp, 0L, SEEK_END) != 0) {
        fclose(tmp);
        return -1;
    }
    const long end = ftell(tmp);
    if (end <= 0L || (unsigned long)end > (unsigned long)PUX_PACKAGE_MAX_MANIFEST_SIZE ||
        fseek(tmp, 0L, SEEK_SET) != 0) {
        fclose(tmp);
        return -1;
    }
    const size_t size = (size_t)end;
    unsigned char *buffer = malloc(size);
    if (buffer == NULL) {
        fclose(tmp);
        return -1;
    }
    const size_t read_size = fread(buffer, 1U, size, tmp);
    const int failed = ferror(tmp) != 0;
    fclose(tmp);
    if (failed || read_size != size ||
        pux_package_manifest_read_buffer(buffer, size, &candidate->manifest, error, sizeof(error)) != 0) {
        free(buffer);
        pux_package_manifest_free(&candidate->manifest);
        return -1;
    }
    free(buffer);

    candidate->path = duplicate_string(path);
    if (candidate->path == NULL) {
        pux_package_manifest_free(&candidate->manifest);
        return -1;
    }
    set->count++;
    return 0;
}

static int load_repository(const char *repository_dir,
                           struct candidate_set *set,
                           char *error, size_t error_size)
{
    struct stat st;
    if (stat(repository_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_errorf(error, error_size, "repository is not a directory: %s", repository_dir);
        return -1;
    }

    DIR *dir = opendir(repository_dir);
    if (dir == NULL) {
        set_errorf(error, error_size, "cannot open repository: %s", strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, PUX_REPO_INDEX_NAME) == 0 ||
            !is_repository_entry_filename(entry->d_name)) {
            continue;
        }

        char path[PUX_RESOLVER_MAX_PATH];
        if (join_path(repository_dir, entry->d_name, path, sizeof(path)) != 0) {
            closedir(dir);
            set_error(error, error_size, "repository manifest path is too long");
            return -1;
        }

        struct stat item_st;
        if (stat(path, &item_st) != 0) {
            closedir(dir);
            set_errorf(error, error_size, "cannot stat repository manifest: %s", strerror(errno));
            return -1;
        }
        if (!S_ISREG(item_st.st_mode)) {
            continue;
        }

        struct pux_package_manifest manifest;
        char read_error[512] = {0};
        int read_result;
        if (has_suffix(entry->d_name, ".pux")) {
            read_result = pux_package_archive_validate(path, &manifest, read_error, sizeof(read_error));
        } else {
            read_result = pux_package_manifest_read_file(path, &manifest, read_error, sizeof(read_error));
        }
        if (read_result != 0 ||
            pux_package_manifest_validate(&manifest, read_error, sizeof(read_error)) != 0) {
            closedir(dir);
            set_errorf(error, error_size, "invalid repository package: %s", entry->d_name);
            return -1;
        }

        if (candidate_append(set, &manifest, path) != 0) {
            pux_package_manifest_free(&manifest);
            closedir(dir);
            set_error(error, error_size, "out of memory while loading repository");
            return -1;
        }
        pux_package_manifest_free(&manifest);
    }

    if (closedir(dir) != 0) {
        set_error(error, error_size, "cannot close repository");
        return -1;
    }

    if (set->count == 0U) {
        set_error(error, error_size, "repository contains no package manifests");
        return -1;
    }
    return 0;
}

static void requirement_free(struct requirement *requirement)
{
    if (requirement == NULL) {
        return;
    }
    free(requirement->name);
    free(requirement->version);
    requirement->name = NULL;
    requirement->version = NULL;
    requirement->op = REQ_ANY;
}

static int parse_requirement(const char *expression,
                             struct requirement *requirement,
                             char *error, size_t error_size)
{
    memset(requirement, 0, sizeof(*requirement));

    if (expression == NULL || expression[0] == '\0') {
        set_error(error, error_size, "empty dependency expression");
        return -1;
    }

    for (const char *cursor = expression; *cursor != '\0'; ++cursor) {
        if (isspace((unsigned char)*cursor)) {
            set_error(error, error_size, "dependency expressions may not contain whitespace");
            return -1;
        }
    }

    const char *operator_pos = strpbrk(expression, "<>=");
    if (operator_pos == NULL) {
        requirement->name = duplicate_string(expression);
        if (requirement->name == NULL) {
            set_error(error, error_size, "out of memory while parsing dependency");
            return -1;
        }
        requirement->op = REQ_ANY;
        return 0;
    }

    if (operator_pos == expression) {
        set_error(error, error_size, "dependency is missing a package name");
        return -1;
    }

    requirement->name = duplicate_range(expression, (size_t)(operator_pos - expression));
    if (requirement->name == NULL) {
        set_error(error, error_size, "out of memory while parsing dependency");
        return -1;
    }

    size_t operator_length = 1U;
    if ((operator_pos[0] == '<' || operator_pos[0] == '>') && operator_pos[1] == '=') {
        operator_length = 2U;
    }

    switch (operator_pos[0]) {
        case '=': requirement->op = REQ_EQ; break;
        case '<': requirement->op = operator_length == 2U ? REQ_LE : REQ_LT; break;
        case '>': requirement->op = operator_length == 2U ? REQ_GE : REQ_GT; break;
        default: requirement->op = REQ_ANY; break;
    }

    const char *version = operator_pos + operator_length;
    if (version[0] == '\0') {
        requirement_free(requirement);
        set_error(error, error_size, "dependency is missing a version");
        return -1;
    }

    if (strpbrk(version, "<>=") != NULL) {
        requirement_free(requirement);
        set_error(error, error_size, "dependency contains multiple version operators");
        return -1;
    }

    requirement->version = duplicate_string(version);
    if (requirement->version == NULL) {
        requirement_free(requirement);
        set_error(error, error_size, "out of memory while parsing dependency");
        return -1;
    }
    return 0;
}

static int next_version_part(const char **cursor, char *buffer, size_t buffer_size, int *is_numeric)
{
    const char *value = *cursor;
    while (*value != '\0' && !isalnum((unsigned char)*value)) {
        ++value;
    }
    if (*value == '\0') {
        *cursor = value;
        return 0;
    }

    const char *start = value;
    *is_numeric = isdigit((unsigned char)*value) != 0;
    while (*value != '\0' && isalnum((unsigned char)*value)) {
        if (isdigit((unsigned char)*value) != (*is_numeric != 0)) {
            /* A digit/letter boundary starts a new part. */
            break;
        }
        ++value;
    }

    const size_t length = (size_t)(value - start);
    if (length == 0U || length + 1U > buffer_size) {
        return -1;
    }
    memcpy(buffer, start, length);
    buffer[length] = '\0';
    *cursor = value;
    return 1;
}

static int compare_version(const char *left, const char *right)
{
    const char *l = left;
    const char *r = right;

    for (;;) {
        char lp[64];
        char rp[64];
        int l_numeric = 0;
        int r_numeric = 0;
        const int l_more = next_version_part(&l, lp, sizeof(lp), &l_numeric);
        const int r_more = next_version_part(&r, rp, sizeof(rp), &r_numeric);

        if (l_more < 0 || r_more < 0) {
            return strcmp(left, right);
        }
        if (l_more == 0 && r_more == 0) {
            return 0;
        }
        if (l_more == 0) {
            return -1;
        }
        if (r_more == 0) {
            return 1;
        }

        if (l_numeric != 0 && r_numeric != 0) {
            size_t lo = 0U;
            size_t ro = 0U;
            while (lp[lo] == '0' && lp[lo + 1U] != '\0') ++lo;
            while (rp[ro] == '0' && rp[ro + 1U] != '\0') ++ro;
            const size_t ll = strlen(lp + lo);
            const size_t rl = strlen(rp + ro);
            if (ll != rl) {
                return ll < rl ? -1 : 1;
            }
            const int numeric_compare = strcmp(lp + lo, rp + ro);
            if (numeric_compare != 0) {
                return numeric_compare < 0 ? -1 : 1;
            }
        } else if (l_numeric != r_numeric) {
            return l_numeric != 0 ? 1 : -1;
        } else {
            const int lexical = strcmp(lp, rp);
            if (lexical != 0) {
                return lexical < 0 ? -1 : 1;
            }
        }
    }
}

static int version_satisfies(const char *candidate_version,
                             const struct requirement *requirement)
{
    if (requirement->op == REQ_ANY) {
        return 1;
    }
    const int compare = compare_version(candidate_version, requirement->version);
    switch (requirement->op) {
        case REQ_EQ: return compare == 0;
        case REQ_LT: return compare < 0;
        case REQ_LE: return compare <= 0;
        case REQ_GT: return compare > 0;
        case REQ_GE: return compare >= 0;
        case REQ_ANY: return 1;
    }
    return 0;
}

static int capability_exists(const struct pux_package_manifest *manifest, const char *name)
{
    if (strcmp(manifest->name, name) == 0) {
        return 1;
    }
    for (size_t i = 0U; i < manifest->provides.count; ++i) {
        if (strcmp(manifest->provides.items[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Returns 2 for a direct package-name match, 1 for an unversioned provide,
 * and 0 for no match. */
static int candidate_match(const struct candidate *candidate,
                           const struct requirement *requirement,
                           const char *target_arch)
{
    if (strcmp(candidate->manifest.arch, target_arch) != 0 &&
        strcmp(candidate->manifest.arch, "noarch") != 0) {
        return 0;
    }

    if (strcmp(candidate->manifest.name, requirement->name) == 0) {
        return version_satisfies(candidate->manifest.version, requirement) ? 2 : 0;
    }

    if (requirement->op != REQ_ANY) {
        return 0;
    }

    for (size_t i = 0U; i < candidate->manifest.provides.count; ++i) {
        if (strcmp(candidate->manifest.provides.items[i], requirement->name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int candidate_preferred(const struct candidate_set *set,
                               size_t left_index,
                               size_t right_index,
                               const struct requirement *requirement,
                               const char *target_arch)
{
    const struct candidate *left = &set->items[left_index];
    const struct candidate *right = &set->items[right_index];
    const int left_match = candidate_match(left, requirement, target_arch);
    const int right_match = candidate_match(right, requirement, target_arch);
    if (left_match != right_match) {
        return left_match > right_match;
    }

    const int version = compare_version(left->manifest.version, right->manifest.version);
    if (version != 0) {
        return version > 0;
    }
    if (left->manifest.release != right->manifest.release) {
        return left->manifest.release > right->manifest.release;
    }
    return strcmp(left->path, right->path) < 0;
}

static int selected_has_name(const struct solve_state *state, const char *name, size_t *index_out)
{
    for (size_t i = 0U; i < state->candidates->count; ++i) {
        if (state->selected[i] != 0U && strcmp(state->candidates->items[i].manifest.name, name) == 0) {
            if (index_out != NULL) {
                *index_out = i;
            }
            return 1;
        }
    }
    return 0;
}

static int requirement_satisfied(const struct solve_state *state,
                                 const struct requirement *requirement)
{
    for (size_t i = 0U; i < state->candidates->count; ++i) {
        if (state->selected[i] != 0U &&
            candidate_match(&state->candidates->items[i], requirement, state->target_arch) != 0) {
            return 1;
        }
    }
    return 0;
}

static int conflict_token_matches(const struct pux_package_manifest *manifest, const char *token)
{
    return capability_exists(manifest, token);
}

static int conflicts_supported(const struct pux_package_string_list *conflicts)
{
    for (size_t i = 0U; i < conflicts->count; ++i) {
        if (strpbrk(conflicts->items[i], "<>=") != NULL) {
            return 0;
        }
    }
    return 1;
}

static int candidate_conflicts_with_selected(const struct solve_state *state, size_t candidate_index)
{
    const struct candidate *candidate = &state->candidates->items[candidate_index];

    if (!conflicts_supported(&candidate->manifest.conflicts)) {
        return 1;
    }

    for (size_t i = 0U; i < candidate->manifest.conflicts.count; ++i) {
        const char *conflict = candidate->manifest.conflicts.items[i];
        if (capability_exists(&candidate->manifest, conflict)) {
            return 1;
        }
        for (size_t j = 0U; j < state->candidates->count; ++j) {
            if (state->selected[j] != 0U &&
                conflict_token_matches(&state->candidates->items[j].manifest, conflict)) {
                return 1;
            }
        }
    }

    for (size_t j = 0U; j < state->candidates->count; ++j) {
        if (state->selected[j] == 0U) {
            continue;
        }
        const struct pux_package_manifest *selected = &state->candidates->items[j].manifest;
        if (!conflicts_supported(&selected->conflicts)) {
            return 1;
        }
        for (size_t i = 0U; i < selected->conflicts.count; ++i) {
            if (capability_exists(&candidate->manifest, selected->conflicts.items[i])) {
                return 1;
            }
        }
    }
    return 0;
}

static int append_plan(struct solve_state *state, size_t candidate_index)
{
    if (state->plan_count == state->plan_capacity) {
        size_t next_capacity = state->plan_capacity == 0U ? 16U : state->plan_capacity * 2U;
        size_t *plan = realloc(state->plan, next_capacity * sizeof(*plan));
        if (plan == NULL) {
            return -1;
        }
        state->plan = plan;
        state->plan_capacity = next_capacity;
    }
    state->plan[state->plan_count++] = candidate_index;
    return 0;
}

static int resolve_requirement(struct solve_state *state,
                               const struct requirement *requirement,
                               char *error, size_t error_size)
{
    if (++state->steps > PUX_RESOLVER_MAX_STEPS) {
        set_error(error, error_size, "dependency resolution step limit exceeded");
        return -1;
    }

    if (requirement_satisfied(state, requirement)) {
        return 0;
    }

    size_t existing_index = 0U;
    if (selected_has_name(state, requirement->name, &existing_index)) {
        set_errorf(error, error_size, "selected version does not satisfy dependency: %s", requirement->name);
        return -1;
    }

    size_t *matches = NULL;
    size_t match_count = 0U;
    for (size_t i = 0U; i < state->candidates->count; ++i) {
        if (state->selected[i] != 0U) {
            continue;
        }
        if (candidate_match(&state->candidates->items[i], requirement, state->target_arch) == 0) {
            continue;
        }
        size_t *new_matches = realloc(matches, (match_count + 1U) * sizeof(*new_matches));
        if (new_matches == NULL) {
            free(matches);
            set_error(error, error_size, "out of memory while resolving dependency");
            return -1;
        }
        matches = new_matches;
        matches[match_count++] = i;
    }

    if (match_count == 0U) {
        free(matches);
        set_errorf(error, error_size, "no package satisfies dependency: %s", requirement->name);
        return -1;
    }

    for (size_t i = 0U; i < match_count; ++i) {
        for (size_t j = i + 1U; j < match_count; ++j) {
            if (!candidate_preferred(state->candidates, matches[i], matches[j], requirement, state->target_arch)) {
                const size_t temporary = matches[i];
                matches[i] = matches[j];
                matches[j] = temporary;
            }
        }
    }

    char last_error[512] = {0};

    for (size_t match_pos = 0U; match_pos < match_count; ++match_pos) {
        const size_t candidate_index = matches[match_pos];
        const struct candidate *candidate = &state->candidates->items[candidate_index];

        if (selected_has_name(state, candidate->manifest.name, NULL)) {
            continue;
        }
        if (candidate_conflicts_with_selected(state, candidate_index)) {
            (void)snprintf(last_error, sizeof(last_error),
                           "candidate conflicts with the current resolution: %s",
                           candidate->manifest.name);
            continue;
        }

        const size_t old_plan_count = state->plan_count;
        state->selected[candidate_index] = 1U;
        int success = 1;

        for (size_t i = 0U; i < candidate->manifest.depends.count; ++i) {
            struct requirement dependency;
            char dependency_error[512] = {0};
            if (parse_requirement(candidate->manifest.depends.items[i], &dependency,
                                  dependency_error, sizeof(dependency_error)) != 0) {
                success = 0;
                (void)snprintf(last_error, sizeof(last_error),
                               "invalid dependency in package %.200s: %.250s",
                               candidate->manifest.name, dependency_error);
                break;
            }

            if (resolve_requirement(state, &dependency, error, error_size) != 0) {
                (void)snprintf(last_error, sizeof(last_error),
                               "candidate %.200s failed: %.250s",
                               candidate->manifest.name, error);
                requirement_free(&dependency);
                success = 0;
                break;
            }
            requirement_free(&dependency);
        }

        if (success != 0 && append_plan(state, candidate_index) != 0) {
            set_error(error, error_size, "out of memory while building resolution plan");
            success = 0;
        }

        if (success != 0) {
            free(matches);
            return 0;
        }

        state->selected[candidate_index] = 0U;
        state->plan_count = old_plan_count;
    }

    free(matches);
    if (last_error[0] != '\0') {
        set_errorf(error, error_size, "cannot resolve dependency: %s", last_error);
    } else {
        set_errorf(error, error_size, "cannot resolve dependency: %s", requirement->name);
    }
    return -1;
}

static int resolve_conflicts_in_plan(const struct solve_state *state,
                                     char *error, size_t error_size)
{
    for (size_t i = 0U; i < state->plan_count; ++i) {
        const struct candidate *left = &state->candidates->items[state->plan[i]];
        for (size_t j = i + 1U; j < state->plan_count; ++j) {
            const struct candidate *right = &state->candidates->items[state->plan[j]];
            for (size_t c = 0U; c < left->manifest.conflicts.count; ++c) {
                if (strpbrk(left->manifest.conflicts.items[c], "<>=") != NULL) {
                    set_error(error, error_size, "versioned conflicts are not supported yet");
                    return -1;
                }
                if (capability_exists(&right->manifest, left->manifest.conflicts.items[c])) {
                    set_errorf(error, error_size, "package conflict: %s", left->manifest.name);
                    return -1;
                }
            }
            for (size_t c = 0U; c < right->manifest.conflicts.count; ++c) {
                if (strpbrk(right->manifest.conflicts.items[c], "<>=") != NULL) {
                    set_error(error, error_size, "versioned conflicts are not supported yet");
                    return -1;
                }
                if (capability_exists(&left->manifest, right->manifest.conflicts.items[c])) {
                    set_errorf(error, error_size, "package conflict: %s", right->manifest.name);
                    return -1;
                }
            }
        }
    }
    return 0;
}


void pux_resolve_plan_free(struct pux_resolve_plan *plan)
{
    if (plan == NULL) return;
    for (size_t i = 0U; i < plan->count; ++i) free(plan->package_paths[i]);
    free(plan->package_paths);
    plan->package_paths = NULL;
    plan->count = 0U;
}

int pux_resolve_package_plan(const char *package_name,
                             const char *repository_dir,
                             struct pux_resolve_plan *plan,
                             char *error,
                             size_t error_size)
{
    if (package_name == NULL || package_name[0] == '\0' ||
        repository_dir == NULL || plan == NULL) {
        set_error(error, error_size, "invalid resolver argument");
        return -1;
    }

    plan->package_paths = NULL;
    plan->count = 0U;

    struct candidate_set candidates = {0};
    if (load_repository(repository_dir, &candidates, error, error_size) != 0) return -1;

    struct requirement root = {
        .name = duplicate_string(package_name),
        .version = NULL,
        .op = REQ_ANY
    };
    if (root.name == NULL) {
        candidate_set_free(&candidates);
        set_error(error, error_size, "out of memory while resolving root package");
        return -1;
    }

    const char *target_arch = getenv("PUX_ARCH");
    if (target_arch == NULL || target_arch[0] == '\0') {
#if defined(__x86_64__)
        target_arch = "x86_64";
#elif defined(__aarch64__)
        target_arch = "aarch64";
#elif defined(__i386__)
        target_arch = "i686";
#else
        target_arch = "unknown";
#endif
    }

    int root_found = 0;
    for (size_t i = 0U; i < candidates.count; ++i) {
        if (strcmp(candidates.items[i].manifest.name, package_name) == 0 &&
            (strcmp(candidates.items[i].manifest.arch, target_arch) == 0 ||
             strcmp(candidates.items[i].manifest.arch, "noarch") == 0)) {
            root_found = 1;
            break;
        }
    }
    if (root_found == 0) {
        requirement_free(&root);
        candidate_set_free(&candidates);
        set_errorf(error, error_size, "package not found for architecture: %s", package_name);
        return -1;
    }

    unsigned char *selected = calloc(candidates.count, sizeof(*selected));
    if (selected == NULL) {
        requirement_free(&root);
        candidate_set_free(&candidates);
        set_error(error, error_size, "out of memory while creating resolver state");
        return -1;
    }

    struct solve_state state = {
        .candidates = &candidates,
        .selected = selected,
        .plan = NULL,
        .plan_count = 0U,
        .plan_capacity = 0U,
        .target_arch = target_arch,
        .steps = 0U
    };

    const int result = resolve_requirement(&state, &root, error, error_size);
    requirement_free(&root);
    if (result != 0 || resolve_conflicts_in_plan(&state, error, error_size) != 0) {
        free(state.plan);
        free(selected);
        candidate_set_free(&candidates);
        return -1;
    }

    if (state.plan_count > 0U) {
        plan->package_paths = calloc(state.plan_count, sizeof(*plan->package_paths));
        if (plan->package_paths == NULL) {
            free(state.plan);
            free(selected);
            candidate_set_free(&candidates);
            set_error(error, error_size, "out of memory while creating package plan");
            return -1;
        }
    }

    for (size_t i = 0U; i < state.plan_count; ++i) {
        const struct candidate *candidate = &candidates.items[state.plan[i]];
        if (!has_suffix(candidate->path, ".pux")) {
            pux_resolve_plan_free(plan);
            free(state.plan);
            free(selected);
            candidate_set_free(&candidates);
            set_errorf(error, error_size,
                       "repository candidate is not an installable .pux package: %s",
                       candidate->path);
            return -1;
        }
        plan->package_paths[i] = duplicate_string(candidate->path);
        if (plan->package_paths[i] == NULL) {
            pux_resolve_plan_free(plan);
            free(state.plan);
            free(selected);
            candidate_set_free(&candidates);
            set_error(error, error_size, "out of memory while copying package plan");
            return -1;
        }
        plan->count++;
    }

    free(state.plan);
    free(selected);
    candidate_set_free(&candidates);
    return 0;
}

int pux_resolve_package(const char *package_name,
                        const char *repository_dir,
                        FILE *output,
                        char *error,
                        size_t error_size)
{
    if (package_name == NULL || package_name[0] == '\0' || repository_dir == NULL || output == NULL) {
        set_error(error, error_size, "invalid resolver argument");
        return -1;
    }

    struct candidate_set candidates = {0};
    if (load_repository(repository_dir, &candidates, error, error_size) != 0) {
        return -1;
    }

    /* Root package names are resolved directly; provides are intentionally not
     * used for the root to keep `pux resolve NAME` deterministic. */
    struct requirement root = {
        .name = duplicate_string(package_name),
        .version = NULL,
        .op = REQ_ANY
    };
    if (root.name == NULL) {
        candidate_set_free(&candidates);
        set_error(error, error_size, "out of memory while resolving root package");
        return -1;
    }

    const char *target_arch = getenv("PUX_ARCH");
    if (target_arch == NULL || target_arch[0] == '\0') {
#if defined(__x86_64__)
        target_arch = "x86_64";
#elif defined(__aarch64__)
        target_arch = "aarch64";
#elif defined(__i386__)
        target_arch = "i686";
#else
        target_arch = "unknown";
#endif
    }

    int root_found = 0;
    for (size_t i = 0U; i < candidates.count; ++i) {
        if (strcmp(candidates.items[i].manifest.name, package_name) == 0 &&
            (strcmp(candidates.items[i].manifest.arch, target_arch) == 0 ||
             strcmp(candidates.items[i].manifest.arch, "noarch") == 0)) {
            root_found = 1;
            break;
        }
    }
    if (root_found == 0) {
        requirement_free(&root);
        candidate_set_free(&candidates);
        set_errorf(error, error_size, "package not found for architecture: %s", package_name);
        return -1;
    }

    unsigned char *selected = calloc(candidates.count, sizeof(*selected));
    if (selected == NULL) {
        requirement_free(&root);
        candidate_set_free(&candidates);
        set_error(error, error_size, "out of memory while creating resolver state");
        return -1;
    }

    struct solve_state state = {
        .candidates = &candidates,
        .selected = selected,
        .plan = NULL,
        .plan_count = 0U,
        .plan_capacity = 0U,
        .target_arch = target_arch,
        .steps = 0U
    };

    const int result = resolve_requirement(&state, &root, error, error_size);
    requirement_free(&root);

    if (result != 0 || resolve_conflicts_in_plan(&state, error, error_size) != 0) {
        free(state.plan);
        free(selected);
        candidate_set_free(&candidates);
        return -1;
    }

    fprintf(output, "resolution: OK\n");
    for (size_t i = 0U; i < state.plan_count; ++i) {
        const struct candidate *candidate = &candidates.items[state.plan[i]];
        fprintf(output, "install: %s %s-%u %s\n",
                candidate->manifest.name,
                candidate->manifest.version,
                candidate->manifest.release,
                candidate->manifest.arch);
    }

    free(state.plan);
    free(selected);
    candidate_set_free(&candidates);
    return 0;
}
