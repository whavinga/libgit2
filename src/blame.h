#include "git2/blame.h"
#include "common.h"
#include "vector.h"

struct git_blame {
	const char *path;
	git_vector hunks;
	git_repository *repository;
	git_blame_options options;

	git_blame__line *lines;
	size_t num_lines;

	/* Trivial context */
	size_t current_line;
	git_oid current_commit;
};

git_blame *git_blame__alloc(
	git_repository *repo,
	git_blame_options opts,
	const char *path);
