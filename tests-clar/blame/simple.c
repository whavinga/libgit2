#include "clar_libgit2.h"

#include "blame.h"


static void check_blame_hunk_index(git_blame *blame, int idx, int start_line, int len, const char *commit_id)
{
	const git_blame_hunk *hunk = git_blame_get_hunk_byindex(blame, idx);
	cl_assert(hunk);

	cl_assert_equal_i(hunk->final_start_line_number, start_line);
	cl_assert_equal_i(hunk->lines_in_hunk, len);

	cl_git_pass(git_oid_streq(&hunk->final_commit_id, commit_id));
}

/*
 * $ git blame branch_file.txt
 * c47800c7 (Scott Chacon 2010-05-25 11:58:14 -0700 1) hi
 * a65fedf3 (Scott Chacon 2011-08-09 19:33:46 -0700 2) bye!
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
 * $ git blame b.txt
 * da237394 (Ben Straub 2013-02-12 15:11:30 -0800  1) EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE
 * da237394 (Ben Straub 2013-02-12 15:11:30 -0800  2) EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE
 * da237394 (Ben Straub 2013-02-12 15:11:30 -0800  3) EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE
 * da237394 (Ben Straub 2013-02-12 15:11:30 -0800  4) EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE
 * ^b99f7ac (Ben Straub 2013-02-12 15:10:12 -0800  5) 
 * 63d671eb (Ben Straub 2013-02-12 15:13:04 -0800  6) BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
 * 63d671eb (Ben Straub 2013-02-12 15:13:04 -0800  7) BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
 * 63d671eb (Ben Straub 2013-02-12 15:13:04 -0800  8) BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
 * 63d671eb (Ben Straub 2013-02-12 15:13:04 -0800  9) BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
 * 63d671eb (Ben Straub 2013-02-12 15:13:04 -0800 10) 
 * aa06ecca (Ben Straub 2013-02-12 15:14:46 -0800 11) CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
 * aa06ecca (Ben Straub 2013-02-12 15:14:46 -0800 12) CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
 * aa06ecca (Ben Straub 2013-02-12 15:14:46 -0800 13) CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
 * aa06ecca (Ben Straub 2013-02-12 15:14:46 -0800 14) CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
 * aa06ecca (Ben Straub 2013-02-12 15:14:46 -0800 15) 
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
