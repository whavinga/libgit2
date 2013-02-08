/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "git2/blame.h"
#include "git2/commit.h"
#include "common.h"
#include "vector.h"
#include "util.h"
#include "repository.h"



/* Extended-private structures for results. */
struct git_blame_results {
	git_vector hunks;
	git_repository *repository;
	git_blame_options options;

	bool newest_commit_is_ours;
};

/* Comparator to maintain hunk order */
static int blame_hunk_cmp(const void *_a, const void *_b)
{
	git_blame_hunk *a = (git_blame_hunk*)_a,
						*b = (git_blame_hunk*)_b;

	return (a->final_start_line_number == b->final_start_line_number) ? 0 : -1;
}

static git_blame_results* alloc_results(git_repository *repo, git_blame_options opts)
{
	git_blame_results *gbr = (git_blame_results*)calloc(1, sizeof(git_blame_results));
	if (!gbr) {
		giterr_set_oom();
		return NULL;
	}
	git_vector_init(&gbr->hunks, 8, blame_hunk_cmp);
	gbr->repository = repo;
	gbr->options = opts;
	return gbr;
}

void git_blame_free(git_blame_results *results)
{
	if (!results) return;

	git_vector_free(&results->hunks);
	if (results->newest_commit_is_ours) git_commit_free(results->options.newest_commit);
	git__free(results);
}

uint32_t git_blame_results_hunk_count(git_blame_results *results)
{
	assert(results);
	return results->hunks.length;
}

const git_blame_hunk *git_blame_rests_hunk_byindex(git_blame_results *results, uint32_t index)
{
	assert(results);
	return (git_blame_hunk*)git_vector_get(&results->hunks, index);
}

void normalize_options(git_blame_options *out, const git_blame_options *in, git_repository *repo)
{
	git_blame_options dummy = GIT_BLAME_OPTIONS_INIT;
	if (!in) in = &dummy;
	
	memmove(out, in, sizeof(git_blame_options));

	/* No newest_commit => HEAD */
	if (!out->newest_commit) {
		git_reference *head = NULL;
		git_repository_head(&head, repo);
		git_reference_peel((git_object**)(&out->newest_commit), head, GIT_OBJ_COMMIT);
		git_reference_free(head);
	}
}

int git_blame_file(
		git_blame_results **out,
		git_repository *repo,
		const char *path,
		git_blame_options *options)
{
	git_blame_options normOptions = GIT_BLAME_OPTIONS_INIT;
	git_blame_results *res = NULL;

	if (!out || !repo || !path) return -1;
	normalize_options(&normOptions, options, repo);

	res = alloc_results(repo, normOptions);
	if (!options || !options->newest_commit)
		res->newest_commit_is_ours = true;

	*out = res;
	return 0;
}
