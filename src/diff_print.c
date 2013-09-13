#include "buffer.h"
	else if (mode & 0100) /* -V536 */
		git_buf_printf(out, "%c\t%s%c -> %s%c\n", code,
		git_buf_printf(out, "%c\t%s%c (%o -> %o)\n", code,
			delta->old_file.path, new_suffix, delta->old_file.mode, delta->new_file.mode);
static int diff_print_oid_range(diff_print_info *pi, const git_diff_delta *delta)
	git_buf *out = pi->buf;
	git_oid_tostr(start_oid, pi->oid_strlen, &delta->old_file.oid);
	git_oid_tostr(end_oid, pi->oid_strlen, &delta->new_file.oid);
static int diff_print_patch_file(
	const git_diff_delta *delta, float progress, void *data)
	diff_print_info *pi = data;
	const char *oldpfx = pi->diff ? pi->diff->opts.old_prefix : NULL;
	const char *newpfx = pi->diff ? pi->diff->opts.new_prefix : NULL;
	uint32_t opts_flags = pi->diff ? pi->diff->opts.flags : GIT_DIFF_NORMAL;
	GIT_UNUSED(progress);
	if (S_ISDIR(delta->new_file.mode) ||
		delta->status == GIT_DELTA_UNMODIFIED ||
		delta->status == GIT_DELTA_IGNORED ||
		(delta->status == GIT_DELTA_UNTRACKED &&
		 (opts_flags & GIT_DIFF_INCLUDE_UNTRACKED_CONTENT) == 0))
		return 0;
	git_buf_clear(pi->buf);
	git_buf_printf(pi->buf, "diff --git %s%s %s%s\n",
	if (diff_print_oid_range(pi, delta) < 0)
	if (git_oid_iszero(&delta->old_file.oid)) {
		oldpfx = "";
		oldpath = "/dev/null";
	}
	if (git_oid_iszero(&delta->new_file.oid)) {
		newpfx = "";
		newpath = "/dev/null";
	}
	if ((delta->flags & GIT_DIFF_FLAG_BINARY) == 0) {
		git_buf_printf(pi->buf, "--- %s%s\n", oldpfx, oldpath);
		git_buf_printf(pi->buf, "+++ %s%s\n", newpfx, newpath);
	}
	if (git_buf_oom(pi->buf))
	git_buf_printf(
		pi->buf, "Binary files %s%s and %s%s differ\n",
		oldpfx, oldpath, newpfx, newpath);
	if (git_buf_oom(pi->buf))