/*
 *  git2r, R bindings to the libgit2 library.
 *  Copyright (C) 2013-2014  Stefan Widgren
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2 of the License.
 *
 *  git2r is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/** @file git2r.c
 *  @brief R bindings to the libgit2 library
 *
 *  These functions are called from R with .Call to interface the
 *  libgit2 library from R.
 *
 */

#include <Rdefines.h>
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

#include <git2.h>
#include <git2/repository.h>

static size_t count_staged_changes(git_status_list *status_list);
static size_t count_unstaged_changes(git_status_list *status_list);
static git_repository* get_repository(const SEXP repo);
static void init_commit(const git_commit *commit, SEXP sexp_commit);
static void init_reference(git_reference *ref, SEXP reference);
static void init_signature(const git_signature *sig, SEXP signature);
static int number_of_branches(git_repository *repo, int flags, size_t *n);

/**
 * Error messages
 */

const char err_alloc_memory_buffer[] = "Unable to allocate memory buffer";
const char err_invalid_repository[] = "Invalid repository";
const char err_nothing_added_to_commit[] = "Nothing added to commit";
const char err_unexpected_type_of_branch[] = "Unexpected type of branch";
const char err_unexpected_head_of_branch[] = "Unexpected head of branch";

/**
 * Add files to a repository
 *
 * @param repo S4 class git_repository
 * @param path
 * @return R_NilValue
 */
SEXP add(const SEXP repo, const SEXP path)
{
    int err;
    git_index *index = NULL;
    git_repository *repository = NULL;

    if (R_NilValue == path)
        error("'path' equals R_NilValue");
    if (!isString(path))
        error("'path' must be a string");

    repository= get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    err = git_repository_index(&index, repository);
    if (err < 0)
        goto cleanup;

    err = git_index_add_bypath(index, CHAR(STRING_ELT(path, 0)));
    if (err < 0)
        goto cleanup;

    err = git_index_write(index);
    if (err < 0)
        goto cleanup;

cleanup:
    if (index)
        git_index_free(index);

    if (repository)
        git_repository_free(repository);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d/%d: %s\n", err, e->klass, e->message);
    }

    return R_NilValue;
}

/**
 * List branches in a repository
 *
 * @param repo S4 class git_repository
 * @return VECXSP with S4 objects of class git_branch
 */
SEXP branches(const SEXP repo, const SEXP flags)
{
    SEXP list;
    int err = 0;
    const char* err_msg = NULL;
    git_branch_iterator *iter = NULL;
    size_t i = 0, n = 0;
    size_t protected = 0;
    git_repository *repository = NULL;

    repository = get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    /* Count number of branches before creating the list */
    err = number_of_branches(repository, INTEGER(flags)[0], &n);
    if (err < 0)
        goto cleanup;

    PROTECT(list = allocVector(VECSXP, n));
    protected++;

    err = git_branch_iterator_new(&iter, repository,  INTEGER(flags)[0]);
    if (err < 0)
        goto cleanup;

    for (;;) {
        SEXP branch;
        git_branch_t type;
        git_reference *ref;
        const char *refname;

        err = git_branch_next(&ref, &type, iter);
        if (err < 0) {
            if (GIT_ITEROVER == err) {
                err = 0;
                break;
            }
            goto cleanup;
        }

        PROTECT(branch = NEW_OBJECT(MAKE_CLASS("git_branch")));
        protected++;

        refname = git_reference_name(ref);
        init_reference(ref, branch);

        switch (type) {
        case GIT_BRANCH_LOCAL:
            break;
        case GIT_BRANCH_REMOTE: {
            char *buf;
            size_t buf_size;
            git_remote *remote = NULL;

            buf_size = git_branch_remote_name(NULL, 0, repository, refname);
            buf = malloc(buf_size * sizeof(char));
            if (NULL == buf) {
                err = -1;
                err_msg = err_alloc_memory_buffer;
                goto cleanup;
            }

            git_branch_remote_name(buf, buf_size, repository, refname);
            SET_SLOT(branch, Rf_install("remote"), ScalarString(mkChar(buf)));

            err = git_remote_load(&remote, repository, buf);
            if (err < 0) {
                err = git_remote_create_inmemory(&remote, repository, NULL, buf);
                if (err < 0)
                    goto cleanup;
            }

            SET_SLOT(branch,
                     Rf_install("url"),
                     ScalarString(mkChar(git_remote_url(remote))));

            free(buf);
            git_remote_free(remote);
            break;
        }
        default:
            err = -1;
            err_msg = err_unexpected_type_of_branch;
            goto cleanup;
        }

        switch (git_branch_is_head(ref)) {
        case 0:
            SET_SLOT(branch, Rf_install("head"), ScalarLogical(0));
            break;
        case 1:
            SET_SLOT(branch, Rf_install("head"), ScalarLogical(1));
            break;
        default:
            err = -1;
            err_msg = err_unexpected_head_of_branch;
            goto cleanup;
        }

        git_reference_free(ref);
        SET_VECTOR_ELT(list, i, branch);
        UNPROTECT(1);
        protected--;
        i++;
    }

cleanup:
    if (iter)
        git_branch_iterator_free(iter);

    if (repository)
        git_repository_free(repository);

    if (protected)
        UNPROTECT(protected);

    if (err < 0) {
        if (err_msg) {
            error(err_msg);
        } else {
            const git_error *e = giterr_last();
            error("Error %d: %s\n", e->klass, e->message);
        }
    }

    return list;
}

/**
 * Checkout
 *
 * @param repo S4 class git_repository
 * @param treeish
 * @return R_NilValue
 */
