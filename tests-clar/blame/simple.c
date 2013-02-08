#include "clar_libgit2.h"

#include "git2/blame.h"


git_repository *g_repo;

void test_blame_simple__initialize(void)
{
	g_repo = cl_git_sandbox_init("testrepo.git");
}

void test_blame_simple__cleanup(void)
{
	cl_git_sandbox_cleanup();
	g_repo = NULL;
}

void test_blame_simple__foo(void)
{
	git_blame *blame = NULL;
	cl_git_pass(git_blame_file(&blame, g_repo, "readme.txt", NULL));
	git_blame_free(blame);
}

