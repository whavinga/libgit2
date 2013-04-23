/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "blame.h"
#include "git2/commit.h"
#include "git2/revparse.h"
#include "git2/revwalk.h"
#include "git2/tree.h"
#include "git2/diff.h"
#include "util.h"
#include "repository.h"


static int hunk_byline_search_cmp(const void *key, const void *entry)
{
	uint32_t lineno = *(size_t*)key;
	git_blame_hunk *hunk = (git_blame_hunk*)entry;

	if (lineno < hunk->final_start_line_number)
		return -1;
	if (lineno > (hunk->final_start_line_number + hunk->lines_in_hunk))
		return 1;
	return 0;
}

static int hunk_sort_cmp_by_start_line(const void *_a, const void *_b)
{
	git_blame_hunk *a = (git_blame_hunk*)_a,
						*b = (git_blame_hunk*)_b;

	return a->final_start_line_number - b->final_start_line_number;
}

static git_blame_hunk* new_hunk(uint16_t start, uint16_t lines, uint16_t orig_start, const char *path)
{
	git_blame_hunk *hunk = git__calloc(1, sizeof(git_blame_hunk));
	if (!hunk) return NULL;

	hunk->lines_in_hunk = lines;
	hunk->final_start_line_number = start;
	hunk->orig_start_line_number = orig_start;
	hunk->orig_path = git__strdup(path);

	return hunk;
}

static void free_hunk(git_blame_hunk *hunk)
{
	git__free(hunk->orig_path);
	git__free(hunk);
}

git_blame* git_blame__alloc(
	git_repository *repo,
	git_blame_options opts,
	const char *path)
{
	git_blame *gbr = (git_blame*)calloc(1, sizeof(git_blame));
	if (!gbr) {
		giterr_set_oom();
		return NULL;
	}
	git_vector_init(&gbr->hunks, 8, hunk_sort_cmp_by_start_line);
	git_vector_init(&gbr->unclaimed_hunks, 8, hunk_sort_cmp_by_start_line);
	gbr->repository = repo;
	gbr->options = opts;
	gbr->path = git__strdup(path);
	gbr->final_blob = NULL;
	return gbr;
}

/*
 * Construct a list of char indices for where lines begin
 * Adapted from core git:
 * https://github.com/gitster/git/blob/be5c9fb9049ed470e7005f159bb923a5f4de1309/builtin/blame.c#L1760-L1789
 */
static int prepare_lines(git_blame *blame)
{
	const char *final_buf = git_blob_rawcontent(blame->final_blob);
	const char *buf = final_buf;
	unsigned long len = git_blob_rawsize(blame->final_blob);
	int num = 0, incomplete = 0, bol = 1;

	if (len && buf[len-1] != '\n')
		incomplete++; /* incomplete line at the end */
	while (len--) {
		if (bol) {
			blame->line_index = realloc(blame->line_index,
					sizeof(int *) * (num + 1));
			blame->line_index[num] = buf - final_buf;
			bol = 0;
		}
		if (*buf++ == '\n') {
			num++;
			bol = 1;
		}
	}
	blame->line_index = realloc(blame->line_index,
			sizeof(int *) * (num + incomplete + 1));
	blame->line_index[num + incomplete] = buf - final_buf;
	blame->num_lines = num + incomplete;
	return 0;
}

void git_blame_free(git_blame *blame)
{
	size_t i;
	git_blame_hunk *hunk;

	if (!blame) return;

	git_vector_foreach(&blame->hunks, i, hunk)
		free_hunk(hunk);
	git_vector_free(&blame->hunks);

	git_vector_foreach(&blame->unclaimed_hunks, i, hunk)
		free_hunk(hunk);
	git_vector_free(&blame->unclaimed_hunks);

	git__free((void*)blame->path);
	git_blob_free(blame->final_blob);
	git__free(blame);
}

uint32_t git_blame_get_hunk_count(git_blame *blame)
{
	assert(blame);
	return blame->hunks.length;
}

const git_blame_hunk *git_blame_get_hunk_byindex(git_blame *blame, uint32_t index)
{
	assert(blame);
	return (git_blame_hunk*)git_vector_get(&blame->hunks, index);
}

