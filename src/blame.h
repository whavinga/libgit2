#include "git2/blame.h"
#include "common.h"
#include "vector.h"

struct git_blame {
	git_vector hunks;
	git_repository *repository;
	git_blame_options options;
};

git_blame *git_blame__alloc(git_repository *repo, git_blame_options opts);
