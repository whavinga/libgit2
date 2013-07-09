#include "git2/blame.h"
#include "common.h"
#include "vector.h"
#include "diff.h"
#include "array.h"

struct git_blame {
	const char *path;
	git_repository *repository;
	git_blame_options options;

	git_vector hunks;
	git_vector unclaimed_hunks;
	git_vector paths;

	git_blob *final_blob;
	size_t num_lines;
	git_array_t(size_t) line_index;

	git_oid current_commit;
	size_t current_diff_line;
	size_t current_blame_line;
	git_blame_hunk *current_hunk;

	bool trivial_file_match;
};

git_blame *git_blame__alloc(
	git_repository *repo,
	git_blame_options opts,
	const char *path);
