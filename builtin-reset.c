/*
 * "git reset" builtin command
 *
 * Copyright (c) 2007 Carlos Rica
 *
 * Based on git-reset.sh, which is
 *
 * Copyright (c) 2005, 2006 Linus Torvalds and Junio C Hamano
 */
#include "cache.h"
#include "tag.h"
#include "object.h"
#include "commit.h"
#include "run-command.h"
#include "refs.h"
#include "diff.h"
#include "diffcore.h"
#include "tree.h"
#include "branch.h"
#include "parse-options.h"

static const char * const git_reset_usage[] = {
	"git reset [--mixed | --soft | --hard] [-q] [<commit>]",
	"git reset [--mixed] <commit> [--] <paths>...",
	NULL
};

static char *args_to_str(const char **argv)
{
	char *buf = NULL;
	unsigned long len, space = 0, nr = 0;

	for (; *argv; argv++) {
		len = strlen(*argv);
		ALLOC_GROW(buf, nr + 1 + len, space);
		if (nr)
			buf[nr++] = ' ';
		memcpy(buf + nr, *argv, len);
		nr += len;
	}
	ALLOC_GROW(buf, nr + 1, space);
	buf[nr] = '\0';

	return buf;
}

static inline int is_merge(void)
{
	return !access(git_path("MERGE_HEAD"), F_OK);
}

static int reset_index_file(const unsigned char *sha1, int is_hard_reset, int quiet)
{
	int i = 0;
	const char *args[6];

	args[i++] = "read-tree";
	if (!quiet)
		args[i++] = "-v";
	args[i++] = "--reset";
	if (is_hard_reset)
		args[i++] = "-u";
	args[i++] = sha1_to_hex(sha1);
	args[i] = NULL;

	return run_command_v_opt(args, RUN_GIT_CMD);
}

static void print_new_head_line(struct commit *commit)
{
	const char *hex, *body;

	hex = find_unique_abbrev(commit->object.sha1, DEFAULT_ABBREV);
	printf("HEAD is now at %s", hex);
	body = strstr(commit->buffer, "\n\n");
	if (body) {
		const char *eol;
		size_t len;
		body += 2;
		eol = strchr(body, '\n');
		len = eol ? eol - body : strlen(body);
		printf(" %.*s\n", (int) len, body);
	}
	else
		printf("\n");
}

static int update_index_refresh(int fd, struct lock_file *index_lock, int flags)
{
	int result;

	if (!index_lock) {
		index_lock = xcalloc(1, sizeof(struct lock_file));
		fd = hold_locked_index(index_lock, 1);
	}

	if (read_cache() < 0)
		return error("Could not read index");

	result = refresh_cache(flags) ? 1 : 0;
	if (write_cache(fd, active_cache, active_nr) ||
			commit_locked_index(index_lock))
		return error ("Could not refresh index");
	return result;
}

static void update_index_from_diff(struct diff_queue_struct *q,
		struct diff_options *opt, void *data)
{
	int i;
	int *discard_flag = data;

	/* do_diff_cache() mangled the index */
	discard_cache();
	*discard_flag = 1;
	read_cache();

	for (i = 0; i < q->nr; i++) {
		struct diff_filespec *one = q->queue[i]->one;
		if (one->mode) {
			struct cache_entry *ce;
			ce = make_cache_entry(one->mode, one->sha1, one->path,
				0, 0);
			if (!ce)
				die("make_cache_entry failed for path '%s'",
				    one->path);
			add_cache_entry(ce, ADD_CACHE_OK_TO_ADD |
				ADD_CACHE_OK_TO_REPLACE);
		} else
			remove_file_from_cache(one->path);
	}
}

static int read_from_tree(const char *prefix, const char **argv,
		unsigned char *tree_sha1, int refresh_flags)
{
	struct lock_file *lock = xcalloc(1, sizeof(struct lock_file));
	int index_fd, index_was_discarded = 0;
	struct diff_options opt;

	memset(&opt, 0, sizeof(opt));
	diff_tree_setup_paths(get_pathspec(prefix, (const char **)argv), &opt);
	opt.output_format = DIFF_FORMAT_CALLBACK;
	opt.format_callback = update_index_from_diff;
	opt.format_callback_data = &index_was_discarded;

	index_fd = hold_locked_index(lock, 1);
	index_was_discarded = 0;
	read_cache();
	if (do_diff_cache(tree_sha1, &opt))
		return 1;
	diffcore_std(&opt);
	diff_flush(&opt);
	diff_tree_release_paths(&opt);

	if (!index_was_discarded)
		/* The index is still clobbered from do_diff_cache() */
		discard_cache();
	return update_index_refresh(index_fd, lock, refresh_flags);
}

static void prepend_reflog_action(const char *action, char *buf, size_t size)
{
	const char *sep = ": ";
	const char *rla = getenv("GIT_REFLOG_ACTION");
	if (!rla)
		rla = sep = "";
	if (snprintf(buf, size, "%s%s%s", rla, sep, action) >= size)
		warning("Reflog action message too long: %.*s...", 50, buf);
}

