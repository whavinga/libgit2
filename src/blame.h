#include "git2/blame.h"
#include "common.h"
#include "vector.h"
#include "diff.h"
#include "array.h"
#include "git2/oid.h"

/* Experimental line-map */
#define kmalloc git__malloc
#define kcalloc git__calloc
#define krealloc git__realloc
#define kfree git__free
#include "khash.h"
__KHASH_TYPE(line, const git_oid*, uint16_t);
typedef khash_t(line) blame_linemap;
GIT_INLINE(khint_t) linemap_hash(const git_oid *oid)
{
	khint_t h;
	memcpy(&h, oid, sizeof(khint_t));
	return h;
}
#define GIT__USE_LINEMAP \
	__KHASH_IMPL(line, static kh_inline, const git_oid *, uint16_t, 1, linemap_hash, git_oid_equal)
#define blame_linemap_alloc() kh_init(line)
#define blame_linemap_free(h) kh_destroy(line,h), h = NULL

typedef struct blame_hunk {
	/* git_blame_hunk fields */
	uint16_t lines_in_hunk;
	git_oid final_commit_id;
	uint16_t final_start_line_number;
	git_oid orig_commit_id;
	const char *orig_path;
	uint16_t orig_start_line_number;

	/* Internal fields */
	blame_linemap *linemap;
	size_t current_score;
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
	git_oid parent_commit;
	size_t current_diff_line;
	size_t current_blame_line;
	blame_hunk *current_hunk;
};

git_blame *git_blame__alloc(
	git_repository *repo,
	git_blame_options opts,
	const char *path);

blame_hunk *git_blame__alloc_hunk();