SEXP checkout(SEXP repo, SEXP treeish)
{
    git_repository *repository = NULL;

    repository = get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

cleanup:
    if (repository)
        git_repository_free(repository);

    return R_NilValue;
}

typedef struct {
    int received_progress;
    int received_done;
} progress_data;

/**
 * Show progress of clone
 *
 * @param progress
 * @param payload
 * @return 0
 */
static int clone_progress(const git_transfer_progress *progress, void *payload)
{
    int kbytes = progress->received_bytes / 1024;
    progress_data *pd = (progress_data*)payload;

    if (progress->received_objects < progress->total_objects) {
        int received_percent =
            (100 * progress->received_objects) /
            progress->total_objects;

        if (received_percent > pd->received_progress) {
            Rprintf("Receiving objects: % 3i%% (%i/%i), %4d kb\r",
                    received_percent,
                    progress->received_objects,
                    progress->total_objects,
                    kbytes);
            pd->received_progress += 10;
        }
    } else if (!pd->received_done) {
        Rprintf("Receiving objects: 100%% (%i/%i), %4d kb, done.\n",
                progress->received_objects,
                progress->total_objects,
                kbytes);
        pd->received_done = 1;
    }

    return 0;
}

/**
 * Clone a remote repository
 *
 * @param url the remote repository to clone
 * @param local_path local directory to clone to
 * @param progress show progress
 * @return R_NilValue
 */
SEXP clone(SEXP url, SEXP local_path, SEXP progress)
{
    int err;
    git_repository *repository = NULL;
    git_clone_options clone_opts = GIT_CLONE_OPTIONS_INIT;
    git_checkout_opts checkout_opts = GIT_CHECKOUT_OPTS_INIT;
    progress_data pd = {0};

    /* Check arguments to clone */
    if (R_NilValue == url
        || R_NilValue == local_path
        || R_NilValue == progress
        || !isString(url)
        || !isString(local_path)
        || !isLogical(progress)
        || 1 != length(url)
        || 1 != length(local_path)
        || 1 != length(progress))
        error("Invalid arguments to clone");

    checkout_opts.checkout_strategy = GIT_CHECKOUT_SAFE_CREATE;
    clone_opts.checkout_opts = checkout_opts;
    if (LOGICAL(progress)[0]) {
        clone_opts.remote_callbacks.transfer_progress = &clone_progress;
        clone_opts.remote_callbacks.payload = &pd;
        Rprintf("cloning into '%s'...\n", CHAR(STRING_ELT(local_path, 0)));
    }

    err = git_clone(&repository,
                    CHAR(STRING_ELT(url, 0)),
                    CHAR(STRING_ELT(local_path, 0)),
                    &clone_opts);

    if (repository)
        git_repository_free(repository);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d: %s\n", e->klass, e->message);
    }

    return R_NilValue;
}

/**
 * Commit
 *
 * @param repo S4 class git_repository
 * @param message
 * @param author S4 class git_signature
 * @param committer S4 class git_signature
 * @param parent_list
 * @return R_NilValue
 */
