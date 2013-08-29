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

/*#define DO_DEBUG*/

GIT__USE_LINEMAP

#ifdef DO_DEBUG
#define DEBUGF(...) printf(__VA_ARGS__)
static char *oidstr(const git_oid *oid)
{
	static char str[10] = {0};
	git_oid_tostr(str, 9, oid);
	return str;
}
static void dump_hunks(git_blame *blame)
{
	size_t i;
	blame_hunk *hunk;
	char *path;

	DEBUGF("----------\n");
	DEBUGF("Paths watched: ");
	git_vector_foreach(&blame->paths, i, path) {
		DEBUGF("%s   ", path);
	}
	DEBUGF("\n");

	git_vector_foreach(&blame->hunks, i, hunk) {
		DEBUGF("CLAIMED: %2d +%d (orig %2d) %s (from %s)\n",
				hunk->final_start_line_number,
				hunk->lines_in_hunk - 1,
				hunk->orig_start_line_number,
				oidstr(&hunk->final_commit_id),
				hunk->orig_path);
	}
	git_vector_foreach(&blame->unclaimed_hunks, i, hunk) {
		khiter_t k;

		DEBUGF("UNCLAIMED: %d +%d (orig %2d)\n",
				hunk->final_start_line_number,
				hunk->lines_in_hunk - 1,
				hunk->orig_start_line_number);
		for (k = kh_begin(hunk->linemap); k != kh_end(hunk->linemap); ++k) {
			if (kh_exist(hunk->linemap, k)) {
				DEBUGF("  %s -> line %u\n", oidstr(kh_key(hunk->linemap, k)), kh_val(hunk->linemap, k));
			}
		}
	}
	DEBUGF("----------\n");
}
#else
#define DEBUGF(...)
#define dump_hunks(x)
#endif

static void linemap_put(blame_linemap *h, const git_oid *k, uint16_t v)
{
	khiter_t pos;
	int err = 0;
	git_oid *newoid = git__calloc(1, sizeof(git_oid));
	git_oid_cpy(newoid, k);

	pos = kh_get(line, h, newoid);
	if (pos == kh_end(h)) {
		DEBUGF("%% Adding %s to linemap, line %u\n", oidstr(newoid), v);
		pos = kh_put(line, h, newoid, &err);
	} else {
		DEBUGF("%% Modifying %s in linemap; was %u, now %u\n", oidstr(newoid), kh_val(h,pos), v);
	}

	if (err >= 0) {
		kh_val(h, pos) = v;
	}
}

/* Returns (uint16_t)-1 if the hash doesn't contain the key */
static uint16_t linemap_pop(blame_linemap *h, const git_oid *k)
{
	khiter_t pos = kh_get_line(h, k);
	uint16_t ret;

	if (pos == kh_end(h)) return (uint16_t)-1;
	ret = kh_val(h, pos);
	kh_del_line(h, pos);
	return ret;
}


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
	blame_hunk *hunk = (blame_hunk*)entry;
	return hunk_search_cmp_helper(key, hunk->final_start_line_number, hunk->lines_in_hunk);
}
static int hunk_byorigline_search_cmp(const void *key, const void *entry)
{
	blame_hunk *hunk = (blame_hunk*)entry;
	return hunk_search_cmp_helper(key, hunk->orig_start_line_number, hunk->lines_in_hunk);
}

static int hunk_sort_cmp_by_start_line(const void *_a, const void *_b)
{
	blame_hunk *a = (blame_hunk*)_a,
						*b = (blame_hunk*)_b;

	return a->final_start_line_number - b->final_start_line_number;
}

static bool hunk_ends_at_or_before_line(blame_hunk *hunk, size_t line)
{
	return line >= (hunk->final_start_line_number + hunk->lines_in_hunk - 1);
}

static bool hunk_starts_at_or_after_line(blame_hunk *hunk, size_t line)
{
	return line <= hunk->final_start_line_number;
}

static bool hunk_includes_origline(blame_hunk *hunk, uint16_t lineno)
{
	return lineno >= hunk->orig_start_line_number &&
		lineno < hunk->orig_start_line_number + hunk->lines_in_hunk;
}

