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
#include "git2/blob.h"
#include "util.h"
#include "repository.h"

/*#define DO_HUNK_SHIFT*/
/*#define DO_DEBUG*/

#ifdef DO_DEBUG
#define DEBUGF(...) printf(__VA_ARGS__)
static void dump_hunks(git_blame *blame)
{
	size_t i;
	git_blame_hunk *hunk;
	char str[41] = {0};
	char *path;

	DEBUGF("----------\n");
	DEBUGF("Paths watched: ");
	git_vector_foreach(&blame->paths, i, path) {
		DEBUGF("%s   ", path);
	}
	DEBUGF("\n");

	git_vector_foreach(&blame->hunks, i, hunk) {
		git_oid_tostr(str, 9, &hunk->final_commit_id);
		DEBUGF("CLAIMED: %2d +%d (orig %2d) %s (from %s)\n",
				hunk->final_start_line_number,
				hunk->lines_in_hunk - 1,
				hunk->orig_start_line_number,
				str,
				hunk->orig_path);
	}
	git_vector_foreach(&blame->unclaimed_hunks, i, hunk) {
		DEBUGF("UNCLAIMED: %d +%d (orig %2d)\n",
				hunk->final_start_line_number,
				hunk->lines_in_hunk - 1,
				hunk->orig_start_line_number);
	}
	DEBUGF("----------\n");
}
#else
#define DEBUGF(...)
#define dump_hunks(x)
#endif

static int hunk_search_cmp_helper(const void *key, size_t start_line, size_t num_lines)
{
	uint32_t lineno = *(size_t*)key;
	if (lineno < start_line)
		return -1;
	if (lineno >= ((uint32_t)start_line + num_lines))
		return 1;
	return 0;
}
static int hunk_byfinalline_search_cmp(const void *key, const void *entry)
{
	git_blame_hunk *hunk = (git_blame_hunk*)entry;
	return hunk_search_cmp_helper(key, hunk->final_start_line_number, hunk->lines_in_hunk);
}
static int hunk_byorigline_search_cmp(const void *key, const void *entry)
{
	git_blame_hunk *hunk = (git_blame_hunk*)entry;
	return hunk_search_cmp_helper(key, hunk->orig_start_line_number, hunk->lines_in_hunk);
}

static int hunk_sort_cmp_by_start_line(const void *_a, const void *_b)
{
	git_blame_hunk *a = (git_blame_hunk*)_a,
						*b = (git_blame_hunk*)_b;

	return a->final_start_line_number - b->final_start_line_number;
}

static bool line_is_at_end_of_hunk(size_t line, git_blame_hunk *hunk)
{
	return line >= (hunk->final_start_line_number + hunk->lines_in_hunk - 1);
}

