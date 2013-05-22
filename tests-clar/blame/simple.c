#include "clar_libgit2.h"

#include "blame.h"


static void check_blame_hunk_index(git_repository *repo, git_blame *blame, int idx, int start_line, int len, const char *commit_id)
{
	git_object *obj;
	const git_blame_hunk *hunk = git_blame_get_hunk_byindex(blame, idx);
	cl_assert(hunk);

	cl_git_pass(git_revparse_single(&obj, repo, commit_id));

	cl_assert_equal_i(hunk->final_start_line_number, start_line);
	cl_assert_equal_i(hunk->lines_in_hunk, len);

	cl_assert_equal_i(0, git_oid_cmp(&hunk->final_commit_id, git_object_id(obj)));
	git_object_free(obj);
}

/*
 * $ git blame -s branch_file.txt
 * c47800c7 1) hi
 * a65fedf3 2) bye!
 */
void test_blame_simple__trivial_testrepo(void)
{
	git_blame *blame = NULL;
	git_repository *repo;
	cl_git_pass(git_repository_open(&repo, cl_fixture("testrepo/.gitted")));
	cl_git_pass(git_blame_file(&blame, repo, "branch_file.txt", NULL));

	cl_assert_equal_i(2, git_blame_get_hunk_count(blame));
	check_blame_hunk_index(repo, blame, 0, 1, 1, "c47800c7");
	check_blame_hunk_index(repo, blame, 1, 2, 1, "a65fedf3");
	git_blame_free(blame);
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
	git_repository *repo;
	cl_git_pass(git_repository_open(&repo, cl_fixture("blametest.git")));
	cl_git_pass(git_blame_file(&blame, repo, "b.txt", NULL));

	cl_assert_equal_i(4, git_blame_get_hunk_count(blame));
	check_blame_hunk_index(repo, blame, 0,  1, 4, "da237394");
	check_blame_hunk_index(repo, blame, 1,  5, 1, "b99f7ac0");
	check_blame_hunk_index(repo, blame, 2,  6, 5, "63d671eb");
	check_blame_hunk_index(repo, blame, 3, 11, 5, "aa06ecca");
	git_blame_free(blame);
}


/*
 * $ git blame -s 359fc2d -- include/git2.h
 * d12299fe src/git.h       1) / *
 * 359fc2d2 include/git2.h  2)  * Copyright (C) the libgit2 contributors. All rights reserved.
 * d12299fe src/git.h       3)  *
 * bb742ede include/git2.h  4)  * This file is part of libgit2, distributed under the GNU GPL v2 with
 * bb742ede include/git2.h  5)  * a Linking Exception. For full terms see the included COPYING file.
 * d12299fe src/git.h       6)  * /
 * d12299fe src/git.h       7) 
 * d12299fe src/git.h       8) #ifndef INCLUDE_git_git_h__
 * d12299fe src/git.h       9) #define INCLUDE_git_git_h__
 * d12299fe src/git.h      10) 
 * 96fab093 include/git2.h 11) #include "git2/version.h"
 * 9d1dcca2 src/git2.h     12) 
 * 44908fe7 src/git2.h     13) #include "git2/common.h"
 * a15c550d include/git2.h 14) #include "git2/threads.h"
 * 44908fe7 src/git2.h     15) #include "git2/errors.h"
 * d12299fe src/git.h      16) 
 * 44908fe7 src/git2.h     17) #include "git2/types.h"
 * d12299fe src/git.h      18) 
 * 44908fe7 src/git2.h     19) #include "git2/oid.h"
 * 638c2ca4 src/git2.h     20) #include "git2/signature.h"
 * 44908fe7 src/git2.h     21) #include "git2/odb.h"
 * d12299fe src/git.h      22) 
 * 44908fe7 src/git2.h     23) #include "git2/repository.h"
 * 44908fe7 src/git2.h     24) #include "git2/revwalk.h"
 * bf787bd8 include/git2.h 25) #include "git2/merge.h"
 * 0984c876 include/git2.h 26) #include "git2/graph.h"
 * 2f8a8ab2 src/git2.h     27) #include "git2/refs.h"
 * 27df4275 include/git2.h 28) #include "git2/reflog.h"
 * a346992f include/git2.h 29) #include "git2/revparse.h"
 * d12299fe src/git.h      30) 
 * 44908fe7 src/git2.h     31) #include "git2/object.h"
 * 44908fe7 src/git2.h     32) #include "git2/blob.h"
 * 44908fe7 src/git2.h     33) #include "git2/commit.h"
 * 44908fe7 src/git2.h     34) #include "git2/tag.h"
 * 44908fe7 src/git2.h     35) #include "git2/tree.h"
 * 65b09b1d include/git2.h 36) #include "git2/diff.h"
 * d12299fe src/git.h      37) 
 * 44908fe7 src/git2.h     38) #include "git2/index.h"
 * 5d4cd003 include/git2.h 39) #include "git2/config.h"
 * 41fb1ca0 include/git2.h 40) #include "git2/transport.h"
 * 2dc31040 include/git2.h 41) #include "git2/remote.h"
 * 764df57e include/git2.h 42) #include "git2/clone.h"
 * 5280f4e6 include/git2.h 43) #include "git2/checkout.h"
 * 613d5eb9 include/git2.h 44) #include "git2/push.h"
 * d12299fe src/git.h      45) 
 * 111ee3fe include/git2.h 46) #include "git2/attr.h"
 * f004c4a8 include/git2.h 47) #include "git2/ignore.h"
 * 111ee3fe include/git2.h 48) #include "git2/branch.h"
 * 9c82357b include/git2.h 49) #include "git2/refspec.h"
 * d6258deb include/git2.h 50) #include "git2/net.h"
 * b311e313 include/git2.h 51) #include "git2/status.h"
 * 3412391d include/git2.h 52) #include "git2/indexer.h"
 * bfc9ca59 include/git2.h 53) #include "git2/submodule.h"
 * bf477ed4 include/git2.h 54) #include "git2/notes.h"
 * edebceff include/git2.h 55) #include "git2/reset.h"
 * 743a4b3b include/git2.h 56) #include "git2/message.h"
 * 0a32dca5 include/git2.h 57) #include "git2/pack.h"
 * 590fb68b include/git2.h 58) #include "git2/stash.h"
 * bf477ed4 include/git2.h 59) 
 * d12299fe src/git.h      60) #endif
 */
