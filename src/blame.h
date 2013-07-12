#include "git2/blame.h"
#include "common.h"
#include "vector.h"
#include "diff.h"
#include "array.h"
typedef struct blame_hunk {
	/* git_blame_hunk fields */
	uint16_t lines_in_hunk;
	git_oid final_commit_id;
	uint16_t final_start_line_number;
	git_oid orig_commit_id;
	const char *orig_path;
	uint16_t orig_start_line_number;
} blame_hunk;

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
	blame_hunk *current_hunk;
};

git_blame *git_blame__alloc(
	git_repository *repo,
	git_blame_options opts,
	const char *path);

blame_hunk *git_blame__alloc_hunk();