static bool line_is_at_start_of_hunk(size_t line, git_blame_hunk *hunk)
{
	return line <= hunk->final_start_line_number;
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

static git_blame_hunk* dup_hunk(git_blame_hunk *hunk)
{
	git_blame_hunk *newhunk = new_hunk(hunk->final_start_line_number, hunk->lines_in_hunk, hunk->orig_start_line_number, hunk->orig_path);
	git_oid_cpy(&newhunk->orig_commit_id, &hunk->orig_commit_id);
	git_oid_cpy(&newhunk->final_commit_id, &hunk->final_commit_id);
	return newhunk;
}

static void free_hunk(git_blame_hunk *hunk)
{
	git__free((void*)hunk->orig_path);
	git__free(hunk);
}

/* Starting with the hunk that includes start_line, shift all following hunks'
 * final_start_line by shift_by lines */
static void shift_hunks_by_final(git_vector *v, size_t start_line, int shift_by)
{
	size_t i;

	if (!git_vector_bsearch2( &i, v, hunk_byfinalline_search_cmp, &start_line)) {
		for (; i < v->length; i++) {
			git_blame_hunk *hunk = (git_blame_hunk*)v->contents[i];
			DEBUGF("Shifting (f) hunk at line %zu by %d to %zu\n", hunk->final_start_line_number,
					shift_by, hunk->final_start_line_number + shift_by);
			hunk->final_start_line_number += shift_by;
		}
	}
}
/* Starting with the hunk that includes start_line, shift all following hunks'
 * orig_start_line by shift_by lines */
static void shift_hunks_by_orig(git_vector *v, size_t start_line, int shift_by)
{
	size_t i;

	if (!git_vector_bsearch2( &i, v, hunk_byorigline_search_cmp, &start_line)) {
		for (; i < v->length; i++) {
			git_blame_hunk *hunk = (git_blame_hunk*)v->contents[i];
			hunk->orig_start_line_number += shift_by;
			DEBUGF("Shifting (o) hunk at line %zu by %d to %zu\n", hunk->final_start_line_number,
					shift_by, hunk->orig_start_line_number);
		}
	}
}

static int paths_on_dup(void **old, void *new)
{
	GIT_UNUSED(old);
	GIT_UNUSED(new);
	return -1;
}

static int paths_cmp(const void *a, const void *b) { return git__strcmp((char*)a, (char*)b); }
static void add_if_not_present(git_vector *v, const char *value)
{
	git_vector_insert_sorted(v, (void*)git__strdup(value), paths_on_dup);
}

git_blame* git_blame__alloc(
	git_repository *repo,
	git_blame_options opts,
	const char *path)
{
	git_blame *gbr = (git_blame*)git__calloc(1, sizeof(git_blame));
	if (!gbr) {
		giterr_set_oom();
		return NULL;
	}
	git_vector_init(&gbr->hunks, 8, hunk_sort_cmp_by_start_line);
	git_vector_init(&gbr->unclaimed_hunks, 8, hunk_sort_cmp_by_start_line);
	git_vector_init(&gbr->paths, 8, paths_cmp);
	gbr->repository = repo;
	gbr->options = opts;
	gbr->path = git__strdup(path);
	git_vector_insert(&gbr->paths, git__strdup(path));
	gbr->final_blob = NULL;
	return gbr;
}

void git_blame_free(git_blame *blame)
{
	size_t i;
	git_blame_hunk *hunk;
	char *path;

	if (!blame) return;

	git_vector_foreach(&blame->hunks, i, hunk)
		free_hunk(hunk);
	git_vector_free(&blame->hunks);

	git_vector_foreach(&blame->unclaimed_hunks, i, hunk)
		free_hunk(hunk);
	git_vector_free(&blame->unclaimed_hunks);

	git_vector_foreach(&blame->paths, i, path)
		git__free(path);
	git_vector_free(&blame->paths);

	git_array_clear(blame->line_index);

	git__free((void*)blame->path);
	git_blob_free(blame->final_blob);
	git__free(blame);
}

/*
 * Construct a list of char indices for where lines begin
 * Adapted from core git:
 * https://github.com/gitster/git/blob/be5c9fb9049ed470e7005f159bb923a5f4de1309/builtin/blame.c#L1760-L1789
 */
static int index_blob_lines(git_blame *blame)
{
	const char *final_buf = git_blob_rawcontent(blame->final_blob);
	const char *buf = final_buf;
	git_off_t len = git_blob_rawsize(blame->final_blob);
	int num = 0, incomplete = 0, bol = 1;
	size_t *i;

	if (len && buf[len-1] != '\n')
		incomplete++; /* incomplete line at the end */
	while (len--) {
		if (bol) {
			i = git_array_alloc(blame->line_index);
			GITERR_CHECK_ALLOC(i);
			*i = buf - final_buf;
			bol = 0;
		}
		if (*buf++ == '\n') {
			num++;
			bol = 1;
		}
	}
	i = git_array_alloc(blame->line_index);
	GITERR_CHECK_ALLOC(i);
	*i = buf - final_buf;
	blame->num_lines = num + incomplete;
	return 0;
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

	if (!git_vector_bsearch2( &i, &blame->hunks, hunk_byfinalline_search_cmp, &lineno)) {
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

	memcpy(out, in, sizeof(git_blame_options));

	/* No newest_commit => HEAD */
	if (git_oid_iszero(&out->newest_commit)) {
		git_reference_name_to_id(&out->newest_commit, repo, "HEAD");
	}
}


/*******************************************************************************
 * Trivial blaming
 *******************************************************************************/

static int ptrs_equal_cmp(const void *a, const void *b) {
	return
		a < b ? -1 :
		a > b ? 1  :
		0;
}

static const char* raw_line(git_blame *blame, size_t i)
{
	return ((const char*)git_blob_rawcontent(blame->final_blob)) +
		*git_array_get(blame->line_index, i-1);
}

static git_blame_hunk *split_hunk_in_vector(git_vector *vec, git_blame_hunk *hunk, size_t at_line, bool return_new)
{
	size_t new_line_count;
	git_blame_hunk *nh;

	/* Don't split if already at a boundary */
	if (line_is_at_start_of_hunk(at_line, hunk) ||
	    line_is_at_end_of_hunk(at_line-1, hunk))
	{
		DEBUGF("Not splitting hunk (%zd +%zd) at line %zd\n",
				hunk->final_start_line_number, hunk->lines_in_hunk-1, at_line);
		return hunk;
	}

	new_line_count = hunk->final_start_line_number + hunk->lines_in_hunk - at_line;
	DEBUGF("Splitting hunk at line %zu (+%zu)\n", at_line, new_line_count-1);
	nh = new_hunk(at_line, new_line_count, at_line, hunk->orig_path);
	git_oid_cpy(&nh->final_commit_id, &hunk->final_commit_id);
	git_oid_cpy(&nh->orig_commit_id, &hunk->orig_commit_id);
	hunk->lines_in_hunk -= (new_line_count);
	DEBUGF("Got %zu (+%zu) and %zu (+%zu)\n",
			hunk->final_start_line_number, hunk->lines_in_hunk,
			nh->final_start_line_number, nh->lines_in_hunk);
	git_vector_insert_sorted(vec, nh, NULL);
	{
		git_blame_hunk *ret = return_new ? nh : hunk;
		DEBUGF("Returning hunk that starts at %zu (orig %zu)\n",
				ret->final_start_line_number, ret->orig_start_line_number);
		return ret;
	}
}

static void claim_hunk(git_blame *blame, git_blame_hunk *hunk, const char *orig_path)
{
	size_t i;

#ifdef DO_DEBUG
	{
		char str[41]={0};
		git_oid_tostr(str, 9, &blame->current_commit);
		DEBUGF("Claiming hunk for %s\n", str);
	}
#endif

	if (!git_vector_search2(&i, &blame->unclaimed_hunks, ptrs_equal_cmp, hunk)) {
		git_vector_remove(&blame->unclaimed_hunks, i);
	}

	git_oid_cpy(&hunk->final_commit_id, &blame->current_commit);
	git_oid_cpy(&hunk->orig_commit_id, &blame->current_commit);
	hunk->orig_path = git__strdup(orig_path);

	git_vector_insert_sorted(&blame->hunks, hunk, NULL);
	blame->current_hunk = NULL;
	dump_hunks(blame);
}

static void close_and_claim_current_hunk(git_blame *blame, const char *orig_path)
{
	git_blame_hunk *hunk = blame->current_hunk;

	if (!hunk) {
		DEBUGF("Can't close NULL hunk\n");
		return;
	}

	DEBUGF("Closing hunk at line %zu ('%s')\n", blame->current_blame_line, orig_path);

	/* Split this hunk if its end isn't at the current line */
	hunk = split_hunk_in_vector(&blame->unclaimed_hunks, blame->current_hunk, blame->current_blame_line+1, false);

	claim_hunk(blame, blame->current_hunk, orig_path);
}

static void match_line(git_blame *blame, const char *line, size_t len, const char *orig_path)
{
	git_blame_hunk *hunk = blame->current_hunk;
	const char *raw = (blame->current_blame_line == 0 ? NULL : raw_line(blame, blame->current_blame_line));
	size_t i, j;

	/* First, try the current hunk's current line. */
	if (hunk && raw && !memcmp(raw, line, len)) {
		DEBUGF("•\n");
		return;
	}

	/* Blank lines shouldn't be searched for too hard */
	if (len == 1) {
		DEBUGF("Blank line, giving up\n");
		return;
	}

	/* Do a linear search for a matching line in all unclaimed hunks */
	git_vector_foreach(&blame->unclaimed_hunks, i, hunk) {
		for (j = hunk->final_start_line_number;
		     j < (size_t)hunk->final_start_line_number + hunk->lines_in_hunk;
		     j++)
		{
			if (!memcmp(raw_line(blame, j), line, len)) {
				close_and_claim_current_hunk(blame, orig_path);

				DEBUGF("matched at line %zu\n", j);
				blame->current_hunk = hunk;
				blame->current_blame_line = j;
				blame->current_hunk = split_hunk_in_vector(&blame->unclaimed_hunks, blame->current_hunk, j, true);
				blame->current_hunk->orig_start_line_number = blame->current_diff_line;
				return;
			}
		}
	}

	/* If we get this far, we didn't find a matching line. */
	DEBUGF("***Couldn't find string anywhere!\n");
	close_and_claim_current_hunk(blame, orig_path);
}

static int process_diff_line_trivial(
	const git_diff_delta *delta,
	const git_diff_range *range,
	char line_origin,
	const char *content,
	size_t content_len,
	void *payload)
{
	git_blame *blame = (git_blame*)payload;
	git_blame_hunk *curhunk = blame->current_hunk;

#ifdef DO_DEBUG
	{
		char *str = git__substrdup(content, content_len);
		DEBUGF("    %c%3zu %s", line_origin, blame->current_diff_line, str);
		git__free(str);
	}
#endif

	if (line_origin == GIT_DIFF_LINE_ADDITION)
		match_line(blame, content, content_len, delta->new_file.path);
	/* DEBUGF("(done with line %zu of %d)\n", 
			blame->current_diff_line,
			range->new_start + range->new_lines); */

	/* End of blame hunk or diff hunk? Close it off and claim it */
	if ((blame->current_diff_line >= (size_t)(range->new_start + range->new_lines - 1)) ||
	    (curhunk && (blame->current_blame_line >=
	         (size_t)(curhunk->final_start_line_number + curhunk->lines_in_hunk - 1))))
	{
		close_and_claim_current_hunk(blame, delta->new_file.path);
	}

	if (line_origin == GIT_DIFF_LINE_ADDITION) {
		blame->current_blame_line++;
		blame->current_diff_line++;
	}

	return 0;
}

/*******************************************************************************
 * Hunk-shift matching
 ******************************************************************************/

static void process_hunk_start_hunk_shift(
		const git_diff_range *range,
		const git_diff_delta *delta,
		git_blame *blame)
{
	/* Pure insertions have an off-by-one start line */
	size_t i,
			 wedge_line = (range->old_lines == 0) ? range->new_start : range->old_start;
	blame->current_diff_line = wedge_line;

	GIT_UNUSED(delta);

	DEBUGF("  Looking for unclaimed hunk at line %zu\n", wedge_line);
	if (!git_vector_bsearch2(&i, &blame->unclaimed_hunks, hunk_byorigline_search_cmp, &wedge_line)) {
		DEBUGF("  Found one at index %zu\n", i);
		blame->current_hunk = (git_blame_hunk*)git_vector_get(&blame->unclaimed_hunks, i);
		if (!line_is_at_start_of_hunk(wedge_line, blame->current_hunk)){
			blame->current_hunk = split_hunk_in_vector(&blame->unclaimed_hunks,
					blame->current_hunk, wedge_line, true);
		}
		blame->current_blame_line = blame->current_hunk->final_start_line_number;
	} else {
		DEBUGF("  No unclaimed hunks match that line\n");
		blame->current_hunk = NULL;
	}
}

static int process_diff_line_hunk_shift(
	const git_diff_delta *delta,
	const git_diff_range *range,
	char line_origin,
	const char *content,
	size_t content_len,
	git_blame *blame)
{
	size_t next_hunk_start;
	git_blame_hunk *hunk;

#ifdef DO_DEBUG
	{
		char *str = git__substrdup(content, content_len);
		DEBUGF("    %c%3zu %s", line_origin, blame->current_diff_line, str);
		git__free(str);
	}
#endif

	GIT_UNUSED(delta);
	GIT_UNUSED(range);
	GIT_UNUSED(content);
	GIT_UNUSED(content_len);

	if (!blame->current_hunk) return 0;
	hunk = blame->current_hunk;

	next_hunk_start = hunk->orig_start_line_number + hunk->lines_in_hunk + 1;
	if (line_origin == GIT_DIFF_LINE_ADDITION) {
		shift_hunks_by_orig(&blame->unclaimed_hunks, next_hunk_start, -1);
		blame->current_blame_line++;
		blame->current_diff_line++;
	} else if (line_origin == GIT_DIFF_LINE_DELETION) {
		shift_hunks_by_orig(&blame->unclaimed_hunks, next_hunk_start , 1);
	}

	return 0;
}

static void process_hunk_end_hunk_shift(
		const git_diff_range *range,
		const git_diff_delta *delta,
		git_blame *blame)
{
	GIT_UNUSED(range);
	blame->current_blame_line--;
	close_and_claim_current_hunk(blame, delta->old_file.path);
}

/*******************************************************************************
 * Plumbing
 ******************************************************************************/

static int process_patch(git_diff_patch *patch, git_blame *blame)
{
	int error = 0;
	size_t i, num_hunks = git_diff_patch_num_hunks(patch);
	const git_diff_delta *delta = git_diff_patch_delta(patch);

	for (i=0; i<num_hunks; ++i) {
		const git_diff_range *range;
		size_t lines, j;

		if ((error = git_diff_patch_get_hunk(&range, NULL, NULL, &lines, patch, i)) < 0)
			goto cleanup;

		DEBUGF("  Hunk: %s (%d-%d) <- %s (%d-%d)\n",
				delta->new_file.path,
				range->new_start, range->new_start + max(0, range->new_lines - 1),
				delta->old_file.path,
				range->old_start, range->old_start + max(0, range->old_lines - 1));
		blame->current_diff_line = range->new_start;

#ifdef DO_HUNK_SHIFT
		process_hunk_start_hunk_shift(range, delta, blame);
#endif

		for (j=0; j<lines; ++j) {
			char line_origin;
			const char *content;
			size_t content_len;
			int old_lineno;
			int new_lineno;
			if ((error = git_diff_patch_get_line_in_hunk(
							&line_origin, &content, &content_len, &old_lineno, &new_lineno,
							patch, i, j)) < 0)
				goto cleanup;

#ifdef DO_HUNK_SHIFT
			error = process_diff_line_hunk_shift(delta, range, line_origin, content, content_len, blame);
#else
			error = process_diff_line_trivial(delta, range, line_origin, content, content_len, blame);
#endif
		}

#ifdef DO_HUNK_SHIFT
		process_hunk_end_hunk_shift(range, delta, blame);
#endif

	}

cleanup:
	return error;
}

static int process_diff(git_diff_list *diff, git_blame *blame)
{
	int error = 0;
	size_t i, max_i = git_diff_num_deltas(diff);

	for (i=0; i<max_i; ++i) {
		const git_diff_delta *delta;
		git_diff_patch *patch;

		/* just get the delta to see if we care about this entry */
		if ((error = git_diff_get_patch(NULL, &delta, diff, i)) < 0)
			break;

		/* try to look up filename in the list of paths */
		if (git_vector_bsearch(NULL, &blame->paths, delta->new_file.path) != 0)
			continue;

		/* track renames */
#ifdef DO_DEBUG
		if (git_vector_bsearch(NULL, &blame->paths, delta->old_file.path) != 0)
			DEBUGF("===> Now tracking through %s\n", delta->old_file.path);
#endif
		add_if_not_present(&blame->paths, delta->old_file.path);

		/* now that we know we're interested, generate the text diff */
		if ((error = git_diff_get_patch(&patch, &delta, diff, i)) < 0)
			break;

		error = process_patch(patch, blame);
		git_diff_patch_free(patch);
		if (error < 0)
			break;
	}

	blame->current_hunk = NULL;
	return error;
}

static int walk_and_mark(git_blame *blame, git_revwalk *walk)
{
	git_oid oid;
	int error;

	while (!(error = git_revwalk_next(&oid, walk))) {
		git_commit *commit = NULL,
		           *parent = NULL;
		git_tree *committree = NULL,
		         *parenttree = NULL;
		git_diff_list *diff = NULL;
		git_diff_options diffopts = GIT_DIFF_OPTIONS_INIT;
		git_diff_find_options diff_find_opts = GIT_DIFF_FIND_OPTIONS_INIT;

#ifdef DO_DEBUG
		{
			char str[41] = {0};
			git_oid_tostr(str, 9, &oid);
			DEBUGF("Rev %s\n", str);
		}
#endif

		if ((error = git_commit_lookup(&commit, blame->repository, &oid)) < 0)
			break;

		/* TODO: consider merge commits */
		if (git_commit_parentcount(commit) > 1) {
			continue;
		}

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

		/* Check to see if files we're interested in have changed */
		diffopts.pathspec.count = blame->paths.length;
		diffopts.pathspec.strings = (char**)blame->paths.contents;
		if ((error = git_diff_tree_to_tree(&diff, blame->repository, parenttree, committree, &diffopts)) < 0)
			goto cleanup;

		/* Generate a full diff between the two trees */
		if (git_diff_num_deltas(diff) > 0) {
			git_diff_list_free(diff);
			diffopts.pathspec.count = 0;
			if ((error = git_diff_tree_to_tree(&diff, blame->repository, parenttree, committree, &diffopts)) < 0)
				goto cleanup;
		}

		/* Let diff find file moves */
		diff_find_opts.flags = GIT_DIFF_FIND_RENAMES;
		if ((error = git_diff_find_similar(diff, &diff_find_opts)) < 0)
			goto cleanup;

		git_oid_cpy(&blame->current_commit, &oid);

		error = process_diff(diff, blame);

cleanup:
		git_tree_free(committree);
		git_tree_free(parenttree);
		git_commit_free(commit);
		git_commit_free(parent);
		git_diff_list_free(diff);
		if (error != 0) break;
	}

	/* Attribute dangling hunks to oldest commit in the range */
	while (blame->unclaimed_hunks.length > 0) {
		git_blame_hunk *hunk = (git_blame_hunk*)git_vector_get(&blame->unclaimed_hunks, 0);
		claim_hunk(blame, hunk, blame->path);
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
		 ((retval = git_object_lookup_bypath(&obj, (git_object*)commit, path, GIT_OBJ_BLOB)) < 0) ||
	    ((retval = git_object_type(obj)) != GIT_OBJ_BLOB))
		goto cleanup;
	blame->final_blob = (git_blob*)obj;

	index_blob_lines(blame);

cleanup:
	git_tree_entry_free(tree_entry);
	git_tree_free(tree);
	git_commit_free(commit);
	return retval;
}

/*******************************************************************************
 * File blaming
 ******************************************************************************/

int git_blame_file(
		git_blame **out,
		git_repository *repo,
		const char *path,
		git_blame_options *options)
{
	int error = -1;
	git_blame_options normOptions = GIT_BLAME_OPTIONS_INIT;
	git_blame *blame = NULL;
	git_revwalk *walk = NULL;

	assert(out && repo && path);
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
	DEBUGF("\n'%s' has %zu lines\n", path, blame->num_lines);

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

/*******************************************************************************
 * Buffer blaming
 *******************************************************************************/

static bool hunk_is_bufferblame(git_blame_hunk *hunk)
{
	return git_oid_iszero(&hunk->final_commit_id);
}

static int buffer_hunk_cb(
	const git_diff_delta *delta,
	const git_diff_range *range,
	const char *header,
	size_t header_len,
	void *payload)
{
	git_blame *blame = (git_blame*)payload;
	size_t wedge_line;

	GIT_UNUSED(delta);
	GIT_UNUSED(header);
	GIT_UNUSED(header_len);

	wedge_line = (range->old_lines == 0) ? range->new_start : range->old_start;
	blame->current_diff_line = wedge_line;

	/* If this hunk doesn't start between existing hunks, split a hunk up so it does */
	blame->current_hunk = (git_blame_hunk*)git_blame_get_hunk_byline(blame, wedge_line);
	if (!line_is_at_start_of_hunk(wedge_line, blame->current_hunk)){
		blame->current_hunk = split_hunk_in_vector(&blame->hunks, blame->current_hunk, wedge_line, true);
		dump_hunks(blame);
	}

	return 0;
}

static int buffer_line_cb(
	const git_diff_delta *delta,
	const git_diff_range *range,
	char line_origin,
	const char *content,
	size_t content_len,
	void *payload)
{
	git_blame *blame = (git_blame*)payload;

	GIT_UNUSED(delta);
	GIT_UNUSED(range);
	GIT_UNUSED(content);
	GIT_UNUSED(content_len);

#ifdef DO_DEBUG
	{
		char *str = git__substrdup(content, content_len);
		DEBUGF("    %c%3zu %s", line_origin, blame->current_diff_line, str);
		git__free(str);
	}
#endif

	if (line_origin == GIT_DIFF_LINE_ADDITION) {
		if (hunk_is_bufferblame(blame->current_hunk) &&
		    line_is_at_end_of_hunk(blame->current_diff_line, blame->current_hunk)) {
			/* Append to the current buffer-blame hunk */
			DEBUGF("Adding line to existing hunk\n");
			blame->current_hunk->lines_in_hunk++;
			shift_hunks_by_final(&blame->hunks, blame->current_diff_line+1, 1);
		} else {
			/* Create a new buffer-blame hunk with this line */
			DEBUGF("Creating a new hunk for this line\n");
			shift_hunks_by_final(&blame->hunks, blame->current_diff_line, 1);
			blame->current_hunk = new_hunk(blame->current_diff_line, 1, 0, blame->path);
			git_vector_insert_sorted(&blame->hunks, blame->current_hunk, NULL);
		}
		blame->current_diff_line++;
	}

	if (line_origin == GIT_DIFF_LINE_DELETION) {
		/* Trim the line from the current hunk; remove it if it's now empty */
		size_t shift_base = blame->current_diff_line + blame->current_hunk->lines_in_hunk+1;

		if (--(blame->current_hunk->lines_in_hunk) == 0) {
			DEBUGF("Removing empty hunk at final line %zu\n",
					blame->current_hunk->final_start_line_number);
			shift_base--;
			size_t i;
			if (!git_vector_search2(&i, &blame->hunks, ptrs_equal_cmp, blame->current_hunk)) {
				git_vector_remove(&blame->hunks, i);
				free_hunk(blame->current_hunk);
				blame->current_hunk = (git_blame_hunk*)git_blame_get_hunk_byindex(blame, i);
			}
			dump_hunks(blame);
		}
		shift_hunks_by_final(&blame->hunks, shift_base, -1);
	}
	return 0;
}

int git_blame_buffer(
		git_blame **out,
		git_blame *reference,
		const char *buffer,
		size_t buffer_len)
{
	git_blame *blame;
	git_diff_options diffopts = GIT_DIFF_OPTIONS_INIT;
	size_t i;
	git_blame_hunk *hunk;

	diffopts.context_lines = 0;

	assert(out && reference && buffer && buffer_len);

	blame = git_blame__alloc(reference->repository, reference->options, reference->path);

	/* Duplicate all of the hunk structures in the reference blame */
	git_vector_foreach(&reference->hunks, i, hunk) {
		git_vector_insert(&blame->hunks, dup_hunk(hunk));
	}

	/* Diff to the reference blob */
	git_diff_blob_to_buffer(reference->final_blob, blame->path,
			buffer, buffer_len, blame->path,
			&diffopts, NULL, buffer_hunk_cb, buffer_line_cb, blame);

	dump_hunks(blame);
	*out = blame;
	return 0;
}