SEXP commit(SEXP repo, SEXP message, SEXP author, SEXP committer, SEXP parent_list)
{
    SEXP when, sexp_commit;
    int err;
    int changes_in_index = 0;
    const char* err_msg = NULL;
    git_signature *sig_author = NULL;
    git_signature *sig_committer = NULL;
    git_index *index = NULL;
    git_oid commit_id, tree_oid;
    git_repository *repository = NULL;
    git_tree *tree = NULL;
    size_t i, count;
    git_commit **parents = NULL;
    git_commit *new_commit = NULL;
    git_status_list *status = NULL;
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show  = GIT_STATUS_SHOW_INDEX_ONLY;

    if (R_NilValue == repo
        || R_NilValue == message
        || !isString(message)
        || R_NilValue == author
        || S4SXP != TYPEOF(author)
        || R_NilValue == committer
        || S4SXP != TYPEOF(committer)
        || R_NilValue == parent_list
        || !isString(parent_list))
        error("Invalid arguments to commit");

    if (0 != strcmp(CHAR(STRING_ELT(getAttrib(author, R_ClassSymbol), 0)),
                    "git_signature"))
        error("author argument not a git_signature");

    if (0 != strcmp(CHAR(STRING_ELT(getAttrib(committer, R_ClassSymbol), 0)),
                    "git_signature"))
        error("committer argument not a git_signature");

    repository = get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    when = GET_SLOT(author, Rf_install("when"));
    err = git_signature_new(&sig_author,
                            CHAR(STRING_ELT(GET_SLOT(author, Rf_install("name")), 0)),
                            CHAR(STRING_ELT(GET_SLOT(author, Rf_install("email")), 0)),
                            REAL(GET_SLOT(when, Rf_install("time")))[0],
                            REAL(GET_SLOT(when, Rf_install("offset")))[0]);
    if (err < 0)
        goto cleanup;

    when = GET_SLOT(committer, Rf_install("when"));
    err = git_signature_new(&sig_committer,
                            CHAR(STRING_ELT(GET_SLOT(committer, Rf_install("name")), 0)),
                            CHAR(STRING_ELT(GET_SLOT(committer, Rf_install("email")), 0)),
                            REAL(GET_SLOT(when, Rf_install("time")))[0],
                            REAL(GET_SLOT(when, Rf_install("offset")))[0]);
    if (err < 0)
        goto cleanup;

    err = git_status_list_new(&status, repository, &opts);
    if (err < 0)
        goto cleanup;

    count = git_status_list_entrycount(status);
    for (i = 0; i < count; ++i) {
        const git_status_entry *s = git_status_byindex(status, i);

        if (s->status == GIT_STATUS_CURRENT)
            continue;

        if (s->status & GIT_STATUS_INDEX_NEW)
            changes_in_index = 1;
        else if (s->status & GIT_STATUS_INDEX_MODIFIED)
            changes_in_index = 1;
        else if (s->status & GIT_STATUS_INDEX_DELETED)
            changes_in_index = 1;
        else if (s->status & GIT_STATUS_INDEX_RENAMED)
            changes_in_index = 1;
        else if (s->status & GIT_STATUS_INDEX_TYPECHANGE)
            changes_in_index = 1;

        if (changes_in_index)
            break;
    }

    if (!changes_in_index) {
        err = -1;
        err_msg = err_nothing_added_to_commit;
        goto cleanup;
    }

    err = git_repository_index(&index, repository);
    if (err < 0)
        goto cleanup;

    if (!git_index_entrycount(index)) {
        err = -1;
        err_msg = err_nothing_added_to_commit;
        goto cleanup;
    }

    err = git_index_write_tree(&tree_oid, index);
    if (err < 0)
        goto cleanup;

    err = git_tree_lookup(&tree, repository, &tree_oid);
    if (err < 0)
        goto cleanup;

    count = LENGTH(parent_list);
    if (count) {
        parents = calloc(count, sizeof(git_commit*));
        if (NULL == parents) {
            err = -1;
            err_msg = err_alloc_memory_buffer;
            goto cleanup;
        }

        for (i = 0; i < count; i++) {
            git_oid oid;

            err = git_oid_fromstr(&oid, CHAR(STRING_ELT(parent_list, 0)));
            if (err < 0)
                goto cleanup;

            err = git_commit_lookup(&parents[i], repository, &oid);
            if (err < 0)
                goto cleanup;
        }
    }

    err = git_commit_create(&commit_id,
                            repository,
                            "HEAD",
                            sig_author,
                            sig_committer,
                            NULL,
                            CHAR(STRING_ELT(message, 0)),
                            tree,
                            count,
                            (const git_commit**)parents);
    if (err < 0)
        goto cleanup;

    err = git_commit_lookup(&new_commit, repository, &commit_id);
    if (err < 0)
        goto cleanup;

    PROTECT(sexp_commit = NEW_OBJECT(MAKE_CLASS("git_commit")));
    init_commit(new_commit, sexp_commit);

cleanup:
    if (sig_author)
        git_signature_free(sig_author);

    if (sig_committer)
        git_signature_free(sig_committer);

    if (index)
        git_index_free(index);

    if (status)
        git_status_list_free(status);

    if (tree)
        git_tree_free(tree);

    if (repository)
        git_repository_free(repository);

    if (parents) {
        for (i = 0; i < count; i++) {
            if (parents[i])
                git_commit_free(parents[i]);
        }
        free(parents);
    }

    if (new_commit)
        git_commit_free(new_commit);

    UNPROTECT(1);

    if (err < 0) {
        if (err_msg) {
            error(err_msg);
        } else {
            const git_error *e = giterr_last();
            error("Error %d/%d: %s\n", err, e->klass, e->message);
        }
    }

    return sexp_commit;
}

/**
 * Config
 *
 * @param repo S4 class git_repository
 * @param variables
 * @return R_NilValue
 */
SEXP config(SEXP repo, SEXP variables)
{
    SEXP names;
    int err, i;
    git_config *cfg = NULL;
    git_repository *repository = NULL;

    if (R_NilValue == variables)
        error("'variables' equals R_NilValue.");
    if (!isNewList(variables))
        error("'variables' must be a list.");

    repository = get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    err = git_repository_config(&cfg, repository);
    if (err < 0)
        goto cleanup;

    names = getAttrib(variables, R_NamesSymbol);
    for (i = 0; i < length(variables); i++) {
        const char *key = CHAR(STRING_ELT(names, i));
        const char *value = CHAR(STRING_ELT(VECTOR_ELT(variables, i), 0));

        err = git_config_set_string(cfg, key, value);
        if (err < 0)
            goto cleanup;
    }

cleanup:
    if (config)
        git_config_free(cfg);

    if (repository)
        git_repository_free(repository);

    return R_NilValue;
}

/**
 * Count number of ignored files
 *
 * @param status
 * @return The number of files
 */
static size_t count_ignored_files(git_status_list *status_list)
{
    size_t i = 0;
    size_t ignored = 0;
    size_t count = git_status_list_entrycount(status_list);

    for (; i < count; ++i) {
        const git_status_entry *s = git_status_byindex(status_list, i);

        if (s->status == GIT_STATUS_IGNORED)
            ignored++;
    }

    return ignored;
}

/**
 * Count number of changes in index
 *
 * @param status
 * @return The number of changes
 */
static size_t count_staged_changes(git_status_list *status_list)
{
    size_t i = 0;
    size_t changes = 0;
    size_t count = git_status_list_entrycount(status_list);

    for (; i < count; ++i) {
        const git_status_entry *s = git_status_byindex(status_list, i);

        if (s->status == GIT_STATUS_CURRENT)
            continue;

        if (s->status & GIT_STATUS_INDEX_NEW)
            changes++;
        else if (s->status & GIT_STATUS_INDEX_MODIFIED)
            changes++;
        else if (s->status & GIT_STATUS_INDEX_DELETED)
            changes++;
        else if (s->status & GIT_STATUS_INDEX_RENAMED)
            changes++;
        else if (s->status & GIT_STATUS_INDEX_TYPECHANGE)
            changes++;
    }

    return changes;
}

/**
 * Count number of changes in workdir relative to index
 *
 * @param status
 * @return The number of changes
 */