const git_blame_hunk *git_blame_get_hunk_byline(git_blame *blame, uint32_t lineno)
{
	size_t i;
	assert(blame);

	if (!git_vector_bsearch2( &i, &blame->hunks, hunk_byline_search_cmp, &lineno)) {
		return git_blame_get_hunk_byindex(blame, i);
	}

	return NULL;
}

static void normalize_options(
		git_blame_options *out,
		const git_blame_options *in,
		git_repository *repo)
{
	git_blame_options dummy = GIT_BLAME_OPTIONS_INIT;
	if (!in) in = &dummy;

	memmove(out, in, sizeof(git_blame_options));

	/* No newest_commit => HEAD */
	if (git_oid_iszero(&out->newest_commit)) {
		git_object *obj;
		git_revparse_single(&obj, repo, "HEAD");
		git_oid_cpy(&out->newest_commit, git_object_id(obj));
		git_object_free(obj);
	}
}

static void dump_hunks(git_blame *blame)
{
	size_t i;
	git_blame_hunk *hunk;
	char str[41] = {0};

	git_vector_foreach(&blame->hunks, i, hunk) {
		git_oid_fmt(str, &hunk->final_commit_id);
		printf("CLAIMED: %d-%d (orig %d) %s\n",
				hunk->final_start_line_number,
				hunk->final_start_line_number + hunk->lines_in_hunk - 1,
				hunk->orig_start_line_number,
				str);
	}
	git_vector_foreach(&blame->unclaimed_hunks, i, hunk) {
		git_oid_fmt(str, &hunk->final_commit_id);
		printf("UNCLAIMED: %d-%d (orig %d) %s\n",
				hunk->final_start_line_number,
				hunk->final_start_line_number + hunk->lines_in_hunk - 1,
				hunk->orig_start_line_number,
				str);
	}
}

/*******************************************************************************
 * Trivial blaming
 *******************************************************************************/

static int trivial_file_cb(
	const git_diff_delta *delta,
	float progress,
	void *payload)
{
	git_blame *blame = (git_blame*)payload;

	GIT_UNUSED(progress);

	/* Trivial blame only cares about the original file name */
	blame->trivial_file_match = (0 == git__strcmp(delta->new_file.path, blame->path));

	return 0;
}

void maybe_split_hunk(git_vector *hunks, size_t line)
{
	/* If the requested position is in the middle of an unclaimed blame hunk, split it. */
	git_blame_hunk *hunk;
	size_t i;

	git_vector_foreach(hunks, i, hunk) {
		if (line > hunk->orig_start_line_number &&
		    line < hunk->orig_start_line_number + hunk->lines_in_hunk) {
			size_t new_line_count = hunk->orig_start_line_number + hunk->lines_in_hunk - line;
			git_blame_hunk *nh = new_hunk(line, new_line_count, line, hunk->orig_path);
			hunk->lines_in_hunk -= new_line_count;
			git_vector_insert(hunks, nh);
			break;
		}
	}
}

static int trivial_hunk_cb(
	const git_diff_delta *delta,
	const git_diff_range *range,
	const char *header,
	size_t header_len,
	void *payload)
{
	git_blame *blame = (git_blame*)payload;
	size_t start = range->new_start;
	size_t end = start + range->new_lines - 1;
	git_blame_hunk *hunk;
	size_t i;

	if (!blame->trivial_file_match) return 0;

	GIT_UNUSED(delta);
	GIT_UNUSED(header);
	GIT_UNUSED(header_len);

	printf("  Hunk: %s (%d-%d) <- %s (%d-%d)\n",
			delta->new_file.path,
			range->new_start, range->new_start + max(0, range->new_lines -1),
			delta->old_file.path,
			range->old_start, range->old_start + max(0, range->old_lines -1));

	blame->current_line = start;

	maybe_split_hunk(&blame->unclaimed_hunks, start);

	return 0;
}

static void adjust_hunk_lines(git_vector *hunks, size_t start, int amount)
{
	git_blame_hunk *hunk;
	size_t i;

	git_vector_foreach(hunks, i, hunk) {
		if (hunk->orig_start_line_number >= start) {
			hunk->orig_start_line_number += amount;
		}
	}
}