enum reset_type { MIXED, SOFT, HARD, NONE };
static const char *reset_type_names[] = { "mixed", "soft", "hard", NULL };

int cmd_reset(int argc, const char **argv, const char *prefix)
{
	int i = 0, reset_type = NONE, update_ref_status = 0, quiet = 0;
	const char *rev = "HEAD";
	unsigned char sha1[20], *orig = NULL, sha1_orig[20],
				*old_orig = NULL, sha1_old_orig[20];
	struct commit *commit;
	char *reflog_action, msg[1024];
	const struct option options[] = {
		OPT_SET_INT(0, "mixed", &reset_type,
						"reset HEAD and index", MIXED),
		OPT_SET_INT(0, "soft", &reset_type, "reset only HEAD", SOFT),
		OPT_SET_INT(0, "hard", &reset_type,
				"reset HEAD, index and working tree", HARD),
		OPT_BOOLEAN('q', NULL, &quiet,
				"disable showing new HEAD in hard reset and progress message"),
		OPT_END()
	};

	git_config(git_default_config, NULL);

	argc = parse_options(argc, argv, options, git_reset_usage,
						PARSE_OPT_KEEP_DASHDASH);
	reflog_action = args_to_str(argv);
	setenv("GIT_REFLOG_ACTION", reflog_action, 0);

	/*
	 * Possible arguments are:
	 *
	 * git reset [-opts] <rev> <paths>...
	 * git reset [-opts] <rev> -- <paths>...
	 * git reset [-opts] -- <paths>...
	 * git reset [-opts] <paths>...
	 *
	 * At this point, argv[i] points immediately after [-opts].
	 */

	if (i < argc) {
		if (!strcmp(argv[i], "--")) {
			i++; /* reset to HEAD, possibly with paths */
		} else if (i + 1 < argc && !strcmp(argv[i+1], "--")) {
			rev = argv[i];
			i += 2;
		}
		/*
		 * Otherwise, argv[i] could be either <rev> or <paths> and
		 * has to be unambigous.
		 */
		else if (!get_sha1(argv[i], sha1)) {
			/*
			 * Ok, argv[i] looks like a rev; it should not
			 * be a filename.
			 */
			verify_non_filename(prefix, argv[i]);
			rev = argv[i++];
		} else {
			/* Otherwise we treat this as a filename */
			verify_filename(prefix, argv[i]);
		}
	}

	if (get_sha1(rev, sha1))
		die("Failed to resolve '%s' as a valid ref.", rev);

	commit = lookup_commit_reference(sha1);
	if (!commit)
		die("Could not parse object '%s'.", rev);
	hashcpy(sha1, commit->object.sha1);

	/* git reset tree [--] paths... can be used to
	 * load chosen paths from the tree into the index without
	 * affecting the working tree nor HEAD. */
	if (i < argc) {
		if (reset_type == MIXED)
			warning("--mixed option is deprecated with paths.");
		else if (reset_type != NONE)
			die("Cannot do %s reset with paths.",
					reset_type_names[reset_type]);
		return read_from_tree(prefix, argv + i, sha1,
				quiet ? REFRESH_QUIET : REFRESH_SAY_CHANGED);
	}
	if (reset_type == NONE)
		reset_type = MIXED; /* by default */

	if (reset_type == HARD && is_bare_repository())
		die("hard reset makes no sense in a bare repository");

	/* Soft reset does not touch the index file nor the working tree
	 * at all, but requires them in a good order.  Other resets reset
	 * the index file to the tree object we are switching to. */
	if (reset_type == SOFT) {
		if (is_merge() || read_cache() < 0 || unmerged_cache())
			die("Cannot do a soft reset in the middle of a merge.");
	}
	else if (reset_index_file(sha1, (reset_type == HARD), quiet))
		die("Could not reset index file to revision '%s'.", rev);

	/* Any resets update HEAD to the head being switched to,
	 * saving the previous head in ORIG_HEAD before. */
	if (!get_sha1("ORIG_HEAD", sha1_old_orig))
		old_orig = sha1_old_orig;
	if (!get_sha1("HEAD", sha1_orig)) {
		orig = sha1_orig;
		prepend_reflog_action("updating ORIG_HEAD", msg, sizeof(msg));
		update_ref(msg, "ORIG_HEAD", orig, old_orig, 0, MSG_ON_ERR);
	}
	else if (old_orig)
		delete_ref("ORIG_HEAD", old_orig);
	prepend_reflog_action("updating HEAD", msg, sizeof(msg));
	update_ref_status = update_ref(msg, "HEAD", sha1, orig, 0, MSG_ON_ERR);

	switch (reset_type) {
	case HARD:
		if (!update_ref_status && !quiet)
			print_new_head_line(commit);
		break;
	case SOFT: /* Nothing else to do. */
		break;
	case MIXED: /* Report what has not been updated. */
		update_index_refresh(0, NULL,
				quiet ? REFRESH_QUIET : REFRESH_SAY_CHANGED);
		break;
	}

	remove_branch_state();

	free(reflog_action);

	return update_ref_status;
}