static size_t count_unstaged_changes(git_status_list *status_list)
{
    size_t i = 0;
    size_t changes = 0;
    size_t count = git_status_list_entrycount(status_list);

    for (; i < count; ++i) {
        const git_status_entry *s = git_status_byindex(status_list, i);

        if (s->status == GIT_STATUS_CURRENT || s->index_to_workdir == NULL)
            continue;

        if (s->status & GIT_STATUS_WT_MODIFIED)
            changes++;
        else if (s->status & GIT_STATUS_WT_DELETED)
            changes++;
        else if (s->status & GIT_STATUS_WT_RENAMED)
            changes++;
        else if (s->status & GIT_STATUS_WT_TYPECHANGE)
            changes++;
    }

    return changes;
}

/**
 * Count number of untracked files
 *
 * @param status
 * @return The number of files
 */
static size_t count_untracked_files(git_status_list *status_list)
{
    size_t i = 0;
    size_t untracked = 0;
    size_t count = git_status_list_entrycount(status_list);

    for (; i < count; ++i) {
        const git_status_entry *s = git_status_byindex(status_list, i);

        if (s->status == GIT_STATUS_WT_NEW)
            untracked++;
    }

    return untracked;
}

/**
 * Get the configured signature for a repository
 *
 * @param repo S4 class git_repository
 * @return S4 class git_signature
 */
SEXP default_signature(const SEXP repo)
{
    int err;
    git_repository *repository = NULL;
    git_signature *signature = NULL;
    SEXP sig;

    repository = get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    err = git_signature_default(&signature, repository);
    if (err < 0)
        goto cleanup;

    PROTECT(sig = NEW_OBJECT(MAKE_CLASS("git_signature")));
    init_signature(signature, sig);

cleanup:
    if (repository)
        git_repository_free(repository);

    if (signature)
        git_signature_free(signature);

    UNPROTECT(1);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d/%d: %s\n", err, e->klass, e->message);
    }

    return sig;
}

/**
 * Get repo slot from S4 class git_repository
 *
 * @param repo S4 class git_repository
 * @return a git_repository pointer on success else NULL
 */
static git_repository* get_repository(const SEXP repo)
{
    SEXP class_name;
    SEXP path;
    git_repository *r;

    if (R_NilValue == repo || S4SXP != TYPEOF(repo))
        return NULL;

    class_name = getAttrib(repo, R_ClassSymbol);
    if (0 != strcmp(CHAR(STRING_ELT(class_name, 0)), "git_repository"))
        return NULL;

    path = GET_SLOT(repo, Rf_install("path"));
    if (R_NilValue == path)
        return NULL;

    if (git_repository_open(&r, CHAR(STRING_ELT(path, 0))) < 0)
        return NULL;

    return r;
}

/**
 * Init a repository.
 *
 * @param path
 * @param bare
 * @return R_NilValue
 */
SEXP init(const SEXP path, const SEXP bare)
{
    int err;
    git_repository *repository = NULL;

    if (R_NilValue == path)
        error("'path' equals R_NilValue");
    if (!isString(path))
        error("'path' must be a string");
    if (R_NilValue == bare)
        error("'bare' equals R_NilValue");
    if (!isLogical(bare))
        error("'bare' must be a logical");

    err = git_repository_init(&repository,
                              CHAR(STRING_ELT(path, 0)),
                              LOGICAL(bare)[0]);
    if (err < 0)
        error("Unable to init repository");

    git_repository_free(repository);

    return R_NilValue;
}

/**
 * Init slots in S4 class git_commit
 *
 * @param commit a commit object
 * @param sexp_commit S4 class git_commit
 * @return void
 */
static void init_commit(const git_commit *commit, SEXP sexp_commit)
{
    const char *message;
    const char *summary;
    const git_signature *author;
    const git_signature *committer;
    char hex[GIT_OID_HEXSZ + 1];

    git_oid_fmt(hex, git_commit_id(commit));
    hex[GIT_OID_HEXSZ] = '\0';
    SET_SLOT(sexp_commit,
             Rf_install("hex"),
             ScalarString(mkChar(hex)));

    author = git_commit_author(commit);
    if (author) {
        SEXP sexp_author;

        PROTECT(sexp_author = NEW_OBJECT(MAKE_CLASS("git_signature")));
        init_signature(author, sexp_author);
        SET_SLOT(sexp_commit, Rf_install("author"), sexp_author);
        UNPROTECT(1);
    }

    committer = git_commit_committer(commit);
    if (committer) {
        SEXP sexp_committer;

        PROTECT(sexp_committer = NEW_OBJECT(MAKE_CLASS("git_signature")));
        init_signature(committer, sexp_committer);
        SET_SLOT(sexp_commit, Rf_install("committer"), sexp_committer);
        UNPROTECT(1);
    }

    summary = git_commit_summary(commit);
    if (summary) {
        SET_SLOT(sexp_commit,
                 Rf_install("summary"),
                 ScalarString(mkChar(summary)));
    }

    message = git_commit_message(commit);
    if (message) {
        SET_SLOT(sexp_commit,
                 Rf_install("message"),
                 ScalarString(mkChar(message)));
    }
}

/**
 * Init slots in S4 class git_reference.
 *
 * @param ref
 * @param reference
 * @return void
 */
static void init_reference(git_reference *ref, SEXP reference)
{
    char out[41];
    out[40] = '\0';

    SET_SLOT(reference,
             Rf_install("name"),
             ScalarString(mkChar(git_reference_name(ref))));

    SET_SLOT(reference,
             Rf_install("shorthand"),
             ScalarString(mkChar(git_reference_shorthand(ref))));

    switch (git_reference_type(ref)) {
    case GIT_REF_OID:
        SET_SLOT(reference, Rf_install("type"), ScalarInteger(GIT_REF_OID));
        git_oid_fmt(out, git_reference_target(ref));
        SET_SLOT(reference, Rf_install("hex"), ScalarString(mkChar(out)));
        break;
    case GIT_REF_SYMBOLIC:
        SET_SLOT(reference, Rf_install("type"), ScalarInteger(GIT_REF_SYMBOLIC));
        SET_SLOT(reference,
                 Rf_install("target"),
                 ScalarString(mkChar(git_reference_symbolic_target(ref))));
        break;
    default:
        error("Unexpected reference type");
    }
}

