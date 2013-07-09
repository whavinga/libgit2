#include "clar_libgit2.h"

#include "repository.h"

static git_repository *g_repo;
static git_tree *g_root_tree;
static git_object *g_expectedobject,
						*g_actualobject;

void test_object_lookupbypath__initialize(void)
{
	git_reference *head;
	git_commit *commit;
	git_tree_entry *tree_entry;

	cl_git_pass(git_repository_open(&g_repo, cl_fixture("attr/.gitted")));

	cl_git_pass(git_repository_head(&head, g_repo));
	cl_git_pass(git_reference_peel((git_object**)&commit, head, GIT_OBJ_COMMIT));
	cl_git_pass(git_commit_tree(&g_root_tree, commit));
	cl_git_pass(git_tree_entry_bypath(&tree_entry, g_root_tree, "subdir/subdir_test2.txt"));
	cl_git_pass(git_object_lookup(&g_expectedobject, g_repo, git_tree_entry_id(tree_entry),
				GIT_OBJ_ANY));

	git_tree_entry_free(tree_entry);
	git_commit_free(commit);
	git_reference_free(head);

	g_actualobject = NULL;
}
void test_object_lookupbypath__cleanup(void)
{
	git_object_free(g_expectedobject);
	g_expectedobject = NULL;
	git_repository_free(g_repo);
	g_repo = NULL;
	git_object_free(g_actualobject);
}

void test_object_lookupbypath__gets_proper_object(void)
{
	cl_git_pass(git_object_lookup_bypath(&g_actualobject, (git_object*)g_root_tree,
				"subdir/subdir_test2.txt", GIT_OBJ_BLOB));
	cl_assert_equal_i(0, git_oid_cmp(git_object_id(g_expectedobject),
				git_object_id(g_actualobject)));
}

void test_object_lookupbypath__checks_parameters(void)
{
	cl_git_fail(git_object_lookup_bypath(NULL, NULL, NULL, -1));
	cl_git_fail(git_object_lookup_bypath(&g_actualobject, NULL, NULL, -1));
	cl_git_fail(git_object_lookup_bypath(NULL, (git_object*)g_root_tree, NULL, -1));
	cl_git_fail(git_object_lookup_bypath(NULL, NULL, "subdir/subdir_test2.txt", -1));
	cl_git_fail(git_object_lookup_bypath(NULL, NULL, NULL, GIT_OBJ_BLOB));
	cl_git_fail(git_object_lookup_bypath(&g_actualobject, (git_object*)g_root_tree, NULL, -1));
}

void test_object_lookupbypath__errors(void)
{
	cl_assert_equal_i(GIT_EINVALIDSPEC,
			git_object_lookup_bypath(&g_actualobject, (git_object*)g_root_tree,
				"subdir/subdir_test2.txt", GIT_OBJ_TREE)); // It's not a tree
	cl_assert_equal_i(GIT_ENOTFOUND,
			git_object_lookup_bypath(&g_actualobject, (git_object*)g_root_tree,
				"file/doesnt/exist", GIT_OBJ_ANY));
}