static blame_hunk* new_hunk(uint16_t start, uint16_t lines, uint16_t orig_start, const char *path)
{
	blame_hunk *hunk = git__calloc(1, sizeof(blame_hunk));
	if (!hunk) return NULL;

	hunk->lines_in_hunk = lines;
	hunk->final_start_line_number = start;
	hunk->orig_start_line_number = orig_start;
	hunk->orig_path = path ? git__strdup(path) : NULL;
	hunk->linemap = blame_linemap_alloc();

	return hunk;
}

blame_hunk* git_blame__alloc_hunk()
{
	return new_hunk(0,0,0,NULL);
}

static blame_hunk* dup_hunk(blame_hunk *hunk)
{
	blame_hunk *newhunk = new_hunk(hunk->final_start_line_number, hunk->lines_in_hunk, hunk->orig_start_line_number, hunk->orig_path);
	git_oid_cpy(&newhunk->orig_commit_id, &hunk->orig_commit_id);
	git_oid_cpy(&newhunk->final_commit_id, &hunk->final_commit_id);
	return newhunk;
}

static void free_hunk(blame_hunk *hunk)
{
	blame_linemap_free(hunk->linemap);
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
			blame_hunk *hunk = (blame_hunk*)v->contents[i];
			DEBUGF("Shifting (f) hunk at line %hu by %d to %d\n", hunk->final_start_line_number,
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
			blame_hunk *hunk = (blame_hunk*)v->contents[i];
			hunk->orig_start_line_number += shift_by;
			DEBUGF("Shifting (o) hunk at line %hu by %d to %d\n", hunk->final_start_line_number,
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
	blame_hunk *hunk;
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

static blame_hunk* get_hunk_by_origline(git_vector *v, uint32_t lineno)
{
	size_t i;
	assert(v);

	if (!git_vector_bsearch2( &i, v, hunk_byorigline_search_cmp, &lineno)) {
		return (blame_hunk*)git_vector_get(v,i);
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

static blame_hunk *split_hunk_in_vector(
		git_vector *vec,
		blame_hunk *hunk,
		size_t rel_line,
		bool return_new)
{
	size_t new_line_count;
	blame_hunk *nh;

	/* Don't split if already at a boundary */
	if (rel_line <= 0 ||
	    rel_line >= hunk->lines_in_hunk)
	{
		DEBUGF("Not splitting hunk (%zd +%zd) at line %zd\n",
				hunk->final_start_line_number, hunk->lines_in_hunk-1, rel_line);
		return hunk;
	}

	new_line_count = hunk->lines_in_hunk - rel_line;
	DEBUGF("Splitting hunk at line %lu (orig %lu) (+%zu)\n",
			hunk->final_start_line_number + rel_line,
			hunk->orig_start_line_number + rel_line,
			new_line_count-1);
	nh = new_hunk(hunk->final_start_line_number+rel_line, new_line_count,
			hunk->orig_start_line_number+rel_line, hunk->orig_path);
	git_oid_cpy(&nh->final_commit_id, &hunk->final_commit_id);
	git_oid_cpy(&nh->orig_commit_id, &hunk->orig_commit_id);
	nh->current_score = hunk->current_score;
	hunk->lines_in_hunk -= new_line_count;
	kh_clear_line(hunk->linemap);
	DEBUGF("Got %hu (+%d) and %hu (+%d)\n",
			hunk->final_start_line_number, hunk->lines_in_hunk-1,
			nh->final_start_line_number, nh->lines_in_hunk-1);
	git_vector_insert_sorted(vec, nh, NULL);
	{
		blame_hunk *ret = return_new ? nh : hunk;
		DEBUGF("Returning hunk that starts at %hu (orig %hu)\n",
				ret->final_start_line_number, ret->orig_start_line_number);
		return ret;
	}
}

static void claim_hunk(git_blame *blame, blame_hunk *hunk, const char *orig_path)
{
	size_t i;

	DEBUGF("Claiming hunk for %s\n", oidstr(&blame->current_commit));

	if (!git_vector_search2(&i, &blame->unclaimed_hunks, ptrs_equal_cmp, hunk)) {
		git_vector_remove(&blame->unclaimed_hunks, i);
	}

	git_oid_cpy(&hunk->final_commit_id, &blame->current_commit);
	git_oid_cpy(&hunk->orig_commit_id, &blame->current_commit);
	if (orig_path) hunk->orig_path = git__strdup(orig_path);

	git_vector_insert_sorted(&blame->hunks, hunk, NULL);
	blame->current_hunk = NULL;
	kh_clear_line(hunk->linemap);
	dump_hunks(blame);
}

/*******************************************************************************
 * Blame-passing algorithm
 ******************************************************************************/

static void process_commit_start_passing_blame(git_blame *blame)
{
	blame_hunk *hunk;
	size_t i;

	git_vector_foreach(&blame->unclaimed_hunks, i, hunk) {
		uint16_t recorded_position;

		/* Zero out scores */
		hunk->current_score = 0;
		if (hunk->scored_path) git__free(hunk->scored_path);
		hunk->scored_path = NULL;

		/* Page in expected hunk locations, and clean up the linemap */
		recorded_position = linemap_pop(hunk->linemap, &blame->current_commit);
		if (recorded_position != (uint16_t)-1) hunk->orig_start_line_number = recorded_position;
	}

}

static void process_hunk_start_passing_blame(
		const git_diff_range *range,
		const git_diff_delta *delta,
		git_blame *blame)
{
	blame_hunk *hunk;
	/* Pure insertions have an off-by-one start line */
	size_t i,
	       wedge_line = (range->old_lines == 0) ? range->new_start : range->old_start;

	GIT_UNUSED(delta);

	/* Find a matching hunk? */
	blame->current_hunk = NULL;
	DEBUGF("  Looking for unclaimed hunks to split at orig line %zu\n", wedge_line);
	git_vector_foreach(&blame->unclaimed_hunks, i, hunk) {
		if (hunk_includes_origline(hunk, wedge_line)) {
			DEBUGF("Found one at index %zu\n", i);

			/* Split the hunk if necessary */
			if (!hunk_starts_at_or_after_line(hunk, wedge_line)) {
				hunk = split_hunk_in_vector(&blame->unclaimed_hunks, hunk,
						wedge_line-hunk->orig_start_line_number, true);
			}
		}
	}
	blame->current_diff_line = wedge_line;
}

static int process_diff_line_passing_blame(
		const git_diff_delta *delta,
		const git_diff_range *range,
		char line_origin,
		const char *content,
		size_t content_len,
		git_blame *blame)
{
	size_t i;
	blame_hunk *hunk;

	GIT_UNUSED(delta);
	GIT_UNUSED(range);

#ifdef DO_DEBUG
	{
		char *str = git__substrdup(content, content_len);
		DEBUGF("    %c%3zu %s", line_origin, blame->current_diff_line, str);
		git__free(str);
	}
#endif

	if (line_origin != GIT_DIFF_LINE_ADDITION)
		return 0;

	/* Check all the hunks that expect to be found at this line */
	git_vector_foreach(&blame->unclaimed_hunks, i, hunk) {
		if (hunk->orig_start_line_number == blame->current_diff_line)
		{
			DEBUGF("Checking hunk %zu (%hu + %d)\n", i,
					hunk->final_start_line_number, hunk->lines_in_hunk-1);
			if (!memcmp(raw_line(blame, hunk->final_start_line_number), content, content_len)) {
				hunk->current_score++;
				DEBUGF("YUP score is now %zu\n", hunk->current_score);
				blame->current_hunk = hunk;
				hunk->scored_path = git__strdup(
						delta->old_file.path ?
						delta->old_file.path
						: "");
			} else {
				char *str = git__substrdup(raw_line(blame, hunk->final_start_line_number), 80);
				DEBUGF("NOPE  -> %s…\n", str);
				git__free(str);
			}
		}
	}

	blame->current_diff_line++;
	return 0;
}

static void process_hunk_end_passing_blame(
		const git_diff_range *range,
		const git_diff_delta *delta,
		git_blame *blame)
{
	int shift_amount;

	GIT_UNUSED(range);
	GIT_UNUSED(delta);
	GIT_UNUSED(blame);

	/* Split the hunk at the end if necessary */
	if (blame->current_hunk) {
		blame_hunk *nh = split_hunk_in_vector(&blame->unclaimed_hunks, blame->current_hunk,
				blame->current_diff_line - blame->current_hunk->orig_start_line_number, true);
		DEBUGF("Diff hunk ends before %zu, current hunk ends at %d, nh starts at %zu\n",
				blame->current_diff_line,
				blame->current_hunk->orig_start_line_number + blame->current_hunk->lines_in_hunk,
				nh->final_start_line_number);
		if (nh != blame->current_hunk)
			nh->current_score = 0;
	}

	/* Shift following hunks' expected locations */
	shift_amount = range->old_lines - range->new_lines;
	shift_hunks_by_orig(&blame->unclaimed_hunks, blame->current_diff_line+1, shift_amount);
}

static void process_commit_end_passing_blame(git_blame *blame, git_commit *commit)
{
	/* Hunks with a score equal to the number of parents belong to this commit.
	 * The rest are somebody else's fault. */
	size_t i, parentcount = git_commit_parentcount(commit);

	for (i=0; i<blame->unclaimed_hunks.length;) {
		blame_hunk *hunk = git_vector_get(&blame->unclaimed_hunks, i);
		DEBUGF("Hunk at line %hu (orig %hu) score %zu, %zu parents\n",
				hunk->final_start_line_number, hunk->orig_start_line_number, hunk->current_score, parentcount);
		if (hunk->current_score >= parentcount) {
			/* Claim this hunk for this commit */
			DEBUGF("!!! Hunk at final line %u belongs to %s\n",
					hunk->final_start_line_number, oidstr(&blame->current_commit));
			claim_hunk(blame, hunk, hunk->scored_path);
			continue;
		}

		/* Page the expected location of this hunk into the linemap */
		linemap_put(hunk->linemap, &blame->parent_commit, hunk->orig_start_line_number);
		i++;
	}
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

		process_hunk_start_passing_blame(range, delta, blame);

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

			error = process_diff_line_passing_blame(delta, range, line_origin, content,
					content_len, blame);
		}

		process_hunk_end_passing_blame(range, delta, blame);
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
		add_if_not_present(&blame->paths, delta->old_file.path);

		/* now that we know we're interested, generate the text diff */
		if ((error = git_diff_get_patch(&patch, &delta, diff, i)) < 0)
			break;

		error = process_patch(patch, blame);
		git_diff_patch_free(patch);
		if (error < 0)
			break;
	}

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
		size_t parentcount, i;

		DEBUGF("Rev %s\n", oidstr(&oid));

		git_oid_cpy(&blame->current_commit, &oid);
		if ((error = git_commit_lookup(&commit, blame->repository, &oid)) < 0)
			break;

		parentcount = git_commit_parentcount(commit);
		DEBUGF("%zu PARENTS\n", parentcount);

		process_commit_start_passing_blame(blame);

		for (i=0; i<parentcount; i++) {
			error = git_commit_parent(&parent, commit, i);
			if (error != 0 && error != GIT_ENOTFOUND)
				goto cleanup;

			if (parent)
				git_oid_cpy(&blame->parent_commit, git_commit_id(parent));
			if (!parent) git__memzero(&blame->parent_commit, sizeof(git_oid));

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

			error = process_diff(diff, blame);
			dump_hunks(blame);
		}
		process_commit_end_passing_blame(blame, commit);

cleanup:
		git_tree_free(committree);
		git_tree_free(parenttree);
		git_commit_free(commit);
		git_commit_free(parent);
		git_diff_list_free(diff);
		if (blame->unclaimed_hunks.length == 0 || error != 0)
			break;
	}

	/* Attribute dangling hunks to oldest commit in the range */
	while (blame->unclaimed_hunks.length > 0) {
		blame_hunk *hunk = (blame_hunk*)git_vector_get(&blame->unclaimed_hunks, 0);
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
	GITERR_CHECK_ALLOC(blame);

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
	DEBUGF("\n============\n'%s' has %zu lines\n", path, blame->num_lines);

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

static bool hunk_is_bufferblame(blame_hunk *hunk)
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
	blame->current_hunk = (blame_hunk*)git_blame_get_hunk_byline(blame, wedge_line);
	if (!hunk_starts_at_or_after_line(blame->current_hunk, wedge_line)){
		blame->current_hunk = split_hunk_in_vector(&blame->hunks, blame->current_hunk,
				wedge_line - blame->current_hunk->orig_start_line_number, true);
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
		    hunk_ends_at_or_before_line(blame->current_hunk, blame->current_diff_line)) {
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
			size_t i;
			DEBUGF("Removing empty hunk at final line %hu\n",
					blame->current_hunk->final_start_line_number);
			shift_base--;
			if (!git_vector_search2(&i, &blame->hunks, ptrs_equal_cmp, blame->current_hunk)) {
				git_vector_remove(&blame->hunks, i);
				free_hunk(blame->current_hunk);
				blame->current_hunk = (blame_hunk*)git_blame_get_hunk_byindex(blame, i);
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
	blame_hunk *hunk;

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