/**
 * Init slots in S4 class git_signature.
 *
 * @param sig
 * @param signature
 * @return void
 */
static void init_signature(const git_signature *sig, SEXP signature)
{
    SEXP when;

    SET_SLOT(signature,
             Rf_install("name"),
             ScalarString(mkChar(sig->name)));

    SET_SLOT(signature,
             Rf_install("email"),
             ScalarString(mkChar(sig->email)));

    when = GET_SLOT(signature, Rf_install("when"));

    SET_SLOT(when,
             Rf_install("time"),
             ScalarReal((double)sig->when.time));

    SET_SLOT(when,
             Rf_install("offset"),
             ScalarReal((double)sig->when.offset));
}

/**
 * Init slots in S4 class git_tag
 *
 * @param source a tag
 * @param dest S4 class git_tag to initialize
 * @return void
 */
static void init_tag(git_tag *source, SEXP dest)
{
    int err;
    const git_signature *tagger;
    const git_oid *oid;
    char target[GIT_OID_HEXSZ + 1];

    SET_SLOT(dest,
             Rf_install("message"),
             ScalarString(mkChar(git_tag_message(source))));

    SET_SLOT(dest,
             Rf_install("name"),
             ScalarString(mkChar(git_tag_name(source))));

    tagger = git_tag_tagger(source);
    if (tagger) {
        SEXP sexp_tagger;

        PROTECT(sexp_tagger = NEW_OBJECT(MAKE_CLASS("git_signature")));
        init_signature(tagger, sexp_tagger);
        SET_SLOT(dest, Rf_install("tagger"), sexp_tagger);
        UNPROTECT(1);
    }

    oid = git_tag_target_id(source);
    git_oid_tostr(target, sizeof(target), oid);;
    SET_SLOT(dest,
             Rf_install("target"),
             ScalarString(mkChar(target)));
}

/**
 * Check if repository is bare.
 *
 * @param repo S4 class git_repository
 * @return
 */
SEXP is_bare(const SEXP repo)
{
    SEXP result;
    git_repository *repository;

    repository= get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    if (git_repository_is_bare(repository))
        result = ScalarLogical(TRUE);
    else
        result = ScalarLogical(FALSE);

    git_repository_free(repository);

    return result;
}

/**
 * Check if repository is empty.
 *
 * @param repo S4 class git_repository
 * @return
 */
SEXP is_empty(const SEXP repo)
{
    SEXP result;
    git_repository *repository;

    repository= get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    if (git_repository_is_empty(repository))
        result = ScalarLogical(TRUE);
    else
        result = ScalarLogical(FALSE);

    git_repository_free(repository);

    return result;
}

/**
 * Check if valid repository.
 *
 * @param path
 * @return
 */
SEXP is_repository(const SEXP path)
{
    SEXP result;
    git_repository *repository;
    int err;

    if (R_NilValue == path)
        error("'path' equals R_NilValue");
    if (!isString(path))
        error("'path' must be a string");

    err = git_repository_open(&repository, CHAR(STRING_ELT(path, 0)));
    if (err < 0) {
        result = ScalarLogical(FALSE);
    } else {
        git_repository_free(repository);
        result = ScalarLogical(TRUE);
    }

    return result;
}

/**
 * Add ignored files
 *
 * @param list
 * @param list_index
 * @param status_list
 * @return void
 */
static void list_ignored_files(SEXP list,
                               size_t list_index,
                               git_status_list *status_list)
{
    size_t i = 0, j = 0, count;
    SEXP sub_list, sub_list_names, item;

    /* Create list with the correct number of entries */
    count = count_ignored_files(status_list);
    PROTECT(sub_list = allocVector(VECSXP, count));
    PROTECT(sub_list_names = allocVector(STRSXP, count));

    /* i index the entrycount. j index the added change in list */
    count = git_status_list_entrycount(status_list);
    for (; i < count; i++) {
        const git_status_entry *s = git_status_byindex(status_list, i);

        if (s->status == GIT_STATUS_IGNORED) {
            SET_STRING_ELT(sub_list_names, j, mkChar("ignored"));
            PROTECT(item = allocVector(STRSXP, 1));
            SET_STRING_ELT(item, 0, mkChar(s->index_to_workdir->old_file.path));
            SET_VECTOR_ELT(sub_list, j, item);
            UNPROTECT(1);
            j++;
        }
    }

    setAttrib(sub_list, R_NamesSymbol, sub_list_names);
    SET_VECTOR_ELT(list, list_index, sub_list);
    UNPROTECT(2);
}

/**
 * Add changes in index
 *
 * @param list
 * @param list_index
 * @param status_list
 * @return void
 */