void test_blame_simple__trivial_libgit2(void)
{
	git_repository *repo;
	git_blame *blame;
	git_blame_options opts = GIT_BLAME_OPTIONS_INIT;
	git_object *obj;

	cl_git_pass(git_repository_open(&repo, cl_fixture("../..")));

	cl_git_pass(git_revparse_single(&obj, repo, "359fc2d"));
	git_oid_cpy(&opts.newest_commit, git_object_id(obj));
	git_object_free(obj);

	cl_git_pass(git_blame_file(&blame, repo, "include/git2.h", &opts));

	check_blame_hunk_index(repo, blame,  0,  1, 1, "d12299fe");
	check_blame_hunk_index(repo, blame,  1,  2, 1, "359fc2d2");
	check_blame_hunk_index(repo, blame,  2,  3, 1, "d12299fe");
	check_blame_hunk_index(repo, blame,  3,  4, 2, "bb742ede");
	check_blame_hunk_index(repo, blame,  4,  6, 5, "d12299fe");
	check_blame_hunk_index(repo, blame,  5, 11, 1, "96fab093");
	check_blame_hunk_index(repo, blame,  6, 12, 1, "9d1dcca2");
	check_blame_hunk_index(repo, blame,  7, 13, 1, "44908fe7");
	check_blame_hunk_index(repo, blame,  8, 14, 1, "a15c550d");
	check_blame_hunk_index(repo, blame,  9, 15, 1, "44908fe7");
	check_blame_hunk_index(repo, blame, 10, 16, 1, "d12299fe");
	check_blame_hunk_index(repo, blame, 11, 17, 1, "44908fe7");
	check_blame_hunk_index(repo, blame, 12, 18, 1, "d12299fe");
	check_blame_hunk_index(repo, blame, 13, 19, 1, "44908fe7");
	check_blame_hunk_index(repo, blame, 14, 20, 1, "638c2ca4");
	check_blame_hunk_index(repo, blame, 15, 21, 1, "44908fe7");
	check_blame_hunk_index(repo, blame, 16, 22, 1, "d12299fe");
	check_blame_hunk_index(repo, blame, 17, 23, 2, "44908fe7");
	check_blame_hunk_index(repo, blame, 18, 25, 1, "bf787bd8");
	check_blame_hunk_index(repo, blame, 19, 26, 1, "0984c876");
	check_blame_hunk_index(repo, blame, 20, 27, 1, "2f8a8ab2");
	check_blame_hunk_index(repo, blame, 21, 28, 1, "27df4275");
	check_blame_hunk_index(repo, blame, 22, 29, 1, "a346992f");
	check_blame_hunk_index(repo, blame, 23, 30, 1, "d12299fe");
	check_blame_hunk_index(repo, blame, 24, 31, 5, "44908fe7");
	check_blame_hunk_index(repo, blame, 25, 36, 1, "65b09b1d");
	check_blame_hunk_index(repo, blame, 26, 37, 1, "d12299fe");
	check_blame_hunk_index(repo, blame, 27, 38, 1, "44908fe7");
	check_blame_hunk_index(repo, blame, 28, 39, 1, "5d4cd003");
	check_blame_hunk_index(repo, blame, 29, 40, 1, "41fb1ca0");
	check_blame_hunk_index(repo, blame, 30, 41, 1, "2dc31040");
	check_blame_hunk_index(repo, blame, 31, 42, 1, "764df57e");
	check_blame_hunk_index(repo, blame, 32, 43, 1, "5280f4e6");
	check_blame_hunk_index(repo, blame, 33, 44, 1, "613d5eb9");
	check_blame_hunk_index(repo, blame, 34, 45, 1, "d12299fe");
	check_blame_hunk_index(repo, blame, 34, 46, 1, "111ee3fe");
	check_blame_hunk_index(repo, blame, 34, 47, 1, "f004c4a8");
	check_blame_hunk_index(repo, blame, 34, 48, 1, "111ee3fe");
	check_blame_hunk_index(repo, blame, 34, 49, 1, "9c82357b");
	check_blame_hunk_index(repo, blame, 34, 50, 1, "d6258deb");
	check_blame_hunk_index(repo, blame, 34, 51, 1, "b311e313");
	check_blame_hunk_index(repo, blame, 34, 52, 1, "3412391d");
	check_blame_hunk_index(repo, blame, 34, 53, 1, "bfc9ca59");
	check_blame_hunk_index(repo, blame, 34, 54, 1, "bf477ed4");
	check_blame_hunk_index(repo, blame, 34, 55, 1, "edebceff");
	check_blame_hunk_index(repo, blame, 34, 56, 1, "743a4b3b");
	check_blame_hunk_index(repo, blame, 34, 57, 1, "0a32dca5");
	check_blame_hunk_index(repo, blame, 34, 58, 1, "590fb68b");
	check_blame_hunk_index(repo, blame, 34, 59, 1, "bf477ed4");
	check_blame_hunk_index(repo, blame, 34, 60, 1, "d12299fe");

	git_blame_free(blame);
	git_repository_free(repo);
}

/* TODO: no newline at end of file? */