static int ptrs_equal_cmp(const void *a, const void *b) {
	return
		a < b ? -1 :
		a > b ? 1  :
		0;
}
static void claim_hunk(git_blame *blame, git_blame_hunk *hunk) {
	size_t i;

	if (!hunk) return;

	if (!git_vector_search2(&i, &blame->unclaimed_hunks, ptrs_equal_cmp, hunk))
		git_vector_remove(&blame->unclaimed_hunks, i);

	git_oid_cpy(&hunk->final_commit_id, &blame->current_commit);
	git_oid_cpy(&hunk->orig_commit_id, &blame->current_commit);
	git_vector_insert(&blame->hunks, hunk);
}

static git_blame_hunk* find_hunk_by_orig_line(git_blame *blame, size_t orig_line)
{
	git_blame_hunk *hunk = blame->current_hunk;
	size_t i;

	if (hunk &&
	    hunk->orig_start_line_number <= orig_line &&
	    hunk->orig_start_line_number + hunk->lines_in_hunk > orig_line) {
		return hunk;
	}

	/* Asked-for line isn't in the old hunk. If it exists, claim it. */
	if (hunk) {
		claim_hunk(blame, hunk);
	}

	git_vector_foreach(&blame->unclaimed_hunks, i, hunk) {
		if (hunk->orig_start_line_number <= orig_line &&
		    hunk->orig_start_line_number + hunk->lines_in_hunk > orig_line) {
			blame->current_hunk = hunk;
			return hunk;
		}
	}
}



static int trivial_line_cb(
	const git_diff_delta *delta,
	const git_diff_range *range,
	char line_origin,
	const char *content,
	size_t content_len,
	void *payload)
{
	git_blame *blame = (git_blame*)payload;
	char *str = git__substrdup(content, content_len);
	if (!blame->trivial_file_match) return 0;
	printf("    %c %zu %s", line_origin, blame->current_line, str);

	/* Update context vars */
	blame->current_delta = delta;
	blame->current_range = range;

	if (line_origin == GIT_DIFF_LINE_ADDITION) {
		git_blame_hunk *hunk = find_hunk_by_orig_line(blame, blame->current_line);
		blame->current_line++;
		adjust_hunk_lines(&blame->unclaimed_hunks, blame->current_line, -1);
	} else if (line_origin == GIT_DIFF_LINE_DELETION) {
		adjust_hunk_lines(&blame->unclaimed_hunks, blame->current_line, 1);
	}

	blame->last_line = blame->current_line;

	return 0;
}

static int trivial_match(git_diff_list *diff, git_blame *blame)
{
	int error = git_diff_foreach(diff, trivial_file_cb, trivial_hunk_cb, trivial_line_cb, blame);
	claim_hunk(blame, blame->current_hunk);
	blame->current_hunk = NULL;
	return error;
}

static int walk_and_mark(git_blame *blame, git_revwalk *walk)
{
	git_oid oid;
	int error;

	dump_hunks(blame);

	while (!(error = git_revwalk_next(&oid, walk))) {
		git_commit *commit = NULL,
					  *parent = NULL;
		git_tree *committree = NULL,
					*parenttree = NULL;
		git_diff_list *diff = NULL;
		git_diff_options diffopts = GIT_DIFF_OPTIONS_INIT;
		git_diff_find_options diff_find_opts = GIT_DIFF_FIND_OPTIONS_INIT;
		char *paths[1];

		if ((error = git_commit_lookup(&commit, blame->repository, &oid)) < 0)
			break;

		/* TODO: consider merge commits */
		if (git_commit_parentcount(commit) > 1)
			continue;

		error = git_commit_parent(&parent, commit, 0);
		if (error != 0 && error != GIT_ENOTFOUND)
			goto cleanup;

		/* Get the trees from this commit and its parent */
		if ((error = git_commit_tree(&committree, commit)) < 0)
			goto cleanup;
		if (parent && ((error = git_commit_tree(&parenttree, parent)) < 0))
			goto cleanup;

		/* Configure the diff */
		diffopts.context_lines = 0;
		/*diffopts.pathspec.strings = (char**)&blame->path;*/
		/*diffopts.pathspec.count = 1;*/

		/* Generate a diff between the two trees */
		if ((error = git_diff_tree_to_tree(&diff, blame->repository, parenttree, committree, &diffopts)) < 0)
			goto cleanup;

		/* Let diff find file moves */
		if (blame->options.flags & GIT_BLAME_TRACK_FILE_RENAMES)
			if ((error = git_diff_find_similar(diff, &diff_find_opts)) < 0)
				goto cleanup;

		git_oid_cpy(&blame->current_commit, &oid);

		/* Trivial matching */
		{
			char str[41] = {0};
			git_oid_fmt(str, &oid);
			printf("Rev %s\n", str);
		}
		if ((error = trivial_match(diff, blame)) < 0)
			goto cleanup;
		git_vector_sort(&blame->hunks);

cleanup:
		git_tree_free(committree);
		git_tree_free(parenttree);
		git_commit_free(commit);
		git_commit_free(parent);
		git_diff_list_free(diff);
		if (error != 0) break;
	}

	dump_hunks(blame);

	if (error == GIT_ITEROVER)
		error = 0;
	return error;
}