static void list_staged_changes(SEXP list,
                              size_t list_index,
                              git_status_list *status_list)
{
    size_t i = 0, j = 0, count;
    SEXP sub_list, sub_list_names, item;

    /* Create list with the correct number of entries */
    count = count_staged_changes(status_list);
    PROTECT(sub_list = allocVector(VECSXP, count));
    PROTECT(sub_list_names = allocVector(STRSXP, count));

    /* i index the entrycount. j index the added change in list */
    count = git_status_list_entrycount(status_list);
    for (; i < count; i++) {
        char *istatus = NULL;
        const char *old_path, *new_path;
        const git_status_entry *s = git_status_byindex(status_list, i);

        if (s->status == GIT_STATUS_CURRENT)
            continue;

        if (s->status & GIT_STATUS_INDEX_NEW)
            istatus = "new";
        else if (s->status & GIT_STATUS_INDEX_MODIFIED)
            istatus = "modified";
        else if (s->status & GIT_STATUS_INDEX_DELETED)
            istatus = "deleted";
        else if (s->status & GIT_STATUS_INDEX_RENAMED)
            istatus = "renamed";
        else if (s->status & GIT_STATUS_INDEX_TYPECHANGE)
            istatus = "typechange";

        if (!istatus)
            continue;
        SET_STRING_ELT(sub_list_names, j, mkChar(istatus));

        old_path = s->head_to_index->old_file.path;
        new_path = s->head_to_index->new_file.path;

        if (old_path && new_path && strcmp(old_path, new_path)) {
            PROTECT(item = allocVector(STRSXP, 2));
            SET_STRING_ELT(item, 0, mkChar(old_path));
            SET_STRING_ELT(item, 1, mkChar(new_path));
        } else {
            PROTECT(item = allocVector(STRSXP, 1));
            SET_STRING_ELT(item, 0, mkChar(old_path ? old_path : new_path));
        }

        SET_VECTOR_ELT(sub_list, j, item);
        UNPROTECT(1);
        j++;
    }

    setAttrib(sub_list, R_NamesSymbol, sub_list_names);
    SET_VECTOR_ELT(list, list_index, sub_list);
    UNPROTECT(2);
}

/**
 * Add changes in workdir relative to index
 *
 * @param list
 * @param list_index
 * @param status_list
 * @return void
 */
static void list_unstaged_changes(SEXP list,
                                size_t list_index,
                                git_status_list *status_list)
{
    size_t i = 0, j = 0, count;
    SEXP sub_list, sub_list_names, item;

    /* Create list with the correct number of entries */
    count = count_unstaged_changes(status_list);
    PROTECT(sub_list = allocVector(VECSXP, count));
    PROTECT(sub_list_names = allocVector(STRSXP, count));

    /* i index the entrycount. j index the added change in list */
    count = git_status_list_entrycount(status_list);
    for (; i < count; i++) {
        char *wstatus = NULL;
        const char *old_path, *new_path;
        const git_status_entry *s = git_status_byindex(status_list, i);

        if (s->status == GIT_STATUS_CURRENT || s->index_to_workdir == NULL)
            continue;

        if (s->status & GIT_STATUS_WT_MODIFIED)
            wstatus = "modified";
        else if (s->status & GIT_STATUS_WT_DELETED)
            wstatus = "deleted";
        else if (s->status & GIT_STATUS_WT_RENAMED)
            wstatus = "renamed";
        else if (s->status & GIT_STATUS_WT_TYPECHANGE)
            wstatus = "typechange";

        if (!wstatus)
            continue;
        SET_STRING_ELT(sub_list_names, j, mkChar(wstatus));

        old_path = s->index_to_workdir->old_file.path;
        new_path = s->index_to_workdir->new_file.path;

        if (old_path && new_path && strcmp(old_path, new_path)) {
            PROTECT(item = allocVector(STRSXP, 2));
            SET_STRING_ELT(item, 0, mkChar(old_path));
            SET_STRING_ELT(item, 1, mkChar(new_path));
        } else {
            PROTECT(item = allocVector(STRSXP, 1));
            SET_STRING_ELT(item, 0, mkChar(old_path ? old_path : new_path));
        }

        SET_VECTOR_ELT(sub_list, j, item);
        UNPROTECT(1);
        j++;
    }

    setAttrib(sub_list, R_NamesSymbol, sub_list_names);
    SET_VECTOR_ELT(list, list_index, sub_list);
    UNPROTECT(2);
}

/**
 * Add untracked files
 *
 * @param list
 * @param list_index
 * @param status_list
 * @return void
 */
static void list_untracked_files(SEXP list,
                                 size_t list_index,
                                 git_status_list *status_list)
{
    size_t i = 0, j = 0, count;
    SEXP sub_list, sub_list_names, item;

    /* Create list with the correct number of entries */
    count = count_untracked_files(status_list);
    PROTECT(sub_list = allocVector(VECSXP, count));
    PROTECT(sub_list_names = allocVector(STRSXP, count));

    /* i index the entrycount. j index the added change in list */
    count = git_status_list_entrycount(status_list);
    for (; i < count; i++) {
        const git_status_entry *s = git_status_byindex(status_list, i);

        if (s->status == GIT_STATUS_WT_NEW) {
            SET_STRING_ELT(sub_list_names, j, mkChar("untracked"));
            PROTECT(item = allocVector(STRSXP, 1));
            SET_STRING_ELT(item, 0, mkChar(s->index_to_workdir->old_file.path));
            SET_VECTOR_ELT(sub_list, j, item);
            UNPROTECT(1);
            j++;
        }
    }

    setAttrib(sub_list, R_NamesSymbol, sub_list_names);
    SET_VECTOR_ELT(list, list_index, sub_list);
    UNPROTECT(2);
}

/**
 * Count number of branches.
 *
 * @param repo S4 class git_repository
 * @param flags
 * @return
 */
static int number_of_branches(git_repository *repo, int flags, size_t *n)
{
    int err;
    git_branch_iterator *iter;
    git_branch_t type;
    git_reference *ref;

    *n = 0;

    err = git_branch_iterator_new(&iter, repo, flags);
    if (err < 0)
        return err;

    for (;;) {
        err = git_branch_next(&ref, &type, iter);
        if (err < 0)
            break;
        git_reference_free(ref);
        (*n)++;
    }

    git_branch_iterator_free(iter);

    if (GIT_ITEROVER != err)
        return err;
    return 0;
}

