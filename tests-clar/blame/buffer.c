#include "clar_libgit2.h"

#include "blame.h"

git_repository *g_repo;
git_blame *g_fileblame, *g_bufferblame;

void test_blame_buffer__initialize(void)
{
	cl_git_pass(git_repository_open(&g_repo, cl_fixture("testrepo/.gitted")));
	cl_git_pass(git_blame_file(&g_fileblame, g_repo, "branch_file.txt", NULL));
	g_bufferblame = NULL;
}

void test_blame_buffer__cleanup(void)
{
	git_blame_free(g_fileblame);
	git_blame_free(g_bufferblame);
	git_repository_free(g_repo);
}

/*
 * c47800c7 1) hi
 * 00000000 2) FOO
 * a65fedf3 3) bye!
 */
void test_blame_buffer__added_line(void)
{
	const char *buffer = "hi\nFOO\nbye!\n";

	cl_git_pass(git_blame_buffer(&g_bufferblame, g_fileblame, buffer, strlen(buffer)));
}
