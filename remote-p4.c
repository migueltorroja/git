#include "git-compat-util.h"
#include "cache.h"
#include "strbuf.h"
#include "remote.h"
#include "vcs-p4/git-p4-lib.h"


#define DEBUG_VERBOSE
#ifdef DEBUG_VERBOSE
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOG(...)
#endif

static const char *url_in;
static const char *remote_ref = "refs/heads/master";
static const char *private_ref;

static int cmd_capabilities(const char *line);
static int cmd_import(const char *line);
static int cmd_list(const char *line);

typedef int (*input_command_handler)(const char *);


struct input_command_entry {
	const char *name;
	input_command_handler fn;
};

static const struct input_command_entry input_command_list[] = {
	{ "capabilities", cmd_capabilities},
	{ "import", cmd_import},
	{ "list", cmd_list},
};

static int cmd_capabilities(const char *line)
{
	printf("import\n");
	printf("bidi-import\n");
	printf("refspec %s:%s\n\n", remote_ref, private_ref);
	fflush(stdout);
	return 0;
}

static const char *get_master_depot_path(const char *url)
{
	const char *p4_tag = strstr(url, "p4:");
	return p4_tag + 3;
}

static int cmd_import(const char *line)
{
	const char *ref_name = line;
	fprintf(stderr, "%s\n", line);
	ref_name = strchr(ref_name, ' ');
	int res = 0;
	if (!ref_name) {
		return 1;
	}
	ref_name++;
	if (strcmp(ref_name, remote_ref) != 0)
		return 1;
	res = p4_fetch_update_ref(STDOUT_FILENO, private_ref, NULL, get_master_depot_path(url_in), 1);
	if (res != 0)
		write_str_in_full(STDOUT_FILENO, "done\n");
	return res;
}

static int cmd_list(const char *line)
{
	printf("? %s\n\n", remote_ref);
	fflush(stdout);
	return 0;
}

static int do_command(struct strbuf *line)
{
	const struct input_command_entry *p;
	const struct input_command_entry * const p_end = input_command_list + ARRAY_SIZE(input_command_list);
	for (p = input_command_list; p != p_end; p++) {
		if (starts_with(line->buf, p->name) && (strlen(p->name) == line->len ||
				line->buf[strlen(p->name)] == ' ')) {
			return p->fn(line->buf);
		}
	}
	die("Unknown command '%s'\n", line->buf);
}

int cmd_main(int argc, const char **argv)
{
	struct strbuf private_ref_sb = STRBUF_INIT;
	struct strbuf buf = STRBUF_INIT;
	struct remote *remote;
	const char **arg_indx = NULL;
	for (arg_indx = argv; arg_indx != argv + argc; arg_indx ++) {
		LOG(" %s", *arg_indx);
	}
	LOG("\n");
	setup_git_directory();
	if (argc < 2 || argc > 3) {
		usage("git-remote-p4 <remote-name> [user@perforce-server:perforce-port/master_branch]");
		return 1;
	}

	remote = remote_get(argv[1]);
	url_in = (argc == 3) ? argv[2] : remote->url[0];

	strbuf_addf(&private_ref_sb, "refs/p4/%s/master", remote->name);
	private_ref = private_ref_sb.buf;

	while (1) {
		if (strbuf_getline_lf(&buf, stdin) == EOF) {
			if (ferror(stdin))
				die("Error reading command stream");
			else
				die("Unexpected end of command stream");
		}
		if (buf.len == 0)
			break;
		if (do_command(&buf))
			break;
		strbuf_reset(&buf);
	}

	strbuf_release(&buf);
	strbuf_release(&private_ref_sb);

	return 0;
}