/**
 * Count number of revisions.
 *
 * @param walker
 * @return
 */
static size_t number_of_revisions(git_revwalk *walker)
{
    size_t n = 0;
    git_oid oid;

    while (!git_revwalk_next(&oid, walker))
        n++;
    return n;
}

/**
 * Get all references that can be found in a repository.
 *
 * @param repo S4 class git_repository
 * @return VECXSP with S4 objects of class git_reference
 */
SEXP references(const SEXP repo)
{
    int i, err;
    git_strarray ref_list;
    SEXP list = R_NilValue;
    SEXP names = R_NilValue;
    git_reference *ref;
    git_repository *repository;

    repository = get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    err = git_reference_list(&ref_list, repository);
    if (err < 0)
        goto cleanup;

    PROTECT(list = allocVector(VECSXP, ref_list.count));
    PROTECT(names = allocVector(STRSXP, ref_list.count));

    for (i = 0; i < ref_list.count; i++) {
        SEXP reference;

        err = git_reference_lookup(&ref, repository, ref_list.strings[i]);
        if (err < 0)
            goto cleanup;

        PROTECT(reference = NEW_OBJECT(MAKE_CLASS("git_reference")));
        init_reference(ref, reference);
        SET_STRING_ELT(names, i, mkChar(ref_list.strings[i]));
        SET_VECTOR_ELT(list, i, reference);
        UNPROTECT(1);
    }

cleanup:
    git_strarray_free(&ref_list);

    if (repository)
        git_repository_free(repository);

    if (R_NilValue != list && R_NilValue != names) {
        setAttrib(list, R_NamesSymbol, names);
        UNPROTECT(2);
    }

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d/%d: %s\n", err, e->klass, e->message);
    }

    return list;
}

/**
 * Get the configured remotes for a repo
 *
 * @param repo S4 class git_repository
 * @return
 */
SEXP remotes(const SEXP repo)
{
    int i, err;
    git_strarray rem_list;
    SEXP list;
    git_repository *repository;

    repository = get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    err = git_remote_list(&rem_list, repository);
    if (err < 0)
        goto cleanup;

    PROTECT(list = allocVector(STRSXP, rem_list.count));
    for (i = 0; i < rem_list.count; i++)
        SET_STRING_ELT(list, i, mkChar(rem_list.strings[i]));
    UNPROTECT(1);

cleanup:
    git_strarray_free(&rem_list);

    if (repository)
        git_repository_free(repository);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d/%d: %s\n", err, e->klass, e->message);
    }

    return list;
}

/**
 * Get the remote's url
 *
 * @param repo S4 class git_repository
 * @return
 */
SEXP remote_url(const SEXP repo, const SEXP remote)
{
    int err;
    SEXP url;
    size_t len = LENGTH(remote);
    size_t i = 0;
    git_repository *repository;

    repository = get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    PROTECT(url = allocVector(STRSXP, len));

    for (; i < len; i++) {
        git_remote *r;

        err = git_remote_load(&r, repository, CHAR(STRING_ELT(remote, i)));
        if (err < 0)
            goto cleanup;

        SET_STRING_ELT(url, i, mkChar(git_remote_url(r)));
        git_remote_free(r);
    }

cleanup:
    git_repository_free(repository);

    UNPROTECT(1);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d/%d: %s\n", err, e->klass, e->message);
    }

    return url;
}

/**
 * List revisions
 *
 * @param repo S4 class git_repository
 * @return
 */
SEXP revisions(SEXP repo)
{
    int i=0;
    int err = 0;
    SEXP list;
    size_t n = 0;
    git_revwalk *walker = NULL;
    git_repository *repository;

    repository = get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    if (git_repository_is_empty(repository)) {
        /* No commits, create empty list */
        PROTECT(list = allocVector(VECSXP, 0));
        goto cleanup;
    }

    err = git_revwalk_new(&walker, repository);
    if (err < 0)
        goto cleanup;

    err = git_revwalk_push_head(walker);
    if (err < 0)
        goto cleanup;

    /* Count number of revisions before creating the list */
    n = number_of_revisions(walker);

    /* Create list to store result */
    PROTECT(list = allocVector(VECSXP, n));

    git_revwalk_reset(walker);
    err = git_revwalk_push_head(walker);
    if (err < 0)
        goto cleanup;

    for (;;) {
        git_commit *commit;
        SEXP sexp_commit;
        git_oid oid;

        err = git_revwalk_next(&oid, walker);
        if (err < 0) {
            if (GIT_ITEROVER == err)
                err = 0;
            goto cleanup;
        }

        err = git_commit_lookup(&commit, repository, &oid);
        if (err < 0)
            goto cleanup;

        PROTECT(sexp_commit = NEW_OBJECT(MAKE_CLASS("git_commit")));
        init_commit(commit, sexp_commit);
        SET_VECTOR_ELT(list, i, sexp_commit);
        UNPROTECT(1);
        i++;

        git_commit_free(commit);
    }

cleanup:
    if (walker)
        git_revwalk_free(walker);

    git_repository_free(repository);

    UNPROTECT(1);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d/%d: %s\n", err, e->klass, e->message);
    }

    return list;
}

/**
 * Get state of the repository working directory and the staging area.
 *
 * @param repo S4 class git_repository
 * @return VECXSP with status
 */
