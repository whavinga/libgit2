#include "clar_libgit2.h"

#include "blame.h"


static void check_blame_hunk_index(git_blame *blame, int idx, int start_line, int len, const char *commit_id)
{
	const git_blame_hunk *hunk = git_blame_get_hunk_byindex(blame, idx);
	cl_assert(hunk);

	cl_assert_equal_i(hunk->final_start_line_number, start_line);
	cl_assert_equal_i(hunk->lines_in_hunk, len);

	cl_assert_equal_i(0, git_oid_streq(&hunk->final_commit_id, commit_id));
}

/*
 * $ git blame -s branch_file.txt
 * c47800c7 1) hi
 * a65fedf3 2) bye!
 */
void test_blame_simple__trivial_testrepo(void)
{
	git_blame *blame = NULL;
	git_repository *repo = cl_git_sandbox_init("testrepo");
	cl_git_pass(git_blame_file(&blame, repo, "branch_file.txt", NULL));

	cl_assert_equal_i(2, git_blame_get_hunk_count(blame));
	check_blame_hunk_index(blame, 0, 1, 1, "c47800c7266a2be04c571c04d5a6614691ea99bd");
	check_blame_hunk_index(blame, 1, 2, 1, "a65fedf39aefe402d3bb6e24df4d4f5fe4547750");
	git_blame_free(blame);

	cl_git_sandbox_cleanup();
}

/*
 * $ git blame -s b.txt
 * da237394  1) EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE
 * da237394  2) EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE
 * da237394  3) EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE
 * da237394  4) EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE
 * ^b99f7ac  5) 
 * 63d671eb  6) BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
 * 63d671eb  7) BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
 * 63d671eb  8) BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
 * 63d671eb  9) BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
 * 63d671eb 10) 
 * aa06ecca 11) CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
 * aa06ecca 12) CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
 * aa06ecca 13) CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
 * aa06ecca 14) CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
 * aa06ecca 15) 
 */
void test_blame_simple__trivial_blamerepo(void)
{
	git_blame *blame = NULL;
	git_repository *repo = cl_git_sandbox_init("blametest.git");
	cl_git_pass(git_blame_file(&blame, repo, "b.txt", NULL));

	cl_assert_equal_i(4, git_blame_get_hunk_count(blame));
	check_blame_hunk_index(blame, 0,  1, 4, "da237394e6132d20d30f175b9b73c8638fddddda");
	check_blame_hunk_index(blame, 1,  5, 1, "b99f7ac0b88909253d829554c14af488c3b0f3a5");
	check_blame_hunk_index(blame, 2,  6, 5, "63d671eb32d250e4a83766ebbc60e818c1e1e93a");
	check_blame_hunk_index(blame, 3, 11, 5, "aa06ecca6c4ad6432ab9313e556ca92ba4bcf9e9");
	git_blame_free(blame);

	cl_git_sandbox_cleanup();
}

/* TODO: no newline at end of file? */