static int load_blob(git_blame *blame, git_repository *repo, git_oid *commit_id, const char *path)
{
	int retval = -1;
	git_commit *commit = NULL;
	git_tree *tree = NULL;
	git_tree_entry *tree_entry = NULL;
	git_object *obj = NULL;

	if (((retval = git_commit_lookup(&commit, repo, commit_id)) < 0) ||
	    ((retval = git_commit_tree(&tree, commit)) < 0) ||
	    ((retval = git_tree_entry_bypath(&tree_entry, tree, path)) < 0) ||
	    ((retval = git_tree_entry_to_object(&obj, repo, tree_entry)) < 0) ||
	    ((retval = git_object_type(obj)) != GIT_OBJ_BLOB))
		goto cleanup;
	blame->final_blob = (git_blob*)obj;

	prepare_lines(blame);

cleanup:
	git_tree_entry_free(tree_entry);
	git_tree_free(tree);
	git_commit_free(commit);
	return retval;
}


int git_blame_file(
		git_blame **out,
		git_repository *repo,
		const char *path,
		git_blame_options *options)
{
	int error = -1;
	size_t i;
	git_blame_options normOptions = GIT_BLAME_OPTIONS_INIT;
	git_blame *blame = NULL;
	git_revwalk *walk = NULL;

	if (!out || !repo || !path) return -1;
	normalize_options(&normOptions, options, repo);

	blame = git_blame__alloc(repo, normOptions, path);
	if (!blame) return -1;

	/* Set up the revwalk */
	if ((error = git_revwalk_new(&walk, repo)) < 0 ||
		 (error = git_revwalk_push(walk, &normOptions.newest_commit)) < 0)
		goto on_error;
	if (!git_oid_iszero(&normOptions.oldest_commit) &&
		 (error = git_revwalk_hide(walk, &normOptions.oldest_commit)) < 0)
		goto on_error;
	git_revwalk_sorting(walk, GIT_SORT_TIME);

	if ((error = load_blob(blame, repo, &normOptions.newest_commit, path)) < 0)
		goto on_error;
	printf("\n'%s' has %zu lines\n", path, blame->num_lines);

	/* Initial blame hunk - all lines are unknown */
	git_vector_insert(&blame->unclaimed_hunks,
		new_hunk(1, blame->num_lines, 1, blame->path));

	if ((error = walk_and_mark(blame, walk)) < 0)
		goto on_error;

	git_revwalk_free(walk);
	*out = blame;
	return 0;

on_error:
	git_revwalk_free(walk);
	git_blame_free(blame);
	return error;
}

int git_blame_buffer(
		git_blame **out,
		git_blame *reference,
		const char *buffer,
		size_t buffer_len)
{
	git_blame *blame;

	if (!out || !reference || !buffer || !buffer_len)
		return -1;

	blame = git_blame__alloc(reference->repository, reference->options, reference->path);

	/* TODO */

	*out = blame;
	return 0;
}