SEXP status(const SEXP repo,
            const SEXP staged,
            const SEXP unstaged,
            const SEXP untracked,
            const SEXP ignored)
{
    int err;
    size_t i=0, count;
    SEXP list, list_names;
    git_repository *repository;
    git_status_list *status_list = NULL;
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;

    /* Check arguments to status */
    if (R_NilValue == staged
        || R_NilValue == unstaged
        || R_NilValue == untracked
        || R_NilValue == ignored
        || !isLogical(staged)
        || !isLogical(unstaged)
        || !isLogical(untracked)
        || !isLogical(ignored)
        || 1 != length(staged)
        || 1 != length(unstaged)
        || 1 != length(untracked)
        || 1 != length(ignored))
        error("Invalid arguments to status");

    repository = get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    opts.show  = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
        GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;

    if (LOGICAL(untracked)[0])
        opts.flags |= GIT_STATUS_OPT_INCLUDE_UNTRACKED;
    if (LOGICAL(ignored)[0])
        opts.flags |= GIT_STATUS_OPT_INCLUDE_IGNORED;
    err = git_status_list_new(&status_list, repository, &opts);
    if (err < 0)
        goto cleanup;

    count = LOGICAL(staged)[0] +
        LOGICAL(unstaged)[0] +
        LOGICAL(untracked)[0] +
        LOGICAL(ignored)[0];

    PROTECT(list = allocVector(VECSXP, count));
    PROTECT(list_names = allocVector(STRSXP, count));

    if (LOGICAL(staged)[0]) {
        SET_STRING_ELT(list_names, i, mkChar("staged"));
        list_staged_changes(list, i, status_list);
        i++;
    }

    if (LOGICAL(unstaged)[0]) {
        SET_STRING_ELT(list_names, i, mkChar("unstaged"));
        list_unstaged_changes(list, i, status_list);
        i++;
    }

    if (LOGICAL(untracked)[0]) {
        SET_STRING_ELT(list_names, i, mkChar("untracked"));
        list_untracked_files(list, i, status_list);
        i++;
    }

    if (LOGICAL(ignored)[0]) {
        SET_STRING_ELT(list_names, i, mkChar("ignored"));
        list_ignored_files(list, i, status_list);
    }

    setAttrib(list, R_NamesSymbol, list_names);

cleanup:
    if (status_list)
        git_status_list_free(status_list);

    if (repository)
        git_repository_free(repository);

    UNPROTECT(2);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d/%d: %s\n", err, e->klass, e->message);
    }

    return list;
}

/**
 * Get all tags that can be found in a repository.
 *
 * @param repo S4 class git_repository
 * @return VECXSP with S4 objects of class git_tag
 */
SEXP tags(const SEXP repo)
{
    int err;
    SEXP list;
    size_t protected = 0;
    git_repository *repository;
    git_reference* reference = NULL;
    git_tag *tag = NULL;
    git_strarray tag_names = {0};
    size_t i;

    repository = get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    err = git_tag_list(&tag_names, repository);
    if (err < 0)
        goto cleanup;

    PROTECT(list = allocVector(VECSXP, tag_names.count));
    protected++;

    for(i = 0; i < tag_names.count; i++) {
        SEXP sexp_tag;
        const git_oid *oid;

        err = git_reference_dwim(&reference, repository, tag_names.strings[i]);
        if (err < 0)
            goto cleanup;

        oid = git_reference_target(reference);
        err = git_tag_lookup(&tag, repository, oid);
        if (err < 0)
            goto cleanup;

        PROTECT(sexp_tag = NEW_OBJECT(MAKE_CLASS("git_tag")));
        protected++;
        init_tag(tag, sexp_tag);

        SET_VECTOR_ELT(list, i, sexp_tag);
        UNPROTECT(1);
        protected--;

        git_tag_free(tag);
        tag = NULL;
        git_reference_free(reference);
        reference = NULL;
    }

cleanup:
    git_strarray_free(&tag_names);

    if (tag)
        git_tag_free(tag);

    if (reference)
        git_reference_free(reference);

    if (repository)
        git_repository_free(repository);

    if (protected)
        UNPROTECT(protected);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d: %s\n", e->klass, e->message);
    }

    return list;
}

/**
 * Get workdir of repository.
 *
 * @param repo S4 class git_repository
 * @return
 */
SEXP workdir(const SEXP repo)
{
    SEXP result;
    git_repository *repository;

    repository = get_repository(repo);
    if (!repository)
        error(err_invalid_repository);

    result = ScalarString(mkChar(git_repository_workdir(repository)));

    git_repository_free(repository);

    return result;
}

static const R_CallMethodDef callMethods[] =
{
    {"add", (DL_FUNC)&add, 2},
    {"branches", (DL_FUNC)&branches, 2},
    {"checkout", (DL_FUNC)&checkout, 2},
    {"clone", (DL_FUNC)&clone, 2},
    {"commit", (DL_FUNC)&commit, 5},
    {"config", (DL_FUNC)&config, 2},
    {"default_signature", (DL_FUNC)&default_signature, 1},
    {"init", (DL_FUNC)&init, 2},
    {"is_bare", (DL_FUNC)&is_bare, 1},
    {"is_empty", (DL_FUNC)&is_empty, 1},
    {"is_repository", (DL_FUNC)&is_repository, 1},
    {"references", (DL_FUNC)&references, 1},
    {"remotes", (DL_FUNC)&remotes, 1},
    {"remote_url", (DL_FUNC)&remote_url, 2},
    {"revisions", (DL_FUNC)&revisions, 1},
    {"status", (DL_FUNC)&status, 5},
    {"tags", (DL_FUNC)&tags, 1},
    {"workdir", (DL_FUNC)&workdir, 1},
    {NULL, NULL, 0}
};

/**
 * Register routines to R.
 *
 * @param info Information about the DLL being loaded
 */
void
R_init_gitr(DllInfo *info)
{
    R_registerRoutines(info, NULL, callMethods, NULL, NULL);
}
