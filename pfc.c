/*
 *  git-p4 (C porting)
 *
 *  Copyright (C) 2017 Miguel Torroja <miguel.torroja@gmail.com>
 */
#include "git-compat-util.h"
#include "builtin.h"
#include "strbuf.h"
#include "hashmap.h"
#include "list.h"
#include "parse-options.h"
#include "gettext.h"
#include "config.h"
#include "run-command.h"
#include "utf8.h"
#include "tempfile.h"
#include "vcs-p4/py-marshal.h"
#include "vcs-p4/strbuf-dict.h"
#include "vcs-p4/md5.h"


struct md5_id
{
	uint8_t md5[16];
};

const struct md5_id null_md5 = {
	{0}
};

int md5cmp(struct md5_id *a, struct md5_id *b)
{
	return memcmp(&a->md5, &b->md5, sizeof(a->md5));
}

typedef enum p4_format_types {
	P4_FORMAT_UNKNOWN_TYPE = 0,
	P4_FORMAT_TEXT_TYPE,
	P4_FORMAT_BIN_TYPE,
	P4_FORMAT_UTF8_TYPE,
	P4_FORMAT_UTF16_TYPE,
	P4_FORMAT_LINK_TYPE,
	P4_FORMAT_MAX_TYPE,
} p4_format_types_t;

struct depot_file_t
{
	struct strbuf depot_path_file;
	unsigned int chg_rev;
	int is_revision;
	unsigned mode;
	struct md5_id hash;
	p4_format_types_t bin_type;
	struct list_head lhead;
};

#define DEPOT_FILE_INIT {STRBUF_INIT, 0, 0}

struct depot_file_pair_t
{
	struct depot_file_t a;
	struct depot_file_t b;
	struct list_head lhead;
};


#define CHANGE_SRC_P4 (1)
#define CHANGE_SRC_GIT (2)

struct depot_changelist_desc_t
{
	int change_source;
	struct strbuf changelist_or_commit;
	struct strbuf desc;
	struct strbuf time;
	struct strbuf committer;
	struct strbuf depot_base;
	struct list_head list_of_deleted_files;
	struct list_head list_of_modified_files;
	struct list_head list;
};

#define INIT_DEPOT_CHANGELIST_DESC(ptr) do {\
	(ptr)->change_source = CHANGE_SRC_P4; \
	strbuf_init(&(ptr)->changelist_or_commit, 0); \
	strbuf_init(&(ptr)->desc, 0); \
	strbuf_init(&(ptr)->time, 0); \
	strbuf_init(&(ptr)->committer, 0); \
	strbuf_init(&(ptr)->depot_base, 0); \
	INIT_LIST_HEAD(&(ptr)->list_of_deleted_files); \
	INIT_LIST_HEAD(&(ptr)->list_of_modified_files); \
	INIT_LIST_HEAD(&(ptr)->list); \
}while(0)


#define GITP4_CHANGE_STAMP_NONE (0)
#define GITP4_CHANGE_STAMP_REF (1)
#define GITP4_CHANGE_STAMP_CHERRY_PICK (2)

static void wildcard_encode(struct strbuf *sb);

struct command_t;

typedef int (*run_type) (struct command_t *pcmd,int gargc, const char **gargv);
typedef void (*deinit_type) (struct command_t *pcmd);

typedef struct command_t
{
	struct strbuf   strb_usage;
	uint8_t         needs_git;
	uint8_t         verbose; 
	run_type        run_fn;
	deinit_type     deinit_fn;
	void *          data;
} command_t;

typedef struct p4_user_map_t {
	struct strbuf my_p4_user_id;
	struct hashmap users;
	struct hashmap emails;
	int user_map_from_perfroce_server;
} p4_user_map_t;

struct p4_verbose_debug_t {
	unsigned int verbose_level;
	FILE *fp;
};

#define P4_VERBOSE_CRITICAL_LEVEL	(0)
#define P4_VERBOSE_INFO_LEVEL		(1)
#define P4_VERBOSE_DEBUG_LEVEL		(2)

#define P4_VERBOSE_INIT {0,NULL}

static char *md5_to_hex(struct md5_id *pmd5)
{
	char *md5_hex = NULL;
	struct object_id extended_oid = null_oid;
	memcpy(&extended_oid.hash, &pmd5->md5, sizeof(pmd5->md5));
	md5_hex = oid_to_hex(&extended_oid);
	if (md5_hex)
		md5_hex[ARRAY_SIZE(pmd5->md5)*2] = '\0';
	return md5_hex;
}

static int get_md5_hex(const char *str, struct md5_id *pmd5)
{
	int i;
	uint8_t *pindx = &pmd5->md5[0];
	if (!str)
		return -1;
	for (i = 0; i < ARRAY_SIZE(pmd5->md5); i++) {
		if (!*str || !*(str + 1))
			return -1;
		int val = hex2chr(str);
		if (val < 0)
			return -1;
		*pindx++ = val;
		str += 2;
	}
	return 0;
}

struct p4_verbose_debug_t p4_verbose_debug = P4_VERBOSE_INIT;
struct p4_user_map_t *p4usermap_cached;

void p4_verbose_init(struct p4_verbose_debug_t *p4verbose, int level, FILE *fp)
{
	p4verbose->verbose_level = level;
	if (fp)
		p4verbose->fp = fp;
	else
		p4verbose->fp = stderr;
}



static void p4_verbose_vprintf(int loglevel, const char *fmt, va_list ap)
{
	if (loglevel > p4_verbose_debug.verbose_level)
		return;
	vfprintf(p4_verbose_debug.fp, fmt, ap);
}

__attribute__((format (printf,2,3)))
static void p4_verbose_printf(int loglevel, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	p4_verbose_vprintf(loglevel, fmt, ap);
	va_end(ap);
}

#ifdef HAVE_VARIADIC_MACROS

#define LOG_GITP4_CRITICAL(...) p4_verbose_printf(P4_VERBOSE_CRITICAL_LEVEL, __VA_ARGS__)
#define LOG_GITP4_INFO(...) p4_verbose_printf(P4_VERBOSE_INFO_LEVEL, __VA_ARGS__)
#define LOG_GITP4_DEBUG(...) p4_verbose_printf(P4_VERBOSE_DEBUG_LEVEL, __VA_ARGS__)

#else

__attribute__((format (printf,1,2)))
void LOG_GITP4_CRITICAL(fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	p4_verbose_vprintf(P4_VERBOSE_CRITICAL_LEVEL, fmt, ap);
	va_end(ap);
}

__attribute__((format (printf,1,2)))
void LOG_GITP4_INFO(fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	p4_verbose_vprintf(P4_VERBOSE_INFO_LEVEL, fmt, ap);
	va_end(ap);
}

__attribute__((format (printf,1,2)))
void LOG_GITP4_DEBUG(fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	p4_verbose_vprintf(P4_VERBOSE_DEBUG_LEVEL, fmt, ap);
	va_end(ap);
}
#endif

#define IS_LOG_CRITITAL_ALLOWED (1)
#define IS_LOG_INFO_ALLOWED (P4_VERBOSE_INFO_LEVEL <= p4_verbose_debug.verbose_level)
#define IS_LOG_DEBUG_ALLOWED (P4_VERBOSE_DEBUG_LEVEL <= p4_verbose_debug.verbose_level)

void cmd_print_usage(FILE *fp, command_t *pcmd)
{
	fprintf(fp,"%s\n",pcmd->strb_usage.buf);
}

static void _strbuf_ch_translate(struct strbuf *dst, struct strbuf *src, char inc, char outc)
{
	void *s_p = src->buf;
	void *s_end = src->buf + src->len;
	strbuf_release(dst);
	while (s_p && (s_p < s_end)) {
		void *s_find = memchr(s_p, inc, s_end - s_p);
		if (s_find) {
			strbuf_add(dst, s_p, s_find - s_p);
			strbuf_addch(dst, outc);
			s_p = s_find + 1;
		}
		else {
			strbuf_add(dst, s_p, s_end - s_p);
			s_p = s_end;
		}
	}
}

static void _strbuf_insert_str(struct strbuf *dst, const char *str)
{
	strbuf_insert(dst, 0, str, strlen(str));
}


static void p4_start_command(struct child_process *cmd)
{
	const char **argv = cmd->argv?cmd->argv:cmd->args.argv;
	const char * const*env = cmd->env?cmd->env:cmd->env_array.argv;
	struct argv_array nargs = ARGV_ARRAY_INIT;
	struct argv_array nenv = ARGV_ARRAY_INIT;
	argv_array_push(&nargs, "p4");
	argv_array_push(&nargs, "-G");
	argv_array_pushv(&nargs, argv);
	cmd->argv = nargs.argv;
	SWAP(cmd->args, nargs);
	SWAP(cmd->env_array, nenv);
	if (cmd->dir) {
		argv_array_pushv(&nenv, (const char **)env);
		argv_array_pushf(&nenv, "PWD=%s",cmd->dir);
		cmd->env = nenv.argv;
	}
	if (IS_LOG_DEBUG_ALLOWED) {
	 const char **argv = cmd->args.argv;
	 LOG_GITP4_DEBUG("command:");
	 for (; *argv; argv++)
		LOG_GITP4_DEBUG(" %s", *argv);
	 LOG_GITP4_DEBUG("\n");
	}
	if (start_command(cmd))
		die("cannot start p4 process");
	argv_array_clear(&nargs);
	argv_array_clear(&nenv);
}

/* function that runs the p4 with the python marshal output format
 * Each new py dict created is passed to a callback
 * it returns the exit status of the p4 command process 
 */

static int p4_cmd_run(const char **argv, const char *dir, void (*cb) ( struct hashmap *map, void *datain), void *data)
{
	struct child_process child_p4 = CHILD_PROCESS_INIT;
	struct hashmap map;
	str_dict_init(&map);
	child_p4.argv = argv;
	child_p4.out = -1;
	child_p4.dir = dir;
	p4_start_command(&child_p4);
	while (py_marshal_parse(&map, child_p4.out))
	{
		if (cb)
			cb(&map,data);
	}
	close(child_p4.out);
	str_dict_destroy(&map);
	return finish_command(&child_p4);
}

static void p4_count_objects_cb(struct hashmap *map, void *argout)
{
	int *count = (int *)argout;
	*count = *count + 1;
}


static int p4_nfiles_opened(const char *p4_path)
{
	struct argv_array p4args = ARGV_ARRAY_INIT;
	struct strbuf sb_path = STRBUF_INIT;
	int count=0;
	argv_array_push(&p4args, "opened");
	strbuf_addstr(&sb_path, p4_path);
	strbuf_strip_suffix(&sb_path, "...");
	strbuf_strip_suffix(&sb_path, "/");
	argv_array_pushf(&p4args, "%s/...", sb_path.buf);
	p4_cmd_run(p4args.argv, NULL, p4_count_objects_cb, &count); 
	strbuf_release(&sb_path);
	argv_array_clear(&p4args);
	return count;
}

static void p4_normalize_type(const char *legacy_type, struct strbuf *base_type, struct strbuf *mods)
{
	strbuf_reset(base_type);
	strbuf_reset(mods);
	const char *plus_ptr = strchr(legacy_type,'+');
	if (plus_ptr) {
		strbuf_addf(base_type,"%.*s",(int)(plus_ptr-legacy_type),legacy_type);
		strbuf_addf(mods, "+%s", plus_ptr+1);
		return;
	}
	if (!strcmp(legacy_type,"text") ||
			!strcmp(legacy_type,"binary") ||
			!strcmp(legacy_type,"symlink") ||
			!strcmp(legacy_type,"apple") ||
			!strcmp(legacy_type,"resource") ||
			!strcmp(legacy_type,"unicode") ||
			!strcmp(legacy_type,"utf8") ||
			!strcmp(legacy_type,"utf16")) {
		strbuf_addstr(base_type, legacy_type);
	}
	else if (!strcmp(legacy_type,"ctempobj")) {
		strbuf_addf(base_type,"binary");
		strbuf_addf(mods,"+Sw");
	}
	else if (!strcmp(legacy_type,"ctext")) {
		strbuf_addf(base_type, "text");
		strbuf_addf(mods,"+C");
	}
	else if (!strcmp(legacy_type,"cxtext")) {
		strbuf_addf(base_type, "text");
		strbuf_addf(mods,"+Cx");
	}
	else if (!strcmp(legacy_type,"ktext")) {
		strbuf_addf(base_type, "text");
		strbuf_addf(mods,"+k");
	}
	else if (!strcmp(legacy_type,"kxtext")) {
		strbuf_addf(base_type, "text");
		strbuf_addf(mods,"+kx");
	}
	else if (!strcmp(legacy_type,"ltext")) {
		strbuf_addf(base_type, "text");
		strbuf_addf(mods,"+F");
	}
	else if (!strcmp(legacy_type,"tempobj")) {
		strbuf_addf(base_type, "binary");
		strbuf_addf(mods,"+FSw");
	}
	else if (!strcmp(legacy_type,"ubinary")) {
		strbuf_addf(base_type, "binary");
		strbuf_addf(mods,"+F");
	}
	else if (!strcmp(legacy_type,"uresource")) {
		strbuf_addf(base_type, "resource");
		strbuf_addf(mods,"+F");
	}
	else if (!strcmp(legacy_type,"uxbinary")) {
		strbuf_addf(base_type, "binary");
		strbuf_addf(mods,"+Fx");
	}
	else if (!strcmp(legacy_type,"xbinary")) {
		strbuf_addf(base_type, "binary");
		strbuf_addf(mods,"+x");
	}
	else if (!strcmp(legacy_type,"xltext")) {
		strbuf_addf(base_type, "text");
		strbuf_addf(mods,"+Fx");
	}
	else if (!strcmp(legacy_type,"xtempobj")) {
		strbuf_addf(base_type, "binary");
		strbuf_addf(mods,"+Swx");
	}
	else if (!strcmp(legacy_type,"xtext")) {
		strbuf_addf(base_type, "text");
		strbuf_addf(mods,"+x");
	}
	else if (!strcmp(legacy_type,"xunicode")) {
		strbuf_addf(base_type, "unicode");
		strbuf_addf(mods,"+x");
	}
	else if (!strcmp(legacy_type,"xutf8")) {
		strbuf_addf(base_type, "utf8");
		strbuf_addf(mods,"+x");
	}
	else if (!strcmp(legacy_type,"xutf16")) {
		strbuf_addf(base_type, "utf16");
		strbuf_addf(mods,"+x");
	}
	else {
		die("p4 type not recognized: %s", legacy_type);
	}
}

static unsigned p4type2mode(const char *type)
{
	struct strbuf base = STRBUF_INIT;
	struct strbuf mods = STRBUF_INIT;
	unsigned mode = 0;
	p4_normalize_type(type, &base, &mods);
	if (0 == strcmp(base.buf, "symlink"))
		mode = 0120000;
	else if (NULL != strchr(mods.buf, 'x'))
		mode = 0100755;
	else
		mode = 0100644;
	strbuf_release(&mods);
	strbuf_release(&base);
	return mode;
}

static p4_format_types_t p4type2bintype(const char *type)
{
	struct strbuf base = STRBUF_INIT;
	struct strbuf mods = STRBUF_INIT;
	p4_format_types_t bin_type = P4_FORMAT_UNKNOWN_TYPE;
	p4_normalize_type(type, &base, &mods);
	if (0 == strcmp(base.buf, "text"))
		bin_type = P4_FORMAT_TEXT_TYPE;
	else if (0 == strcmp(base.buf, "utf8"))
		bin_type = P4_FORMAT_UTF8_TYPE;
	else if (0 == strcmp(base.buf, "utf16"))
		bin_type = P4_FORMAT_UTF16_TYPE;
	else if (0 == strcmp(base.buf, "binary"))
		bin_type = P4_FORMAT_BIN_TYPE;
	else if (0 == strcmp(base.buf, "symlink"))
		bin_type = P4_FORMAT_LINK_TYPE;
	strbuf_release(&mods);
	strbuf_release(&base);
	return bin_type;
}

static void add_p4_modes(struct strbuf *p4mod, const char *addmods)
{
	const char *pchr;
	if (p4mod->len && ((p4mod->buf[0] != '+') || (p4mod->len == 1)))
		die("Malformed p4 mods %s", p4mod->buf);
	for (pchr = addmods;*pchr;pchr++) {
		if (strchr(p4mod->buf,*pchr))
			continue;
		if (!p4mod->len)
			strbuf_addch(p4mod, '+');
		strbuf_addch(p4mod, *pchr);
	}
}

static void remove_p4_modes(struct strbuf *p4mod, const char *rmmods)
{
	const char *pchr;
	if (p4mod->len && ((p4mod->buf[0] != '+') || (p4mod->len == 1)))
		die("Malformed p4 mods %s", p4mod->buf);
	for (pchr = rmmods;*pchr;pchr++) {
		const char *pfound;
		pfound=strchr(p4mod->buf,*pchr);
		if (!pfound)
			continue;
		strbuf_remove(p4mod,pfound-p4mod->buf,1);
	}
	if (p4mod->len == 1)
		strbuf_reset(p4mod);
}

static void p4_opened_type_cb(struct hashmap *map, void *arg)
{
	struct strbuf *p4type_buf = (struct strbuf *) arg;
	const char *p4type_str = str_dict_get_value(map, "type");
	if (p4type_str) {
		strbuf_reset(p4type_buf);
		strbuf_addstr(p4type_buf, p4type_str);
	}
}

static int p4_opened_type(const char *client_dir, const char *p4_path, struct strbuf *base_type, struct strbuf *mods)
{
	struct argv_array p4args = ARGV_ARRAY_INIT;
	struct strbuf q_path = STRBUF_INIT;
	struct strbuf p4_type = STRBUF_INIT;
	int rout;
	argv_array_push(&p4args, "opened");
	strbuf_addstr(&q_path, p4_path);
	wildcard_encode(&q_path);
	argv_array_push(&p4args, q_path.buf);
	rout = p4_cmd_run(p4args.argv, client_dir,  p4_opened_type_cb, &p4_type);
	p4_normalize_type(p4_type.buf, base_type, mods);
	argv_array_clear(&p4args);
	strbuf_release(&q_path);
	strbuf_release(&p4_type);
	return rout;
}

static int p4_set_exec_git(const char *client_dir, const char *p4_path, const char *git_mod)
{
	struct strbuf base_type = STRBUF_INIT;
	struct strbuf mode_type = STRBUF_INIT;
	struct strbuf q_path = STRBUF_INIT;
	struct argv_array p4args = ARGV_ARRAY_INIT;
	int res;
	p4_opened_type(client_dir, p4_path, &base_type, &mode_type);
	if (ends_with(git_mod, "755"))
		add_p4_modes(&mode_type,"x");
	else
		remove_p4_modes(&mode_type,"x");
	argv_array_push(&p4args, "reopen");
	argv_array_push(&p4args, "-t");
	argv_array_pushf(&p4args, "%s%s",base_type.buf,mode_type.buf);
	strbuf_addstr(&q_path, p4_path);
	wildcard_encode(&q_path);
	argv_array_push(&p4args, q_path.buf);
	res = p4_cmd_run(p4args.argv, client_dir, NULL, NULL);
	strbuf_release(&base_type);
	strbuf_release(&mode_type);
	strbuf_release(&q_path);
	argv_array_clear(&p4args);
	return res;
}

static void p4_has_admin_permissions_cb(struct hashmap *map, void *arg)
{
	int *admin_perm = (int *)arg;
	const char *perm_val = NULL;
	perm_val = str_dict_get_value(map, "perm");
	if (!perm_val)
		return;
	if (strcmp(perm_val, "admin") == 0)
		*admin_perm = 1;
	else if (strcmp(perm_val, "super") == 0)
		*admin_perm = 1;
}

static int p4_has_admin_permissions(const char *depot_path)
{
	const char *p4args[] = { "protects", depot_path, NULL};
	int has_admin = 0;
	p4_cmd_run(p4args, NULL, p4_has_admin_permissions_cb, &has_admin); 
	return has_admin;
}

static int p4debug_cmd_run(command_t *pcmd, int gargc, const char **gargv)
{
	struct child_process child_p4 = CHILD_PROCESS_INIT;
	struct hashmap map;
	const char **args = gargv+1;
	child_p4.out = -1;
	str_dict_init(&map);

	gargc --;
	for (;gargc; gargc--,args++)
		argv_array_push(&child_p4.args, *args);

	p4_start_command(&child_p4);

	while (py_marshal_parse(&map, child_p4.out))
			str_dict_print(stdout, &map);

	close(child_p4.out);
	str_dict_destroy(&map);
	return finish_command(&child_p4);
}

void p4_cmd_default_deinit(struct command_t *pcmd)
{
	strbuf_release(&pcmd->strb_usage);
	if (pcmd->data)
		free(pcmd->data);
}

void p4_cmd_destroy(struct command_t *pcmd)
{
	pcmd->deinit_fn(pcmd);
}

int git_cmd_read_pipe_line(const char **args,
		void (*cb)(struct strbuf *sb, void *arg),
		void *arg)
{
	struct child_process child_git = CHILD_PROCESS_INIT;
	struct strbuf sbline = STRBUF_INIT;
	FILE *fp;
	child_git.git_cmd = 1;
	child_git.out = -1;
	child_git.argv = args;
	if (start_command(&child_git))
		die("cannot start git process");
	fp = fdopen(child_git.out,"r");
	while (EOF != strbuf_getwholeline(&sbline, fp, '\n')) {
		if (cb)
			cb(&sbline, arg);
	}
	fclose(fp);
	strbuf_release(&sbline);
	return finish_command(&child_git);
}

static void git_cmd_read_pipe_full_cb(struct strbuf *sb_line, void *arg)
{
	struct strbuf *sb_out = (struct strbuf *) arg;
	strbuf_addbuf(sb_out, sb_line);
}

int git_cmd_read_pipe_full(const char **args, struct strbuf *strb)
{
	return git_cmd_read_pipe_line(args, git_cmd_read_pipe_full_cb, strb);
}

int parse_revision(struct strbuf *strout, const char *ref)
{
	const char *list_of_cmds[] = {"rev-parse", ref, NULL};
	int rout;
	strbuf_reset(strout);
	rout = git_cmd_read_pipe_full(list_of_cmds, strout);
	strbuf_trim(strout);
	return rout;
}

int current_git_branch(struct strbuf *branch_name)
{
	const char *list_of_cmds[] = {"symbolic-ref", "--short", "-q", "HEAD", NULL};
	int rout;
	strbuf_reset(branch_name);
	rout = git_cmd_read_pipe_full(list_of_cmds, branch_name);
	strbuf_trim(branch_name);
	return rout;
}

int branch_exists(const char *ref_name)
{
	const char *list_of_cmds[] = {"rev-parse", "-q", "--verify", ref_name, NULL};
	struct strbuf str_ref = STRBUF_INIT;
	int rout;
	rout = git_cmd_read_pipe_full(list_of_cmds, &str_ref);
	strbuf_release(&str_ref);
	return (rout == 0)?1:0;
}

int p4_refs_in_git(const char *ref_prefix, struct hashmap *map)
{
	struct child_process child_git = CHILD_PROCESS_INIT;
	struct strbuf sbline = STRBUF_INIT;
	FILE *fp;
	child_git.git_cmd = 1;
	child_git.out = -1;
	argv_array_push(&child_git.args, "show-ref");
	if (start_command(&child_git))
		die("cannot start git process");
	fp = fdopen(child_git.out,"r");
	while (EOF != strbuf_getline(&sbline, fp)) {
		const char *sp_ptr = strchr(sbline.buf, ' ');
		const char *ref_ptr = sp_ptr + 1;
		if (NULL == sp_ptr)
			continue;
		if (!starts_with(ref_ptr, ref_prefix))
			continue;
		if (ends_with(sbline.buf, "/HEAD"))
			continue;
		str_dict_set_key_valf(map, ref_ptr,"%.*s", sp_ptr - sbline.buf, sbline.buf);
	}
	fclose(fp);
	strbuf_release(&sbline);
	return finish_command(&child_git);
}


struct extract_log_message_t {
	int found_title;
	struct strbuf *logmessage;
};


static void extract_log_message_and_job_cb(struct strbuf *sbline, void *arg)
{
	struct extract_log_message_t *logst = (struct extract_log_message_t *) arg;
	if (!logst->found_title) {
		if (sbline->len == 1)
			logst->found_title = 1;
		return;
	}
	strbuf_addbuf(logst->logmessage, sbline);
}

void extract_log_message(const char *commit, struct strbuf *sb)
{
	const char *cmd_list[] = {"cat-file", "commit", commit, NULL};
	struct extract_log_message_t logst = {0, NULL};
	logst.logmessage = sb;
	if (git_cmd_read_pipe_line(cmd_list, extract_log_message_and_job_cb, &logst) != 0)
		die("Error extract log from commit %s", commit);
}

static void strbuf_strip_boundaries(struct strbuf *sb, const char * boundaries, int optional)
{
	int l_delim;
	int r_delim;
	if (!boundaries[0])
		die("Wrong boundaries settings");
	l_delim = boundaries[0];
	if (!boundaries[1])
		r_delim = l_delim;
	else
		r_delim = boundaries[1];
	strbuf_trim(sb);
	if ((sb->len > 1) && (sb->buf[0] == l_delim) && (sb->buf[sb->len-1] == r_delim)) {
		strbuf_remove(sb, 0, 1);
		strbuf_setlen(sb,sb->len-1);
	}
	else if (!optional) {
		strbuf_setlen(sb,0);
	}
}

static int keyval_equal_assign(keyval_t *kw, const char *str)
{
	struct strbuf **strbuf_list = NULL;
	struct strbuf **sb_iter = NULL;
	struct strbuf *key, *val;
	int ret = -1;
	strbuf_list = strbuf_split_str(str, '=', 0);
	if (!strbuf_list)
		goto _leave;
	if (!strbuf_list[0] || !strbuf_list[1])
		goto _leave;
	for (sb_iter=strbuf_list;*sb_iter;sb_iter++) {
		struct strbuf *sb = *sb_iter;
		if (sb->len && sb->buf[sb->len-1] == '=') {
			strbuf_setlen(sb,sb->len-1);
		}
		strbuf_trim(sb);
	}
	key = strbuf_list[0];
	val = strbuf_list[1];
	strbuf_reset(&kw->key);
	strbuf_addf(&kw->key,"%s",key->buf);
	strbuf_strip_boundaries(val,"\"",1);
	strbuf_reset(&kw->val);
	strbuf_addf(&kw->val,"%s",val->buf);
	ret = 0;
_leave:
	if (strbuf_list)
		strbuf_list_free(strbuf_list);
	return ret;
}

int extract_p4_settings_git_log(struct hashmap *map, const char *log)
{
	struct string_list colon_list = STRING_LIST_INIT_DUP;
	struct string_list_item *item, *equal_item;
	keyval_t *kw = NULL;
	string_list_split(&colon_list, log, '\n',-1);
	for_each_string_list_item(item,&colon_list) {
		struct strbuf sb=STRBUF_INIT;
		struct string_list equal_list = STRING_LIST_INIT_DUP;
		int git_p4_found = 0;
		strbuf_addf(&sb,"%s",item->string);
		strbuf_strip_boundaries(&sb,"[]",0);
		string_list_split(&equal_list, sb.buf, ':', -1);
		for_each_string_list_item(equal_item,&equal_list) {
			if (!git_p4_found && (strcmp(equal_item->string,"git-p4") == 0))
				git_p4_found = 1;
			if (git_p4_found) {
				kw = keyval_init(NULL);
				if (keyval_equal_assign(kw, equal_item->string) == 0)
					str_dict_put_kw(map,kw);
				else
					keyval_release(kw);
			}
		}
		strbuf_release(&sb);
		string_list_clear(&equal_list,0);
	}
	string_list_clear(&colon_list,0);
	return 0;
}

void strbuf_add_gitp4(struct strbuf *sb, const char *depot_path, int cl)
{
	strbuf_addf(sb, "[git-p4: depot-paths = \"%s", depot_path);
	if (!ends_with(depot_path, "/"))
		strbuf_addch(sb, '/');
	strbuf_addf(sb, "\": change = %d]", cl);
}

void strbuf_add_gitp4_df(struct strbuf *sb, const struct depot_file_t *p)
{
	if (p->is_revision)
		die("Revision not supported");
	strbuf_add_gitp4(sb, p->depot_path_file.buf, p->chg_rev);
}

void strbuf_add_gitp4_cl_desc(struct strbuf *sb, const struct depot_changelist_desc_t *p)
{
	int cl;
	if (p->change_source != CHANGE_SRC_P4)
		die("Only p4 descriptions");
	cl = atoi(p->changelist_or_commit.buf);
	strbuf_add_gitp4(sb, p->depot_base.buf, cl);
}

int find_p4_depot_commit(struct strbuf *strb_commit, struct depot_file_t *p)
{
	struct argv_array gitargs = ARGV_ARRAY_INIT;
	struct strbuf git_p4_line = STRBUF_INIT;
	int rout = -1;
	strbuf_add_gitp4_df(&git_p4_line, p);
	argv_array_push(&gitargs, "log");
	argv_array_push(&gitargs, "--format=format:%H");
	argv_array_push(&gitargs, "--first-parent");
	argv_array_push(&gitargs, "--remotes=p4");
	argv_array_push(&gitargs, "-1");
	argv_array_push(&gitargs, "-F");
	argv_array_push(&gitargs, "--grep");
	argv_array_push(&gitargs, git_p4_line.buf);
	strbuf_reset(strb_commit);
	rout = git_cmd_read_pipe_full(gitargs.argv, strb_commit);
	strbuf_trim(strb_commit);
	if (strb_commit->len)
		rout = 0;
	else
		rout = -1;
	strbuf_release(&git_p4_line);
	argv_array_clear(&gitargs);
	return rout;
}

int find_p4_parent_commit(struct strbuf *strb_commit, struct hashmap *p4settings)
{
	const char *cmd_list[] = {"log",
		"--format=format:%H",
		"--first-parent",
		"--grep",
		"^ *\\[git-p4: .*\\]$",
		"-1",
		"HEAD",
		NULL};
	const char *depot_path = NULL;
	struct strbuf sb = STRBUF_INIT;
	int rout = -1;
	strbuf_reset(strb_commit);
	rout = git_cmd_read_pipe_full(cmd_list, strb_commit);
	strbuf_trim(strb_commit);
	if (!rout && strb_commit->len > 0) {
		str_dict_reset(p4settings);
		extract_log_message(strb_commit->buf,&sb);
		rout = extract_p4_settings_git_log(p4settings, sb.buf);
		depot_path = str_dict_get_value(p4settings, "depot-paths");
	}
	else
		rout = -1;
	if (!depot_path)
		rout = -1;
	strbuf_release(&sb);
	return rout;
}

int find_upstream_branch_point(int local,struct strbuf *upstream, struct hashmap *p4settings )
{
	struct strbuf parent_commit = STRBUF_INIT;
	int ret = -1;
	if (find_p4_parent_commit(&parent_commit,p4settings) == 0) {
		const char *depot_path = str_dict_get_value(p4settings,"depot-paths");
		strbuf_reset(upstream);
		strbuf_addbuf(upstream, &parent_commit);
		ret = 0;
	}
	else 
		ret = -1;
	strbuf_release(&parent_commit);
	return ret;
}

static int git_list_commits(const char *origin, const char *head, struct strbuf *commits)
{
	struct argv_array args = ARGV_ARRAY_INIT;
	int ret;
	argv_array_push(&args, "rev-list");
	argv_array_push(&args, "--reverse");
	argv_array_push(&args, "--no-merges");
	argv_array_pushf(&args, "%s..%s",origin, head==NULL||strlen(head)==0?"HEAD":head);
	strbuf_reset(commits);
	ret = git_cmd_read_pipe_full(args.argv, commits);
	argv_array_clear(&args);
	return ret;

}

static void git_print_short_log(FILE *fp, const char *commit)
{
	struct strbuf sb_out = STRBUF_INIT;
	const char *cmd_show_list[] = {"show", "-s",
		"--format=format:%h %s", commit,
		NULL};
	git_cmd_read_pipe_full(cmd_show_list, &sb_out); 
	fprintf(fp, " %s\n", sb_out.buf);
	strbuf_release(&sb_out);
}


static int git_apply_commit(const char *commit_id, const char *dir, int check_only)
{
	struct child_process child_diff_tree = CHILD_PROCESS_INIT;
	struct child_process child_apply = CHILD_PROCESS_INIT;
	int child_diff_tree_ret;
	int child_apply_ret;
	argv_array_push(&child_diff_tree.args, "diff-tree");
	argv_array_push(&child_diff_tree.args, "--full-index");
	argv_array_push(&child_diff_tree.args, "-p");
	argv_array_push(&child_diff_tree.args, commit_id);
	child_diff_tree.git_cmd=1;
	child_diff_tree.out=-1;
	if (start_command(&child_diff_tree)) {
		die("cannot start git diff-tree");
	}
	child_apply.git_cmd=1;
	child_apply.in=child_diff_tree.out;
	argv_array_pushl(&child_apply.args,
			"--git-dir", absolute_path(get_git_dir()),
			"apply", "--ignore-whitespace", "--check", NULL);
	if (!check_only)
		argv_array_push(&child_apply.args, "--apply");
	if (dir)
		child_apply.dir = dir;
	argv_array_push(&child_apply.args, "-");
	if (start_command(&child_apply)) {
		die("cannot start git apply");
	}
	child_diff_tree_ret = finish_command(&child_diff_tree);
	child_apply_ret = finish_command(&child_apply);
	if (child_apply_ret)
		return child_apply_ret;
	return child_diff_tree_ret;
}

struct depot_client_path_t {
	struct strbuf *depot_path;
	struct strbuf *client_path;
};

static void p4_where_cb(struct hashmap *map, void *arg)
{
	struct depot_client_path_t *dc_arg = (struct depot_client_path_t *) arg;
	struct strbuf *argin_depot_path = dc_arg->depot_path;
	struct strbuf *argout_client_path = dc_arg->client_path;
	const char *depot_file = NULL;
	const char *data_str = NULL;
	const char *code_str = NULL;
	depot_file = str_dict_get_value(map, "depotFile");
	data_str = str_dict_get_value(map, "data");
	code_str = str_dict_get_value(map, "code");
	if (code_str && (strcmp(code_str, "error") == 0)) {
		strbuf_reset(argout_client_path);
		return;
	}
	if (depot_file) {
		size_t df_len;
		if (strncmp(depot_file,argin_depot_path->buf,argin_depot_path->len) != 0) 
			return;
		df_len = strlen(depot_file);
		if (df_len < 4)
			return;
		if (strcmp(depot_file+df_len-4,"/...") != 0)
			return;
		strbuf_reset(argout_client_path);
		strbuf_addstr(argout_client_path, str_dict_get_value(map, "path"));
	}
	else if (data_str) {
		struct strbuf **data_split_list = strbuf_split_str(data_str, ' ', 2);
		struct strbuf *l,*r;
		l = data_split_list[0];
		r = data_split_list[1];
		if (l)
			strbuf_trim(l);
		if (r)
			strbuf_trim(r);
		if (l && (strcmp(l->buf,argin_depot_path->buf) == 0) && r) {
			strbuf_reset(argout_client_path);
			strbuf_addbuf(argout_client_path, r);
		}
		strbuf_list_free(data_split_list);
	}
}

static int p4_where(const char *depot_path, struct strbuf *client_path)
{
	struct strbuf strb_depotpath = STRBUF_INIT;
	int ret = -1;
	strbuf_reset(client_path);
	strbuf_addstr(&strb_depotpath, depot_path);
	if (strb_depotpath.len) {
		struct depot_client_path_t arg_cb;
		struct argv_array p4args = ARGV_ARRAY_INIT;
		argv_array_push(&p4args, "where");
		strbuf_strip_suffix(&strb_depotpath,"/");
		strbuf_addf(&strb_depotpath,"/");
		argv_array_pushf(&p4args,"%s...",strb_depotpath.buf);
		arg_cb.depot_path = &strb_depotpath;
		arg_cb.client_path = client_path;
		p4_cmd_run(p4args.argv, NULL, p4_where_cb, &arg_cb);
		strbuf_strip_suffix(client_path,"...");
		argv_array_clear(&p4args);
	}
	strbuf_release(&strb_depotpath);
	return ret;
}

static int p4_sync(const char *client_path, struct string_list *local_files, const char *version_suffix, int force_sync)
{
	struct argv_array sync_args = ARGV_ARRAY_INIT;
	int retp4 = -1;
	argv_array_push(&sync_args, "sync");
	if (force_sync)
		argv_array_push(&sync_args, "-f");
	if (!local_files) {
		argv_array_pushf(&sync_args, "...%s", version_suffix);
	}
	else {
		struct string_list_item *item;
		for_each_string_list_item(item, local_files) {
			argv_array_pushf(&sync_args, "%s%s", item->string, version_suffix);
		}
	}
	retp4 = p4_cmd_run(sync_args.argv, client_path, NULL, NULL);
	argv_array_clear(&sync_args);
	return retp4;
}

static int p4_sync_file_opt(const char *client_path, const char *filename, const char *version_suffix, int force_sync)
{
	struct string_list syncfiles = STRING_LIST_INIT_DUP;
	int retp4sync = 0;
	string_list_insert(&syncfiles, filename);
	retp4sync = p4_sync(client_path, &syncfiles, version_suffix, force_sync);
	string_list_clear(&syncfiles, 0);
	return retp4sync;
}

static int p4_sync_force_file(const char *client_path, const char *filename, const char *version_suffix)
{
	return p4_sync_file_opt(client_path, filename, version_suffix, 1);
}

void p4debug_cmd_init(struct command_t *pcmd)
{
	strbuf_init(&pcmd->strb_usage, 256);
	strbuf_addf(&pcmd->strb_usage, "A tool to debug the output of p4 -G");
	pcmd->needs_git = 0;
	pcmd->verbose = 0;
	pcmd->run_fn = p4debug_cmd_run;
	pcmd->deinit_fn = p4_cmd_default_deinit; 
	pcmd->data = NULL;
}



static void p4usermap_get_id_cb(struct hashmap *map, void *argout)
{
	const char *user_str = NULL;
	struct strbuf *sb_out = (struct strbuf *) argout;
	user_str = str_dict_get_value(map,"User");
	if (user_str) {
		strbuf_reset(sb_out);
		strbuf_addstr(sb_out, user_str);
	}
}

void p4usermap_get_id(struct p4_user_map_t *user_map, struct strbuf *sb_id)
{
	struct hashmap p4_dict;
	str_dict_init(&p4_dict);
	if (!user_map->my_p4_user_id.len) {
		const char *p4_cmd_list[] = {"user", "-o", NULL};
		p4_cmd_run(p4_cmd_list, NULL, p4usermap_get_id_cb, &user_map->my_p4_user_id);
	}
	if (!user_map->my_p4_user_id.len) {
		die("Could not find your p4 user id");
	}
	strbuf_reset(sb_id);
	strbuf_addbuf(sb_id, &user_map->my_p4_user_id);
}


static void p4usermap_add_user(struct p4_user_map_t *p4_user_map,
		const char *user, const char *email, const char *full_name)
{
	if ((!user) || (!email) || (!full_name))
		return;
	str_dict_set_key_valf(&p4_user_map->users, user, "%s <%s>", full_name, email);
	str_dict_set_key_val(&p4_user_map->emails, email, user);
}

static void p4usermap_update_users_info_cb(struct hashmap *map, void *arg1)
{
	struct p4_user_map_t *p4_user_map = (struct p4_user_map_t *) arg1;
	const char *user = str_dict_get_value(map, "User");
	const char *email = str_dict_get_value(map, "Email");
	const char *full_name = str_dict_get_value(map, "FullName");
	p4usermap_add_user(p4_user_map, user, email, full_name);
}

void p4usermap_update_users_info(struct p4_user_map_t *user_map)
{
	const char *p4_cmd_list[] = {"users", NULL};
	p4_cmd_run(p4_cmd_list, NULL, p4usermap_update_users_info_cb, user_map);
}

static int p4usermap_git_config(const char *k, const char *v, void *arg)
{
	struct strbuf **strb_list = NULL;
	struct strbuf **strb_iter = NULL;
	struct p4_user_map_t *p4_user_map = (struct p4_user_map_t *) arg;
	if (!strcasecmp(k,"git-p4.mapuser")) {
		strb_list = strbuf_split_str(v, '\n', -1);
		for (strb_iter = strb_list; *strb_iter; strb_iter++) {
			struct keyval_t kw;
			struct strbuf **name_email_list;
			strbuf_trim(*strb_iter);
			keyval_init(&kw);
			keyval_equal_assign(&kw, (*strb_iter)->buf);
			name_email_list = strbuf_split(&kw.val,'<');
			if (name_email_list[0] && name_email_list[1] && !name_email_list[2]) {
				strbuf_strip_suffix(name_email_list[0],"<");
				strbuf_trim(name_email_list[0]);
				strbuf_trim(name_email_list[1]);
				if (strbuf_strip_suffix(name_email_list[1],">")) {
					p4usermap_add_user(p4_user_map, kw.key.buf,
							name_email_list[1]->buf, name_email_list[0]->buf);
				}
			}
			keyval_release(&kw);
			strbuf_list_free(name_email_list);
		}
		strbuf_list_free(strb_list);
	}
	return 0;
}

void p4usermap_init(struct p4_user_map_t *user_map)
{
	strbuf_init(&user_map->my_p4_user_id, 0);
	str_dict_init(&user_map->users);
	str_dict_init(&user_map->emails);
	p4usermap_update_users_info(user_map);
	git_config(p4usermap_git_config, user_map);
}

void p4usermap_deinit(struct p4_user_map_t *user_map)
{
	str_dict_destroy(&user_map->users);
	str_dict_destroy(&user_map->emails);
	strbuf_release(&user_map->my_p4_user_id);
}

enum { CONFLICT_ASK, CONFLICT_SKIP, CONFLICT_QUIT, CONFLICT_UNKNOWN};

struct p4submit_data_t {
	struct strbuf base_commit;
	struct strbuf branch; 
	struct strbuf depot_path;
	struct strbuf client_path;
	struct strbuf diff_opts;
	struct strbuf cl_suffix;
	int detect_renames;
	int detect_copies;
	int detect_copies_harder;
	int preserve_user;
	int export_labels;
	int dry_run;
	int prepare_p4_only;
	int conflict_behavior;
	int shelve;
	int update_shelve_cl;
	int check_authorship;
	int skip_user_name_check;
	struct strbuf **allow_submit;
} p4submit_options;

const char * p4submit_usage[] = {
	N_("git p4 submit"),
	NULL
};


static int parse_conflict_mode(const char *s)
{
	if (!strcmp(s, "ask"))
		return CONFLICT_ASK;
	else if (!strcmp(s, "skip"))
		return CONFLICT_SKIP;
	else if (!strcmp(s, "quit"))
		return CONFLICT_QUIT;
	else
		return CONFLICT_UNKNOWN;
}

static int p4submit_cmd_parse_conflict_mode(const struct option *opt,
		const char *arg, int unset)
{
	int *v = (int *) opt->value;
	*v = CONFLICT_UNKNOWN;
	if (unset)
		*v = CONFLICT_ASK;
	else
		*v = parse_conflict_mode(arg);
	if (*v == CONFLICT_UNKNOWN)
		return error("Unknown conflict-behavior mode: %s", arg);
	return 0;
}

static void p4submit_cmd_deinit(struct command_t *pcmd)
{
	strbuf_release(&p4submit_options.base_commit);
	strbuf_release(&p4submit_options.branch);
	strbuf_release(&p4submit_options.depot_path);
	strbuf_release(&p4submit_options.client_path);
	strbuf_release(&p4submit_options.diff_opts);
	strbuf_release(&p4submit_options.cl_suffix);
	if (p4submit_options.allow_submit)
		strbuf_list_free(p4submit_options.allow_submit);
	p4submit_options.allow_submit = NULL;
	p4_cmd_default_deinit(pcmd);
}

static void parse_diff_tree_entry(struct hashmap *map, const char *l)
{
	struct strbuf **str_sp_list;
	struct strbuf **str_tab_list;
	struct strbuf **strb_iter;
	int n;
	if (l[0] != ':')
		die("A : expected a first character %s",l);
	str_sp_list = strbuf_split_str(l+1,' ',5);
	for (strb_iter=str_sp_list,n=0;*strb_iter;strb_iter++,n++) {
		strbuf_trim(*strb_iter);
	}
	if (n != 5)
		die("Error parsing diff line %s", l);
	str_tab_list = strbuf_split(str_sp_list[4],'\t');
	for (strb_iter=str_tab_list,n=0;*strb_iter;strb_iter++,n++) {
		strbuf_trim(*strb_iter);
	}
	if (n < 2)
		die("Error parsing diff line %s", str_tab_list[0]->buf);
	str_dict_set_key_val(map, "src_mode", str_sp_list[0]->buf);
	str_dict_set_key_val(map, "dst_mode", str_sp_list[1]->buf);
	str_dict_set_key_val(map, "src_sha1", str_sp_list[2]->buf);
	str_dict_set_key_val(map, "dst_sha1", str_sp_list[3]->buf);
	if (str_tab_list[0]->len < 1)
		die("Unexpected length for status field");
	else if (str_tab_list[0]->len > 1)
		str_dict_set_key_val(map, "status_score", str_tab_list[0]->buf + 1);
	strbuf_setlen(str_tab_list[0],1);
	str_dict_set_key_val(map, "status", str_tab_list[0]->buf);
	str_dict_set_key_val(map, "src", str_tab_list[1]->buf);
	if ((str_tab_list[0]->buf[0] == 'C') ||
			(str_tab_list[0]->buf[0] == 'R')) {
		if (n < 5)
			die("Error parsing diff %s", str_sp_list[4]->buf);
		str_dict_set_key_val(map, "dst", str_tab_list[4]->buf);
	}
	strbuf_list_free(str_tab_list);
	strbuf_list_free(str_sp_list);
}

struct files_modified_t {
	struct string_list added;
	struct string_list type_changed;
	struct string_list deleted;
	struct string_list edited;
	struct string_list renamed_copied;
	struct string_list symlinks;
	struct hashmap exec_bit_changed;
	struct string_list all_files;
};

static inline void p4_files_modified_init(struct files_modified_t *fm)
{
	string_list_init(&fm->added, 1);
	string_list_init(&fm->type_changed, 1);
	string_list_init(&fm->deleted, 1);
	string_list_init(&fm->edited, 1);
	string_list_init(&fm->renamed_copied, 1);
	string_list_init(&fm->symlinks, 1);
	str_dict_init(&fm->exec_bit_changed);
	string_list_init(&fm->all_files, 1);
}

static inline void p4_files_modified_destroy(struct files_modified_t *fm)
{
	string_list_clear(&fm->added, 0);
	string_list_clear(&fm->type_changed, 0);
	string_list_clear(&fm->deleted, 0);
	string_list_clear(&fm->edited, 0);
	string_list_clear(&fm->renamed_copied, 0);
	string_list_clear(&fm->symlinks, 0);
	str_dict_destroy(&fm->exec_bit_changed);
	string_list_clear(&fm->all_files, 0);
}


static int wildcard_present(const char *path)
{
	const char wildcards[] = "[*#@%]";
	const char *pch;
	for (pch=wildcards;*pch;pch++) {
		if (strchr(path, *pch))
			return 1;
	}
	return 0;
}

static void wildcard_encode(struct strbuf *sb)
{
	size_t i;
	struct strbuf sb_tmp = STRBUF_INIT;
	for (i=0; i<sb->len; i++) {
		char c = sb->buf[i];
		switch (c) {
		case '%':
			strbuf_addstr(&sb_tmp, "%25");
			break;
		case '*':
			strbuf_addstr(&sb_tmp, "%2A");
			break;
		case '#':
			strbuf_addstr(&sb_tmp, "%23");
			break;
		case '@':
			strbuf_addstr(&sb_tmp, "%40");
			break;
		default:
			strbuf_addch(&sb_tmp, c);
			break;
		}
	}
	strbuf_reset(sb);
	strbuf_addbuf(sb,&sb_tmp);
	strbuf_release(&sb_tmp);
}

static void wildcard_decode(struct strbuf *sb, const char *format)
{
	struct strbuf_expand_dict_entry map_table[] = {
		{ "25", "%"},
		{ "2A", "*"},
		{ "2a", "*"},
		{ "23", "#"},
		{ "40", "@"},
		{ NULL, NULL}
	};
	strbuf_expand(sb, format, strbuf_expand_dict_cb, map_table);
}

static int p4_edit(const char *client_dir, const char *path, int auto_type)
{
	struct argv_array p4args = ARGV_ARRAY_INIT;
	struct strbuf q_path = STRBUF_INIT;
	int rout = -1;
	argv_array_push(&p4args, "edit");
	if (auto_type) {
		argv_array_push(&p4args, "-t");
		argv_array_push(&p4args, "auto");
	}
	strbuf_addstr(&q_path,path);
	wildcard_encode(&q_path);
	argv_array_push(&p4args, q_path.buf);
	rout = p4_cmd_run(p4args.argv, client_dir, NULL, NULL);
	strbuf_release(&q_path);
	argv_array_clear(&p4args);
	return rout;
}

static int p4_add(const char *client_dir, const char *path)
{
	struct argv_array p4args = ARGV_ARRAY_INIT;
	int rout;
	argv_array_push(&p4args, "add");
	if (wildcard_present(path))
		argv_array_push(&p4args, "-f");
	argv_array_push(&p4args, path);
	rout = p4_cmd_run(p4args.argv, client_dir, NULL, NULL);
	argv_array_clear(&p4args);
	return rout;
}

static int p4_revert(const char *client_dir, const char *path)
{
	struct argv_array p4args = ARGV_ARRAY_INIT;
	struct strbuf q_path = STRBUF_INIT;
	int rout;
	argv_array_push(&p4args, "revert");
	strbuf_addstr(&q_path,path);
	wildcard_encode(&q_path);
	argv_array_push(&p4args, q_path.buf);
	rout = p4_cmd_run(p4args.argv, client_dir, NULL, NULL);
	argv_array_clear(&p4args);
	strbuf_release(&q_path);
	return rout;
}

static int p4_delete(const char *client_dir, const char *path)
{
	struct argv_array p4args = ARGV_ARRAY_INIT;
	struct strbuf q_path = STRBUF_INIT;
	int rout;
	argv_array_push(&p4args, "delete");
	strbuf_addstr(&q_path,path);
	wildcard_encode(&q_path);
	argv_array_push(&p4args, q_path.buf);
	rout = p4_cmd_run(p4args.argv, client_dir, NULL, NULL);
	argv_array_clear(&p4args);
	strbuf_release(&q_path);
	return rout;
}

static int is_git_mode_exec_changed(const char *src_mode, const char *dst_mode)
{
	return (ends_with(src_mode,"755") != ends_with(dst_mode,"755"));
}

static void str_dict_remove_non_depot_files(struct hashmap *map, const char *depot_path)
{
	struct hashmap_iter hm_iter;
	struct hashmap_entry *entry;
	struct hashmap maptmp;
	str_dict_init(&maptmp);
	hashmap_iter_init(map, &hm_iter);
	while ((entry = hashmap_iter_next(&hm_iter))) {
		const keyval_t *kw = container_of(entry, const keyval_t, ent);
		if (starts_with(kw->key.buf, "File") && !starts_with(kw->val.buf, depot_path)) {
			continue;
		}
		str_dict_set_key_val(&maptmp, kw->key.buf, kw->val.buf);
	}
	SWAP(*map, maptmp);
	str_dict_destroy(&maptmp);
}

static void strbuf_add_p4change_multiple_fields(struct strbuf *out, struct hashmap *map, const char *prefix_field, const char *output_field_name)
{
	struct hashmap_iter hm_iter;
	struct hashmap_entry *entry;
	hashmap_iter_init(map, &hm_iter);
	strbuf_addf(out, "\n%s:\n", output_field_name);
	while ((entry = hashmap_iter_next(&hm_iter))) {
		const keyval_t *kw = container_of(entry, const keyval_t, ent);
		if (!starts_with(kw->key.buf, prefix_field))
			continue;
		strbuf_addf(out, "\t%s\n", kw->val.buf);
	}
}

static void strbuf_add_p4change_field(struct strbuf *out, struct hashmap *map, const char *field)
{
	const keyval_t *kw;
	struct string_list field_line_list = STRING_LIST_INIT_DUP;
	struct string_list_item *item;
	kw = str_dict_get_kw(map, field);
	if (!kw)
		return;
	string_list_split(&field_line_list, kw->val.buf, '\n', -1);
	strbuf_addf(out, "\n%s:", field);
	if (((kw->val.len + strlen(field)) > 78) || (field_line_list.nr > 1))
		strbuf_addf(out, "\n");
	for_each_string_list_item(item, &field_line_list)
		strbuf_addf(out, "\t%s\n", item->string);
	string_list_clear(&field_line_list, 0);
}

static void strbuf_add_p4change(struct strbuf *out, struct hashmap *map)
{
	strbuf_addstr(out, "# A Perforce Change Specification.\n");
	strbuf_addstr(out, "#\n");
	strbuf_addstr(out, "#  Change:      The change number. 'new' on a new changelist.\n");
	strbuf_addstr(out, "#  Date:        The date this specification was last modified.\n");
	strbuf_addstr(out, "#  Client:      The client on which the changelist was created.  Read-only.\n");
	strbuf_addstr(out, "#  User:        The user who created the changelist.\n");
	strbuf_addstr(out, "#  Status:      Either 'pending' or 'submitted'. Read-only.\n");
	strbuf_addstr(out, "#  Type:        Either 'public' or 'restricted'. Default is 'public'.\n");
	strbuf_addstr(out, "#  Description: Comments about the changelist.  Required.\n");
	strbuf_addstr(out, "#  Jobs:        What opened jobs are to be closed by this changelist.\n");
	strbuf_addstr(out, "#               You may delete jobs from this list.  (New changelists only.)\n");
	strbuf_addstr(out, "#  Files:       What opened files from the default changelist are to be added\n");
	strbuf_addstr(out, "#               to this changelist.  You may delete files from this list.\n");
	strbuf_addstr(out, "#               (New changelists only.)\n");
	strbuf_add_p4change_field(out, map, "Change");
	strbuf_add_p4change_field(out, map, "Client");
	strbuf_add_p4change_field(out, map, "User");
	strbuf_add_p4change_field(out, map, "Status");
	strbuf_add_p4change_field(out, map, "Description");
	strbuf_add_p4change_field(out, map, "Jobs");
	strbuf_add_p4change_multiple_fields(out, map, "File", "Files");
}

static void get_p4change_cb(struct hashmap *map, void *arg)
{
	struct hashmap *dst = (struct hashmap *) arg;
	const char *codestr=str_dict_get_value(map, "code");
	if (!codestr)
		return;
	if (!strcmp(codestr, "stat"))
		str_dict_copy(dst, map);
}

static void get_p4describe(struct hashmap *change_entry, unsigned int changelist)
{
	struct argv_array p4args = ARGV_ARRAY_INIT;
	argv_array_push(&p4args, "describe");
	argv_array_pushf(&p4args, "%u", changelist);
	p4_cmd_run(p4args.argv, NULL, get_p4change_cb, change_entry);
	if (!str_dict_get_value(change_entry, "code"))
		die("Failed to decode output of p4 describe");
	argv_array_clear(&p4args);
}


static void get_p4change(struct hashmap *change_entry, unsigned int changelist)
{
	struct argv_array p4args = ARGV_ARRAY_INIT;
	argv_array_push(&p4args, "change");
	argv_array_push(&p4args, "-o");
	if (changelist)
		argv_array_pushf(&p4args, "%u", changelist);
	p4_cmd_run(p4args.argv, NULL, get_p4change_cb, change_entry);
	if (!str_dict_get_value(change_entry, "code"))
		die("Failed to decode output of p4 change -o");
	argv_array_clear(&p4args);
}

void dump_p4_log(FILE *fp, const char *commit_id, unsigned int changelist)
{
	struct strbuf upstream = STRBUF_INIT;
	struct strbuf description = STRBUF_INIT;
	struct strbuf outlog = STRBUF_INIT;
	struct hashmap p4change;
	struct hashmap p4settings;
	const char *depot_path = NULL;
	str_dict_init(&p4change);
	str_dict_init(&p4settings);
	if (find_upstream_branch_point(0, &upstream, &p4settings) < 0)
		die("Error findind upstream");
	depot_path = str_dict_get_value(&p4settings, "depot-paths");
	extract_log_message(commit_id, &description);
	strbuf_trim(&description);
	get_p4change(&p4change, changelist);
	str_dict_remove_non_depot_files(&p4change, depot_path);
	str_dict_set_key_val(&p4change, "Description", description.buf);
	strbuf_add_p4change(&outlog, &p4change);
	strbuf_write(&outlog, fp);
	if (IS_LOG_DEBUG_ALLOWED) {
		strbuf_write(&outlog, p4_verbose_debug.fp);
	}
	strbuf_release(&description);
	strbuf_release(&outlog);
	strbuf_release(&upstream);
	str_dict_destroy(&p4change);
	str_dict_destroy(&p4settings);
}

static void p4submit_apply_cb(struct strbuf *l, void *arg)
{
	struct hashmap map;
	struct files_modified_t *files_to_update = (struct files_modified_t *)arg;
	const char *val;
	char modifier = '\0';
	struct strbuf src_path = STRBUF_INIT;
	struct strbuf dst_path = STRBUF_INIT;
	const char *src_mode, *dst_mode;
	const char *cli_path = p4submit_options.client_path.buf;
	const char *cl_suffix = p4submit_options.cl_suffix.buf;
	str_dict_init(&map);
	parse_diff_tree_entry(&map, l->buf);
	val = str_dict_get_value(&map, "status");
	if (val && val[0] && val[1] == '\0')
		modifier = val[0];
	else
		die("Wrong diff line parsed (status) %s",l->buf);
	val = str_dict_get_value(&map, "src");
	if (!val)
		die("Wrong diff line parsed (src) %s",l->buf);
	strbuf_addstr(&src_path, val);
	val = str_dict_get_value(&map, "dst");
	if (val)
		strbuf_addstr(&dst_path, val);
	src_mode = str_dict_get_value(&map, "src_mode");
	dst_mode = str_dict_get_value(&map, "dst_mode");
	if (IS_LOG_DEBUG_ALLOWED) {
		LOG_GITP4_DEBUG("Converted git info to dict: ");
		str_dict_print(p4_verbose_debug.fp, &map);
	}
	switch (modifier) {
	case 'M':
		p4_sync_force_file(cli_path, src_path.buf, cl_suffix);
		p4_edit(cli_path, src_path.buf,0);
		if (is_git_mode_exec_changed(src_mode, dst_mode))
			str_dict_set_key_val(&files_to_update->exec_bit_changed, src_path.buf, dst_mode);
		string_list_insert(&files_to_update->edited, src_path.buf);
		break;
	case 'A':
		string_list_insert(&files_to_update->added, src_path.buf);
		str_dict_set_key_val(&files_to_update->exec_bit_changed, src_path.buf, dst_mode);
		if (0120000 == strtol(dst_mode, NULL, 8))
			string_list_insert(&files_to_update->symlinks, src_path.buf);
		string_list_remove(&files_to_update->deleted, src_path.buf, 0);
		break;
	case 'D':
		p4_sync_force_file(cli_path, src_path.buf, cl_suffix);
		string_list_insert(&files_to_update->deleted, src_path.buf);
		string_list_remove(&files_to_update->added, src_path.buf, 0);
		break;
	case 'C':
#if 0
		p4_integrate(cli_path, src_path->buf, dst_path->buf);
		string_list_insert(&files_to_update->renamed_copied, dst_path->buf);
		if (strcmp(src_sha1, dst_sha1)) {
			p4_edit(cli_path, dst_path->buf);
			string_list_remove(&files_to_update->renamed_copied, dst_path->buf);
		}
		if (is_git_mode_exec_changed(src_mode, dst_mode)) {
			p4_edit(cli_path, dst_path->buf);
			string_list_remove(&files_to_update->renamed_copied, dst_path->buf);
			str_dict_set_key_val(&files_to_update->exec_bit_changed, dst_path->buf, dst_mode);
		}
#ifdef GIT_WINDOWS_NATIVE
		chmod(dst_path->buf, S_IWRITE);
#endif
		unlink(dst_path->buf);
		string_list_insert(&files_to_update->added, dst_path->buf);
#else
		LOG_GITP4_CRITICAL("Copy not implemented\n");
#endif
		break;
	case 'R':
		LOG_GITP4_CRITICAL("Rename not implemented\n");
		break;
	case 'T':
		string_list_insert(&files_to_update->type_changed, src_path.buf);
		break;
	default:
		die("Unknown modifier %c for %s", modifier, src_path.buf);
		break;
	}
	str_dict_destroy(&map);
	strbuf_release(&dst_path);
	strbuf_release(&src_path);
}

int p4_local_unlink(const char *cli_path, const char *local_path)
{
	struct strbuf unpath = STRBUF_INIT;
	int ret;
	strbuf_addstr(&unpath, cli_path);
	if (unpath.len && (!is_dir_sep(unpath.buf[unpath.len-1])))
		strbuf_addch(&unpath, '/');
	strbuf_addstr(&unpath, local_path);
	ret = unlink(unpath.buf);
	strbuf_release(&unpath);
	return ret;
}


int p4submit_apply(const char *commit_id)
{
	struct strbuf user_id = STRBUF_INIT;
	struct argv_array gitargs = ARGV_ARRAY_INIT;
	struct files_modified_t files_to_update;
	struct string_list_item *item;
	const char *cli_path = p4submit_options.client_path.buf;
	struct hashmap_iter hm_iter;
	struct child_process p4_submit = CHILD_PROCESS_INIT;
	int clean_opened_files = 1;
	struct hashmap_entry *entry;
	int res = 1;
	p4_files_modified_init(&files_to_update);
	argv_array_push(&gitargs, "diff-tree");
	argv_array_push(&gitargs, "-r");
	if (p4submit_options.diff_opts.len)
		argv_array_push(&gitargs, p4submit_options.diff_opts.buf);
	argv_array_pushf(&gitargs, "%s^", commit_id);
	argv_array_push(&gitargs, commit_id);
	fprintf(stdout,"Applying");
	safe_create_leading_directories_const(cli_path);
	mkdir(cli_path, 0755);
	git_print_short_log(stdout, commit_id);
	git_cmd_read_pipe_line(gitargs.argv, p4submit_apply_cb, &files_to_update);
	argv_array_clear(&gitargs);
	if (git_apply_commit(commit_id,p4submit_options.client_path.buf, 0)) {
		LOG_GITP4_CRITICAL("Error applying commit %s\n", commit_id);
		goto leave;
	}
	for_each_string_list_item(item,&files_to_update.type_changed) {
		p4_sync_force_file(cli_path, item->string, p4submit_options.cl_suffix.buf);
		p4_edit(cli_path, item->string, 1);
	}
	for_each_string_list_item(item,&files_to_update.added) {
		p4_add(cli_path, item->string);
	}
	for_each_string_list_item(item,&files_to_update.deleted) {
		p4_revert(cli_path, item->string);
		p4_sync_force_file(cli_path, item->string, p4submit_options.cl_suffix.buf);
		p4_delete(cli_path, item->string);
	}
	hashmap_iter_init(&files_to_update.exec_bit_changed, &hm_iter);
	while ((entry = hashmap_iter_next(&hm_iter))) {
		const keyval_t *kw = container_of(entry, const keyval_t, ent);
		p4_set_exec_git(cli_path, kw->key.buf, kw->val.buf);
	}
	argv_array_push(&gitargs, "p4");
	if (p4submit_options.update_shelve_cl) {
		argv_array_push(&gitargs, "shelve");
		argv_array_push(&gitargs, "-r");
		argv_array_push(&gitargs, "-i");
	}
	else if (p4submit_options.shelve) {
		argv_array_push(&gitargs, "shelve");
		argv_array_push(&gitargs, "-i");
	}
	else {
		argv_array_push(&gitargs, "submit");
		argv_array_push(&gitargs, "-i");
		clean_opened_files = 0;
	}
	p4_submit.in = -1;
	p4_submit.argv = gitargs.argv;
	if (start_command(&p4_submit)) {
		die("cannot start p4_submit");
	}
	FILE *fp = fdopen(p4_submit.in, "w");
	dump_p4_log(fp, commit_id, p4submit_options.update_shelve_cl);
	fclose(fp);
	if (finish_command(&p4_submit)) {
		LOG_GITP4_CRITICAL("Failed to submit change\n");
		clean_opened_files = 1;
	}
	if (clean_opened_files) {
		for_each_string_list_item(item,&files_to_update.edited) {
			p4_revert(cli_path, item->string);
		}
		for_each_string_list_item(item,&files_to_update.deleted) {
			p4_revert(cli_path, item->string);
		}
		for_each_string_list_item(item,&files_to_update.added) {
			p4_revert(cli_path, item->string);
			p4_local_unlink(cli_path, item->string);
		}
	}
	argv_array_clear(&gitargs);
	res = 0;
leave:
	strbuf_release(&user_id);
	p4_files_modified_destroy(&files_to_update);
	return res;
}

int p4submit_cmd_run(struct command_t *pcmd, int argc, const char **argv)
{
	struct hashmap map;
	struct strbuf strb_master = STRBUF_INIT;
	struct strbuf commits = STRBUF_INIT;
	const char *origin = NULL;
	const char *branch = NULL;
	int res = 0;
	struct option options[] = {
		OPT_STRING(0,"origin",&origin,N_("revision"), N_("Base commit point")),
		OPT_BOOL('M',NULL,&p4submit_options.detect_renames,N_("Detect Renames")),
		OPT_BOOL(0,"preserve-user",&p4submit_options.preserve_user,N_("Preserve User")),
		OPT_BOOL(0,"export-labels",&p4submit_options.export_labels,N_("Export Labels")),
		OPT_BOOL('n',"dry-run",&p4submit_options.dry_run,N_("dry run")),
		OPT_BOOL(0,"prepare-p4-only",&p4submit_options.prepare_p4_only,N_("Prepare p4 only")),
		OPT_CALLBACK(0,"conflict", &p4submit_options.conflict_behavior, N_("mode"),
				N_("set conflict behavior: [ask skip quit]"),
				p4submit_cmd_parse_conflict_mode),
		OPT_STRING(0,"branch",&branch,N_("branch"), N_("Remote p4 branch to update")), 
		OPT_BOOL(0,"shelve",&p4submit_options.shelve,N_("Shelve instead of submit. Shelved files are reverted,"
					"restoring the workspace to the state before the shelve")), 
		OPT_INTEGER(0,"update-shelve",&p4submit_options.update_shelve_cl,N_("update an existing shelved changelist, implies --shelve")),
		OPT_END()
	};
	argc = parse_options(argc, argv, NULL, options, p4submit_usage ,0);
	if (IS_LOG_DEBUG_ALLOWED && p4submit_options.allow_submit) {
		struct strbuf **strb;
		for (strb = p4submit_options.allow_submit; *strb;strb ++) {
			fprintf(stdout,"%s$\n", (*strb)->buf);
		}
	}
	if (origin) {
		strbuf_reset(&p4submit_options.base_commit);
		strbuf_addstr(&p4submit_options.base_commit, origin);
	}

	if (branch) {
		strbuf_reset(&p4submit_options.branch);
		strbuf_addf(&p4submit_options.branch, "%s",branch);
	}
	if (argc == 0) {
		if (current_git_branch(&strb_master) != 0)
			die("Couldn't find current git branch");
	}
	else if (argc == 1) {
		strbuf_addstr(&strb_master, argv[0]);
		if (!branch_exists(strb_master.buf))
			die("Branch %s does not exist",strb_master.buf);
	}
	else
		die("Wrong p4 submit parameters");
	do {
		struct strbuf base_commit = STRBUF_INIT;
		struct hashmap dict_map;
		str_dict_init(&dict_map);
		if (find_upstream_branch_point(0, &base_commit, &dict_map) == 0) {
			strbuf_reset(&p4submit_options.depot_path);
			strbuf_addf(&p4submit_options.depot_path,"%s", str_dict_get_value(&dict_map,"depot-paths"));
			strbuf_setlen(&p4submit_options.cl_suffix, 0);
			strbuf_addf(&p4submit_options.cl_suffix, "@%s", str_dict_get_value(&dict_map, "change"));
			if (p4submit_options.base_commit.len == 0)
				strbuf_addbuf(&p4submit_options.base_commit, &base_commit);
			LOG_GITP4_DEBUG("Upstream: %s\n",base_commit.buf);
			if (IS_LOG_DEBUG_ALLOWED)
				str_dict_print(p4_verbose_debug.fp, &dict_map);
			LOG_GITP4_DEBUG("Upstream: %s\n",p4submit_options.base_commit.buf);
			LOG_GITP4_DEBUG("depot-path: %s\n", p4submit_options.depot_path.buf);
		}
		strbuf_release(&base_commit);
		str_dict_destroy(&dict_map);
	} while(0);

	if (p4submit_options.update_shelve_cl)
		p4submit_options.shelve = 1;
	if (!p4submit_options.shelve) {
		strbuf_setlen(&p4submit_options.cl_suffix, 0);
	}
	if (p4submit_options.preserve_user &&
			!p4_has_admin_permissions(p4submit_options.depot_path.buf))
		die("Cannot preserve user names without p4 super-user or admin permissions");
	p4_where(p4submit_options.depot_path.buf,&p4submit_options.client_path);
	if (!p4submit_options.client_path.len)
		die("Error: Cannot locate perforce checkout of %s in client view", p4submit_options.depot_path.buf);
	fprintf(stdout, "Perforce checkout for depot path %s located at %s\n",
			p4submit_options.depot_path.buf,
			p4submit_options.client_path.buf);
	if (p4submit_options.dry_run) {
		fprintf(stdout, "Would synchronize p4 checkout in %s\n", p4submit_options.client_path.buf);
	}
	if (p4_nfiles_opened(p4submit_options.client_path.buf))
		die("You have files opened with perforce! Close them before starting the sync.");
	git_list_commits(p4submit_options.base_commit.buf, strb_master.buf, &commits);
	if (p4submit_options.preserve_user && p4submit_options.skip_user_name_check) 
		p4submit_options.check_authorship = 1;
	else
		p4submit_options.check_authorship = 0;
	if (p4submit_options.preserve_user)
		die("Preserve users not supported yet");
	strbuf_reset(&p4submit_options.diff_opts);
	if (p4submit_options.detect_renames)
		strbuf_addstr(&p4submit_options.diff_opts, "-M");
	if (p4submit_options.detect_copies)
		strbuf_addstr(&p4submit_options.diff_opts, " -C");
	if (p4submit_options.detect_copies_harder)
		strbuf_addstr(&p4submit_options.diff_opts, " --find-copies-harder");

	if (p4submit_options.dry_run)
		fprintf(stdout, "Would apply\n");
	if (commits.len) {
		struct strbuf **strb_list;
		struct strbuf **strb_iter;
		strb_list = strbuf_split(&commits,'\n');
		for (strb_iter = strb_list; *strb_iter; strb_iter++) {
			const char *commit_id;
			strbuf_trim(*strb_iter);
			commit_id = (*strb_iter)->buf;
			if (p4submit_options.dry_run) {
				git_print_short_log(stdout, commit_id);
			}
			else if ((res = p4submit_apply(commit_id)) != 0)
				break;
		}
		strbuf_list_free(strb_list);
	}
	strbuf_release(&strb_master);
	strbuf_release(&commits);
	return res;
}

static struct p4_user_map_t *p4usermap_get_cache()
{
	if (p4usermap_cached)
		return p4usermap_cached;
	p4usermap_cached = malloc(sizeof(struct p4_user_map_t));
	if (!p4usermap_cached)
		die("Out of memory");
	p4usermap_init(p4usermap_cached);
	return p4usermap_cached;
}

static void p4usermap_cache_destroy()
{
	if (!p4usermap_cached)
		return;
	p4usermap_deinit(p4usermap_cached);
	p4usermap_cached = NULL;
}

#if 0
static const char *p4usermap_cache_get_user_by_email(const char *email)
{
	struct p4_user_map_t *p4users = p4usermap_get_cache();
	return str_dict_get_value(&p4users->emails, email);
}
#endif

static const char *p4usermap_cache_get_name_email_str_by_user(const char *user)
{
	struct p4_user_map_t *p4users = p4usermap_get_cache();
	const char *email = str_dict_get_value(&p4users->users, user);
	if (email)
		return email;
	str_dict_set_key_valf(&p4users->users, user, "%s <>", user);
	return str_dict_get_value(&p4users->users, user);
}

static int p4submit_git_config(const char *k, const char *v, void *cb)
{
	if (!strcasecmp(k, "git-p4.preserveUser")) {
		p4submit_options.preserve_user = git_config_bool(k,v);
	}
	else if (!strcasecmp(k, "git-p4.largeFileSystem")) {
		die("Large file system not supported for git-p4 submit command. Please remove it from config."); 
	}
	else if (!strcasecmp(k, "git-p4.useclientspec")) {
		die("option git-p4.useclientspec not supported"); 
	}
	else if (!strcasecmp(k, "git-p4.allowSubmit")) {
		if (p4submit_options.allow_submit)
			strbuf_list_free(p4submit_options.allow_submit);
		p4submit_options.allow_submit = strbuf_split_str(v,',',0);
		if (p4submit_options.allow_submit) {
			struct strbuf **strb;
			for (strb = p4submit_options.allow_submit;*strb;strb++) {
				int len = (*strb)->len;
				if (len <= 0)
					continue;
				if ((*strb)->buf[len-1] == ',')
					strbuf_setlen(*strb,len-1);
				strbuf_trim(*strb);
			}
		}
	}
	else if (!strcasecmp(k, "git-p4.conflict")) {
		p4submit_options.conflict_behavior = parse_conflict_mode(v);
		if (p4submit_options.conflict_behavior == CONFLICT_UNKNOWN)
			die("Invalid value \"%s\" for config git-p4.conflict", v);
	}
	else if (!strcasecmp(k, "git-p4.skipusernamecheck"))
		p4submit_options.skip_user_name_check = git_config_bool(k,v);
	else if (!strcasecmp(k, "git-p4.detectrenames"))
		p4submit_options.detect_renames = git_config_bool(k,v);
	else if (!strcasecmp(k, "git-p4.detectcopies"))
		p4submit_options.detect_copies = git_config_bool(k,v);
	else if (!strcasecmp(k, "git-p4.detectcopiesharder"))
		p4submit_options.detect_copies_harder = git_config_bool(k,v);
	return 0;
}


struct dump_file_state
{
	struct strbuf prefix_depot;
	struct strbuf dirname;
};

static void fast_import_blob_fd(int fd_dst, int fd_src)
{
	off_t read_remaining = 0;
	struct strbuf blob_head = STRBUF_INIT;
	struct strbuf read_buf = STRBUF_INIT;
	if (fd_src < 0)
		die("invalid src fd");
	if (fd_dst < 0)
		die("invalid dst fd");
	read_remaining = lseek(fd_src, 0, SEEK_END);
	if (read_remaining < 0)
		die("lseek error");
	if (lseek(fd_src, 0, SEEK_SET) != 0)
		die("error resetting init pos of fd");
	strbuf_addf(&blob_head, "data %jd\n", read_remaining);
	write_str_in_full(fd_dst, blob_head.buf);
	while (read_remaining) {
		ssize_t nread = strbuf_read(&read_buf, fd_src, 0);
		if (nread < 0) {
			switch(errno) {
				case EAGAIN:
				case EINTR:
					continue;
				default:
					die("Error reading from tmp file");
					break;
			}
		}
		else if (nread == 0)
			break;
		if (nread > read_remaining)
			die("File greater than reported, modified outside?");
		read_remaining -= nread;
		write_in_full(fd_dst, read_buf.buf, read_buf.len);
	}
	strbuf_release(&blob_head);
	strbuf_release(&read_buf);
}

static void fast_import_blob_p4filedesc(int fd_out, const struct depot_file_t *p4f_in, const struct strbuf *sb_prefix)
{
	struct child_process child_p4 = CHILD_PROCESS_INIT;
	struct hashmap map;
	iconv_t icd = NULL;
	unsigned mode = 0;
	struct tempfile *temp = NULL;
	str_dict_init(&map);
	if (!starts_with(p4f_in->depot_path_file.buf, sb_prefix->buf))
		goto _leave;
	argv_array_push(&child_p4.args, "print");
	child_p4.out = -1;
	if (p4f_in->is_revision)
		argv_array_pushf(&child_p4.args, "%s#%d", p4f_in->depot_path_file.buf, p4f_in->chg_rev);
	else
		argv_array_pushf(&child_p4.args, "%s@=%d", p4f_in->depot_path_file.buf, p4f_in->chg_rev);
	p4_start_command(&child_p4);
	while (py_marshal_parse(&map, child_p4.out)) {
		const keyval_t *kw = NULL;
		if (IS_LOG_DEBUG_ALLOWED)
			str_dict_print(p4_verbose_debug.fp, &map);
		if (!strcmp(str_dict_get_value(&map, "code"), "stat")) {
			if (temp)
				die("More than one file reported");
			if (icd)
				die("icd not NULL");
			temp = mks_tempfile_t(".p4_blob_XXXXXX");
			if (!temp)
				die ("Failed to create temp file");
			if (p4type2bintype(str_dict_get_value(&map, "type")) == P4_FORMAT_UTF16_TYPE)
				icd = iconv_open("utf16", "utf8");
			mode = p4type2mode(str_dict_get_value(&map, "type"));
			continue;
		}
		else if (strcmp(str_dict_get_value(&map, "code"), "text") &&
				strcmp(str_dict_get_value(&map, "code"), "binary")) {
			continue;
		}
		if (!temp)
			continue;
		kw = str_dict_get_kw(&map, "data");
		if (!kw) {
			die("Unexpected print output format");
		}
		if (!kw->val.len)
			continue;
		if (icd) {
			size_t outsz;
			char *buf_utf16;
			keyval_t *kw_reencoded = keyval_init(NULL);
			buf_utf16 = reencode_string_iconv(kw->val.buf, kw->val.len, icd, 0, &outsz);
			assert(buf_utf16);
			strbuf_addbuf(&kw_reencoded->key, &kw->key);
			strbuf_add(&kw_reencoded->val, buf_utf16, outsz);
			free(buf_utf16);
			str_dict_put_kw(&map, kw_reencoded);
			kw = str_dict_get_kw(&map, "data");
		}
		if (0120000 == mode)
			strbuf_trim_trailing_newline(&kw->val);
		if (write_in_full(temp->fd, kw->val.buf, kw->val.len) != kw->val.len) die("Block not written");
	}
_leave:
	if (temp) {
		struct strbuf filemodify = STRBUF_INIT;
		strbuf_addf(&filemodify, "M %06o inline %s\n", mode,
				p4f_in->depot_path_file.buf + sb_prefix->len);
		write_str_in_full(fd_out, filemodify.buf);
		fast_import_blob_fd(fd_out, temp->fd);
		delete_tempfile(&temp);
		strbuf_release(&filemodify);
	}
	if (icd)
		iconv_close(icd);
	close(child_p4.out);
	finish_command(&child_p4);
	str_dict_destroy(&map);
}

static void depot_file_init(struct depot_file_t *p)
{
	strbuf_init(&p->depot_path_file, 0);
	p->chg_rev = 0;
	p->is_revision = 0;
	p->mode = 0;
	p->bin_type = P4_FORMAT_UNKNOWN_TYPE;
	p->hash = null_md5;
	INIT_LIST_HEAD(&p->lhead);
}

static void depot_file_set(struct depot_file_t *p,
		const char *str,
		unsigned int chg_rev,
		unsigned int is_revision,
		unsigned mode,
		int bin_type,
		struct object_id md5)
{
	strbuf_reset(&p->depot_path_file);
	strbuf_addstr(&p->depot_path_file, str);
	p->chg_rev = chg_rev;
	p->is_revision = is_revision;
	p->bin_type = bin_type;
	p->mode = mode;
}

static void depot_file_copy(struct depot_file_t *dst, struct depot_file_t *src)
{
	strbuf_reset(&dst->depot_path_file);
	strbuf_addbuf(&dst->depot_path_file, &src->depot_path_file);
	dst->chg_rev = src->chg_rev;
	dst->is_revision = src->is_revision;
	dst->mode = src->mode;
	dst->bin_type = src->bin_type;
	dst->hash = src->hash;
}

static void depot_file_printf(FILE *fp, struct depot_file_t *p)
{
	fprintf(fp, "%s", p->depot_path_file.buf);
	if (p->is_revision)
		fprintf(fp, "#");
	else
		fprintf(fp, "@");
	fprintf(fp, "%d",p->chg_rev);
	fprintf(fp, " %s", md5_to_hex(&p->hash));
}

static void depot_file_destroy(struct depot_file_t *p)
{
	strbuf_release(&p->depot_path_file);
}

static void list_depot_files_add(struct list_head *list_depot_files,
		const char *depot_file,
		unsigned int chg_rev,
		int is_revision,
		unsigned mode,
		p4_format_types_t bin_type,
		struct md5_id md5)
{
	struct depot_file_t *df = malloc(sizeof(struct depot_file_t));
	depot_file_init(df);
	strbuf_addstr(&df->depot_path_file, depot_file);
	df->chg_rev = chg_rev;
	df->is_revision = is_revision;
	df->mode = mode;
	df->bin_type = bin_type;
	df->hash = md5;
	list_add_tail(&df->lhead, list_depot_files);
}

static void list_depot_files_destroy(struct list_head *list_depot_files)
{
	struct list_head *pos, *p;
	list_for_each_safe(pos, p, list_depot_files) {
		struct depot_file_t *dp;
		list_del(pos);
		dp = list_entry(pos, struct depot_file_t, lhead);
		depot_file_destroy(dp);
		free(dp);
	}
}

static void list_depot_files_printf(FILE *fp, struct list_head *list_depot_files)
{
	struct list_head *pos;
	list_for_each(pos, list_depot_files) {
		struct depot_file_t *dp;
		dp = list_entry(pos, struct depot_file_t, lhead);
		depot_file_printf(fp, dp);
		fprintf(fp, "\n");
	}
}

static void depot_file_pair_init(struct depot_file_pair_t *dp)
{
	depot_file_init(&dp->a);
	depot_file_init(&dp->b);
}

static void depot_file_pair_destroy(struct depot_file_pair_t *dp)
{
	depot_file_destroy(&dp->a);
	depot_file_destroy(&dp->b);
}

static void list_depot_files_pair_add(struct list_head *list_depot_files_pair, struct depot_file_t *a, struct depot_file_t *b)
{
	struct depot_file_pair_t *df = malloc(sizeof(struct depot_file_pair_t));
	depot_file_pair_init(df);
	depot_file_copy(&df->a, a);
	depot_file_copy(&df->b, b);
	list_add_tail(&df->lhead, list_depot_files_pair);
}

static void list_depot_files_pair_destroy(struct list_head *list_depot_files)
{
	struct list_head *pos, *p;
	list_for_each_safe(pos, p, list_depot_files) {
		struct depot_file_pair_t *dp;
		list_del(pos);
		dp = list_entry(pos, struct depot_file_pair_t, lhead);
		depot_file_pair_destroy(dp);
		free(dp);
	}
}

static void depot_changelist_desc_destroy(struct depot_changelist_desc_t *cl)
{
	strbuf_release(&cl->changelist_or_commit);
	strbuf_release(&cl->desc);
	strbuf_release(&cl->time);
	strbuf_release(&cl->committer);
	strbuf_release(&cl->depot_base);
	list_depot_files_destroy(&cl->list_of_deleted_files);
	list_depot_files_destroy(&cl->list_of_modified_files);
}

static void list_depot_changelist_desc_destroy(struct list_head *list_changes)
{
	struct list_head *pos, *p;
	list_for_each_safe(pos, p, list_changes) {
		struct depot_changelist_desc_t *dp;
		list_del(pos);
		dp = list_entry(pos, struct depot_changelist_desc_t, list);
		depot_changelist_desc_destroy(dp);
		free(dp);
	}
}


struct depot_change_range_t {
	struct strbuf depot_path;
	unsigned int start_changelist;
	unsigned int end_changelist;
};

#define DEPOT_CHANGE_RANGE_INIT { STRBUF_INIT, 0, 0}

static void depot_change_range_destroy(struct depot_change_range_t *ptr)
{
	strbuf_release(&ptr->depot_path);
}

static int p4format_patch_parse(int argc, const char **argv, struct depot_change_range_t *chrng)
{
	const char **argindx = argv;
	char *endptr = NULL;
	if (argc < 1) {
		die ("Failed to parse string, no string passed");
		return -1;
	}
	strtol(*argindx, &endptr, 10);
	if (endptr == *argindx) {
		strbuf_addstr(&chrng->depot_path, *argindx);
		argindx ++;
		argc --;
	}
	else {
		struct strbuf upstream = STRBUF_INIT;
		struct hashmap p4settings;
		const char *depot_path = NULL;
		str_dict_init(&p4settings);
		if (find_upstream_branch_point(0, &upstream, &p4settings) < 0)
			die("Error findind upstream (%s:%d)", __FILE__,__LINE__ );
		depot_path = str_dict_get_value(&p4settings, "depot-paths");
		strbuf_addstr(&chrng->depot_path, depot_path);
		str_dict_destroy(&p4settings);
		strbuf_release(&upstream);
	}
	if (!argc)
		die ("Failed to parse string, changelist expected");
	chrng->start_changelist = atoi(*argindx);
	argindx++;
	argc --;
	if (argc)
		chrng->end_changelist = atoi(*argindx);
	else
		chrng->end_changelist = chrng->start_changelist;
	return 0;
}

static void print_change_range(FILE *fp, struct depot_change_range_t *chrng)
{
	fprintf(fp, "depot: %s\n",chrng->depot_path.buf);
	fprintf(fp, "\tfrom: % 9d to: % 9d\n", chrng->start_changelist, chrng->end_changelist);
}


static int p4revtoi(const char *p4rev)
{
	long int n = 0;
	char *endptr;
	n = strtol(p4rev,  &endptr, 10);
	if (endptr != p4rev)
		return n;
	if (!strcmp(p4rev, "none"))
		return 0;
	die ("Not a valid revision: %s", p4rev);
	return n;
}


static int add_list_files_from_changelist(struct depot_changelist_desc_t *prev,
		struct depot_changelist_desc_t *current,
		const char *depot_path,
		int changelist,
		int change_stamp_fmt)
{
	const char *depotfile_str = "depotFile";
	const int depotfile_len = strlen(depotfile_str);
	struct child_process child_p4 = CHILD_PROCESS_INIT;
	struct hashmap map;
	str_dict_init(&map);
	argv_array_push(&child_p4.args, "describe");
	argv_array_push(&child_p4.args, "-S");
	argv_array_pushf(&child_p4.args, "%d", changelist);
	child_p4.out = -1;
	p4_start_command(&child_p4);
	while (py_marshal_parse(&map, child_p4.out))
	{
		struct hashmap_iter hm_iter;
		const struct hashmap_entry *entry;
		int is_shelved;
		const char *p4user;
		assert(str_dict_strcmp(&map, "code", NULL));
		if (!str_dict_strcmp(&map, "code", "error"))
			die("Error geting description for change %d", changelist);
		if (!str_dict_strcmp(&map, "code", "info"))
			continue;
		//changelist = atoi(str_dict_get_value(&map, "change"));
		is_shelved = str_dict_get_value(&map, "shelved")!=NULL;
		p4user = p4usermap_cache_get_name_email_str_by_user(str_dict_get_kw(&map, "user")->val.buf);
		strbuf_addbuf(&current->desc, &str_dict_get_kw(&map, "desc")->val);
		switch (change_stamp_fmt) {
			case GITP4_CHANGE_STAMP_NONE:
				break;
			case GITP4_CHANGE_STAMP_REF:
				strbuf_addf(&current->desc, "\n");
				strbuf_add_gitp4_cl_desc(&current->desc, current);
				strbuf_addf(&current->desc, "\n");
				break;
			case GITP4_CHANGE_STAMP_CHERRY_PICK:
				strbuf_addf(&current->desc,
						"\n[git-p4-cherry-pick: %s...@=%d]", depot_path, changelist);
				break;
		}
		strbuf_addbuf(&current->changelist_or_commit, &str_dict_get_kw(&map, "change")->val);
		strbuf_addstr(&current->committer, p4user);
		strbuf_addbuf(&current->time, &str_dict_get_kw(&map, "time")->val);
		if (prev) {
			strbuf_addbuf(&prev->desc, &str_dict_get_kw(&map, "desc")->val);
			strbuf_addf(&prev->desc,
					"\n[git-p4-cherry-pick: %s...@=%d~]", depot_path, changelist);
			strbuf_addbuf(&prev->changelist_or_commit, &str_dict_get_kw(&map, "change")->val);
			strbuf_addstr(&prev->committer, p4user);
			strbuf_addbuf(&prev->time, &str_dict_get_kw(&map, "time")->val);
		}
		hashmap_iter_init(&map, &hm_iter);
		while ((entry = hashmap_iter_next(&hm_iter))) {
			const keyval_t *kw = container_of(entry, const keyval_t, ent);
			if (starts_with(kw->key.buf, depotfile_str)) {
				const char *dp_suffix = kw->key.buf + depotfile_len;
				unsigned int rev = 0;
				const char *action = str_dict_get_valuef(&map, "action%s", dp_suffix);
				unsigned mode;
				p4_format_types_t p4_bin_type;
				struct md5_id md5;
				rev = p4revtoi(str_dict_get_valuef(&map, "rev%s", dp_suffix));
				mode = p4type2mode(str_dict_get_valuef(&map, "type%s", dp_suffix));
				p4_bin_type = p4type2bintype(str_dict_get_valuef(&map, "type%s", dp_suffix));
				const char *digest_md5 = str_dict_get_valuef(&map, "digest%s", dp_suffix);
				if (digest_md5)
					get_md5_hex(digest_md5, &md5);
				else
					md5 = null_md5;
				if (IS_LOG_DEBUG_ALLOWED) {
					fprintf(p4_verbose_debug.fp, "%s#%d (%06o) %s\n",
							kw->val.buf,
							rev,
							mode,
							md5_to_hex(&md5)
							);
				}
				if (is_shelved) {
					if (strstr(action, "delete") == NULL)
						list_depot_files_add(&current->list_of_modified_files, kw->val.buf, changelist, 0, mode, p4_bin_type, md5);
					else
						list_depot_files_add(&current->list_of_deleted_files, kw->val.buf, 0, 1, mode, p4_bin_type, md5);
				}
				else {
					if (strstr(action, "delete") == NULL)
						list_depot_files_add(&current->list_of_modified_files, kw->val.buf, rev, 1, mode, P4_FORMAT_UNKNOWN_TYPE, md5);
					else
						list_depot_files_add(&current->list_of_deleted_files, kw->val.buf, 0, 1, mode, p4_bin_type, md5);
					if (rev)
						rev --; //Previous revision
				}
				if (strstr(action, "add") == NULL &&
						strstr(action, "delete") == NULL &&
						strcmp(action, "branch") &&
						strcmp(action, "integrate") &&
						strcmp(action, "edit")) {
					die("Action %s not supported", action);
				}
				else if (prev &&
						strstr(action, "add") == NULL &&
						strcmp(action, "branch") &&
						rev != 0) {
					list_depot_files_add(&prev->list_of_modified_files,
							kw->val.buf, rev, 1, mode, P4_FORMAT_UNKNOWN_TYPE, null_md5);
				}
			}
		}
	}
	close(child_p4.out);
	str_dict_destroy(&map);
	return finish_command(&child_p4);
}

static void add_list_files_from_changelist_range(struct list_head *list_changes, struct depot_change_range_t *chrng)
{
	struct depot_changelist_desc_t *prev, *current;
	prev = malloc(sizeof(struct depot_changelist_desc_t));
	current = malloc(sizeof(struct depot_changelist_desc_t));
	INIT_DEPOT_CHANGELIST_DESC(prev);
	INIT_DEPOT_CHANGELIST_DESC(current);
	strbuf_addbuf(&prev->depot_base, &chrng->depot_path);
	strbuf_addbuf(&current->depot_base, &chrng->depot_path);
	add_list_files_from_changelist(prev,
			current,
			chrng->depot_path.buf,
			chrng->start_changelist,
			GITP4_CHANGE_STAMP_CHERRY_PICK);
	list_add(&current->list, list_changes);
	list_add(&prev->list, list_changes);
}

static int strbuf_fast_import_commit_header(int fd_out, struct depot_changelist_desc_t *pcl, const char *ref, int mark_id)
{
	struct strbuf sb_commit = STRBUF_INIT;
	struct strbuf sb_delim = STRBUF_INIT;
	strbuf_addf(&sb_delim, "__COMMIT_DELIM_%s_%s_%d",
			pcl->changelist_or_commit.buf,
			pcl->time.buf,
			mark_id
			);
	strbuf_addf(&sb_commit, "commit %s\n", ref);
	strbuf_addf(&sb_commit, "mark :%d\n", mark_id);
	strbuf_addf(&sb_commit, "committer %s %s +0000\n",
			pcl->committer.buf,
			pcl->time.buf);
	strbuf_addf(&sb_commit, "data <<%s\n", sb_delim.buf);
	strbuf_addbuf(&sb_commit, &pcl->desc);
	strbuf_addf(&sb_commit, "\n%s\n", sb_delim.buf);
	write_str_in_full(fd_out, sb_commit.buf);
	strbuf_release(&sb_delim);
	strbuf_release(&sb_commit);
	return mark_id + 1;
}


static void p4export_apply_file_changes(int fd_out, struct depot_changelist_desc_t *pchange)
{
	const struct strbuf *dp_sb = &pchange->depot_base;
	struct list_head *pos;
	list_for_each(pos, &pchange->list_of_deleted_files) {
		struct depot_file_t *df;
		struct strbuf sb_del = STRBUF_INIT;
		df = list_entry(pos, struct depot_file_t, lhead);
		if (!starts_with(df->depot_path_file.buf, dp_sb->buf))
			continue;
		const char *dp_suffix = df->depot_path_file.buf + dp_sb->len;
		strbuf_addf(&sb_del, "D %s\n", dp_suffix);
		write_str_in_full(fd_out, sb_del.buf);
		strbuf_release(&sb_del);
	}
	list_for_each(pos, &pchange->list_of_modified_files) {
		struct depot_file_t *df;
		df = list_entry(pos, struct depot_file_t, lhead);
		if (!starts_with(df->depot_path_file.buf, dp_sb->buf))
			continue;
		struct depot_file_t df_out;
		depot_file_init(&df_out);
		fast_import_blob_p4filedesc(fd_out, df, dp_sb);
		depot_file_destroy(&df_out);
	}
}

static int p4export_list_changes(int fd_out, struct list_head *plist, const char *ref)
{
	struct list_head *pos;
	int mark_id = 1;
	struct depot_changelist_desc_t prev_commit;
	INIT_DEPOT_CHANGELIST_DESC(&prev_commit);
	prev_commit.change_source = CHANGE_SRC_GIT;
	strbuf_addstr(&prev_commit.changelist_or_commit, oid_to_hex(&null_oid));
	list_for_each(pos, plist) {
		struct depot_changelist_desc_t *this_change = list_entry(pos, struct depot_changelist_desc_t, list);
		if (CHANGE_SRC_P4 == this_change->change_source) {
			int this_change_mark_id = mark_id;
			mark_id = strbuf_fast_import_commit_header(fd_out, this_change, ref, mark_id);
			write_str_in_full(fd_out, "from ");
			write_str_in_full(fd_out, prev_commit.changelist_or_commit.buf);
			write_str_in_full(fd_out, "\n");
			p4export_apply_file_changes(fd_out, this_change);
			strbuf_setlen(&prev_commit.changelist_or_commit, 0);
			strbuf_addf(&prev_commit.changelist_or_commit, ":%d", this_change_mark_id);
		}
		else if (CHANGE_SRC_GIT == this_change->change_source){
			strbuf_setlen(&prev_commit.changelist_or_commit, 0);
			strbuf_addbuf(&prev_commit.changelist_or_commit, &this_change->changelist_or_commit);
		}
		else
			die("Only p4 changes for now\n");
	}
	depot_changelist_desc_destroy(&prev_commit);
	return mark_id;
}

static void p4export_change(int fd_out, struct depot_change_range_t *chg_range)
{
	LIST_HEAD(list_of_changes);
	char tmp_ref[] = "refs/temp/p4/XXXXXX";
	struct strbuf export_done = STRBUF_INIT;
	int next_mark_id = -1;
	add_list_files_from_changelist_range(&list_of_changes, chg_range);
	next_mark_id = p4export_list_changes(fd_out, &list_of_changes, tmp_ref);
	strbuf_addf(&export_done, "\nreset %s\n", tmp_ref);
	strbuf_addf(&export_done, "get-mark :%d\n", next_mark_id - 1);
	strbuf_addf(&export_done, "done\n");
	write_str_in_full(fd_out, export_done.buf);
	strbuf_release(&export_done);
	list_depot_changelist_desc_destroy(&list_of_changes);

}

static int p4export_change_commit(struct strbuf *p_sb_sha1, struct depot_change_range_t *p_chg_rng)
{
	struct child_process git_fast_import = CHILD_PROCESS_INIT;
	FILE *fp = NULL;
	argv_array_push(&git_fast_import.args, "git");
	argv_array_push(&git_fast_import.args, "fast-import");
	git_fast_import.in = -1;
	git_fast_import.out = -1;
	if (start_command(&git_fast_import)) {
		die("cannot start git fast-import");
	}
	if (NULL == (fp = fdopen(git_fast_import.out, "r")))
		die("cannot open fast-import out");
	p4export_change(git_fast_import.in, p_chg_rng);
	if (EOF == strbuf_getline(p_sb_sha1, fp)) {
		die("Failed to retrieve commit");
	}
	fclose(fp);
	return finish_command(&git_fast_import);
}


int p4format_patch_cmd_run(struct command_t *pcmd, int argc, const char **argv)
{
	struct option options[] = {
		OPT_END()
	};
	struct depot_change_range_t chg_range = DEPOT_CHANGE_RANGE_INIT;
	struct strbuf commit_sha1 = STRBUF_INIT;
	struct child_process git_format_patch = CHILD_PROCESS_INIT;
	argc = parse_options(argc, argv, NULL, options, NULL, 0);
	if (p4format_patch_parse(argc, argv, &chg_range) != 0) {
		die("Error parsing changelists");
	}
	if (IS_LOG_DEBUG_ALLOWED)
		print_change_range(p4_verbose_debug.fp, &chg_range);
	if (p4export_change_commit(&commit_sha1, &chg_range))
		die("p4 export failed");
	argv_array_push(&git_format_patch.args, "git");
	argv_array_push(&git_format_patch.args, "format-patch");
	argv_array_pushf(&git_format_patch.args, "%s~1..%s", commit_sha1.buf, commit_sha1.buf);
	if (start_command(&git_format_patch))
		die("cannot start git format-patch");
	depot_change_range_destroy(&chg_range);
	return finish_command(&git_format_patch);
}

void p4submit_cmd_init(struct command_t *pcmd)
{
	strbuf_init(&pcmd->strb_usage,0);
	pcmd->needs_git = 0;
	pcmd->verbose = 0;
	pcmd->run_fn = p4submit_cmd_run;
	pcmd->deinit_fn = p4submit_cmd_deinit;
	pcmd->data = NULL;
	memset(&p4submit_options, 0, sizeof(p4submit_options));
	strbuf_init(&p4submit_options.base_commit, 0);
	strbuf_init(&p4submit_options.branch, 0);
	strbuf_init(&p4submit_options.depot_path, 0);
	strbuf_init(&p4submit_options.client_path, 0);
	strbuf_init(&p4submit_options.diff_opts, 0);
	strbuf_init(&p4submit_options.cl_suffix, 0);
	git_config(p4submit_git_config,NULL);
}

void p4shelve_cmd_init(struct command_t *pcmd)
{
	p4submit_cmd_init(pcmd);
	p4submit_options.shelve = 1;
}

/* This functions expects a dictionary with at least these file 
 * branch:                (refs/heads/... or refs/remotes/p4/...)
 * msg:                   (with the message of the commit)
 * committer:              (John Smith <john.smith@gmail.com> )
 * author(optional):      (same format as committer)
 * base_commit(optional): (If not specified, it doesn't have any parent)
 * time:                  (unix epochs)
 */
int git_commit(struct hashmap *map)
{
	struct child_process git_fast_import = CHILD_PROCESS_INIT;
	const char *msg = str_dict_get_value(map, "msg");
	argv_array_push(&git_fast_import.args, "git");
	argv_array_push(&git_fast_import.args, "fast-import");
	git_fast_import.in = -1;
	if (!str_dict_get_value(map, "branch"))
		die("No branch provided");
	if (!str_dict_get_value(map, "msg"))
		die("No msg provided");
	if (!str_dict_get_value(map, "committer"))
		die("No committer provided");
	if (!str_dict_get_value(map, "time"))
		die("No time provided");
	if (start_command(&git_fast_import)) {
		die("cannot start git fast-import");
	}
	FILE *fp = fdopen(git_fast_import.in, "w");
	fprintf(fp, "commit %s\n", str_dict_get_value(map, "branch"));
	fprintf(fp, "committer %s %d +0000\n",
			str_dict_get_value(map, "committer"),
			atoi(str_dict_get_value(map, "time")));
	fprintf(fp, "data %"PRIuMAX"\n", strlen(msg));
	fprintf(fp, "%s\n", msg);
	if (str_dict_get_value(map, "base_commit"))
		fprintf(fp, "from %s\n", str_dict_get_value(map, "base_commit"));
	fprintf(fp, "\n");
	fclose(fp);
	return finish_command(&git_fast_import);
}

int git_update_ref(const char *new_ref, const char *commit)
{
	struct child_process git_upd_ref = CHILD_PROCESS_INIT;
	argv_array_push(&git_upd_ref.args, "git");
	argv_array_push(&git_upd_ref.args, "update-ref");
	argv_array_push(&git_upd_ref.args, new_ref);
	argv_array_push(&git_upd_ref.args, commit);
	if (start_command(&git_upd_ref)) {
		die("cannot start git update-ref");
	}
	return finish_command(&git_upd_ref);
}

static int p4_check_identical_branches(struct depot_file_pair_t *depot_pair)
{
	struct depot_file_t *a = &depot_pair->a;
	struct depot_file_t *b = &depot_pair->b;
	struct child_process child_p4 = CHILD_PROCESS_INIT;
	struct hashmap map;
	int are_identical = 1;
	if (!a->depot_path_file.len)
		return 0;
	if (!a->chg_rev)
		return 0;
	if (!b->depot_path_file.len)
		return 0;
	if (!b->chg_rev)
		return 0;
	child_p4.out = -1;
	str_dict_init(&map);
	argv_array_push(&child_p4.args, "diff2");
	argv_array_pushf(&child_p4.args, "%s...@%u", a->depot_path_file.buf, a->chg_rev);
	argv_array_pushf(&child_p4.args, "%s...@%u", b->depot_path_file.buf, b->chg_rev);
	p4_start_command(&child_p4);
	while (py_marshal_parse(&map, child_p4.out)) {
		if (!str_dict_strcmp(&map, "code", "info"))
			continue;
		else if ((!str_dict_strcmp(&map, "code", "stat"))
					&& (!str_dict_strcmp(&map, "status", "identical"))) {
			continue;
		}
		if (IS_LOG_DEBUG_ALLOWED) {
			LOG_GITP4_DEBUG("Branches not identical\n");
			str_dict_print(p4_verbose_debug.fp, &map);
		}
		are_identical = 0;
		break;
	}
	close(child_p4.out);
	finish_command(&child_p4);
	str_dict_destroy(&map);
	return are_identical;
}

static void p4discover_branches_find_p4_parent(struct depot_file_pair_t *depot_pair,
		struct strbuf *sub_file_name)
{
	const char *p = NULL;
	struct child_process child_p4 = CHILD_PROCESS_INIT;
	struct hashmap map;
	unsigned int count_changes = 0;
	unsigned int base_candidate = 0;
	child_p4.out = -1;
	str_dict_init(&map);
	argv_array_push(&child_p4.args, "changes");
	argv_array_push(&child_p4.args, "-m2");
	argv_array_push(&child_p4.args, "-i");
	argv_array_pushf(&child_p4.args, "%s...@%d",depot_pair->a.depot_path_file.buf, depot_pair->a.chg_rev);
	p4_start_command(&child_p4);
	while (py_marshal_parse(&map, child_p4.out)) {
		count_changes ++;
		if ((count_changes == 1) &&
				(depot_pair->a.chg_rev != atoi(str_dict_get_value(&map, "change"))))
			break;
		else if (count_changes == 2) {
			base_candidate = atoi(str_dict_get_value(&map, "change"));
		}
	}
	close(child_p4.out);
	finish_command(&child_p4);
	get_p4describe(&map, base_candidate);
	if (IS_LOG_DEBUG_ALLOWED) {
		LOG_GITP4_DEBUG("p4 describe %u\n", base_candidate);
		str_dict_print(p4_verbose_debug.fp, &map);
	}
	for (count_changes = 0, p = str_dict_get_value(&map, "depotFile0"); p;
			p = str_dict_get_valuef(&map, "depotFile%u", ++count_changes)) {
		LOG_GITP4_DEBUG("depotFile:%s (%u)\n", p, count_changes);
		if (starts_with(p, depot_pair->b.depot_path_file.buf)) {
			depot_pair->b.chg_rev = base_candidate;
			depot_pair->b.is_revision = 0;
			break;
		}
	}
	str_dict_destroy(&map);
}

static int p4create_new_p4_branch(struct strbuf *lbranch, struct depot_file_pair_t *dp)
{
	struct strbuf base_sha = STRBUF_INIT;
	struct strbuf user = STRBUF_INIT;
	struct strbuf gitp4_line = STRBUF_INIT;
	struct hashmap m_describe;
	int res = 0;
	str_dict_init(&m_describe);
	find_p4_depot_commit(&base_sha, &dp->b);
	if (!base_sha.len) {
		LOG_GITP4_INFO("No associate commit found\n");
		goto _leave;
	}
	get_p4describe(&m_describe, dp->a.chg_rev);
	if (!str_dict_get_value(&m_describe, "desc")) {
		LOG_GITP4_INFO("No desc found\n");
		goto _leave;
	}
	if (!str_dict_get_value(&m_describe, "user")) {
		LOG_GITP4_INFO("No user found\n");
		goto _leave;
	}
	if (!str_dict_get_value(&m_describe, "user")) {
		LOG_GITP4_INFO("No user found\n");
		goto _leave;
	}
	if (!str_dict_get_value(&m_describe, "time")) {
		LOG_GITP4_INFO("No time found\n");
		goto _leave;
	}
	LOG_GITP4_DEBUG("sha: %s\n", base_sha.buf);
	LOG_GITP4_DEBUG("desc: %s\n", str_dict_get_value(&m_describe, "desc"));
	LOG_GITP4_DEBUG("user: %s\n", str_dict_get_value(&m_describe, "user"));
	LOG_GITP4_DEBUG("time: %s\n", str_dict_get_value(&m_describe, "time"));
	strbuf_addstr(&user, p4usermap_cache_get_name_email_str_by_user(str_dict_get_value(&m_describe, "user")));
	LOG_GITP4_DEBUG("user full address %s\n", user.buf);
	str_dict_set_key_val(&m_describe, "branch", lbranch->buf);
	strbuf_add_gitp4_df(&gitp4_line, &dp->a);
	str_dict_set_key_valf(&m_describe, "msg", "%s\n%s\n",
			str_dict_get_value(&m_describe, "desc"), gitp4_line.buf);
	str_dict_set_key_val(&m_describe, "committer", user.buf);
	str_dict_set_key_val(&m_describe, "base_commit", base_sha.buf);
	res = git_commit(&m_describe);
_leave:
	str_dict_destroy(&m_describe);
	strbuf_release(&gitp4_line);
	strbuf_release(&user);
	strbuf_release(&base_sha);
	return res;
}

static int p4discover_branches_find_branches(struct list_head *new_branches, const char *str_pattern, const char *local_branch_pattern)
{
	const char *p = NULL;
	const char *ellipsis = "/.../";
	struct strbuf sub_file_name = STRBUF_INIT;
	struct strbuf common_depot_base_name = STRBUF_INIT;
	struct child_process child_p4 = CHILD_PROCESS_INIT;
	struct hashmap map;
	struct list_head *pos;
	child_p4.out = -1;
	str_dict_init(&map);

	strbuf_addstr(&sub_file_name, str_pattern);
	strbuf_addstr(&common_depot_base_name, str_pattern);
	if ((p = memchr(sub_file_name.buf, '@', sub_file_name.len)))
		strbuf_setlen(&sub_file_name, p-sub_file_name.buf);
	if ((p = memchr(sub_file_name.buf, '#', sub_file_name.len)))
		strbuf_setlen(&sub_file_name, p-sub_file_name.buf);
	if ((p = memmem(sub_file_name.buf, sub_file_name.len, ellipsis, strlen(ellipsis))))
		strbuf_splice(&sub_file_name, 0, p - sub_file_name.buf + strlen(ellipsis),
				"", 0);
	else
		goto _leave;

	if ((p = memmem(common_depot_base_name.buf, common_depot_base_name.len,
				   	ellipsis, strlen(ellipsis))))
	{
		strbuf_setlen(&common_depot_base_name, p-common_depot_base_name.buf);
		strbuf_addch(&common_depot_base_name, '/');
	}

	argv_array_push(&child_p4.args, "filelog");
	argv_array_push(&child_p4.args, str_pattern);
	p4_start_command(&child_p4);
	while (py_marshal_parse(&map, child_p4.out)) {
			struct depot_file_t branch_depot_path = DEPOT_FILE_INIT;
			struct depot_file_t branch_base_depot_path = DEPOT_FILE_INIT;
			int indx = 0;
			const char *branch_from = NULL;
			/* str_dict_print(stdout, &map); */
			if (str_dict_strcmp(&map, "action0", "branch"))
				continue;
			if (str_dict_strcmp(&map, "rev0", "1"))
				continue;
			for (;(branch_from = str_dict_get_valuef(&map, "file0,%d", indx)); ++indx) {
				if (strcmp(str_dict_get_valuef(&map, "how0,%d", indx), "branch from"))
					branch_from = NULL;
				else
					break;
			}
			if (!branch_from)
				continue;
			assert(str_dict_get_value(&map, "depotFile"));
			assert(str_dict_get_value(&map, "change0"));
			depot_file_set(&branch_depot_path,
					str_dict_get_value(&map, "depotFile"),
					atoi(str_dict_get_value(&map, "change0")), 0, 040000, P4_FORMAT_UNKNOWN_TYPE, null_oid);
			depot_file_set(&branch_base_depot_path,
					branch_from,
					0, 0, 040000, P4_FORMAT_UNKNOWN_TYPE, null_oid);
			strbuf_strip_suffix(&branch_depot_path.depot_path_file, sub_file_name.buf);
			strbuf_strip_suffix(&branch_base_depot_path.depot_path_file, sub_file_name.buf);
			LOG_GITP4_DEBUG("After stripping: %s (%s)\n",
					branch_depot_path.depot_path_file.buf, sub_file_name.buf);
			list_depot_files_pair_add(new_branches, &branch_depot_path, &branch_base_depot_path);
			depot_file_destroy(&branch_depot_path);
			depot_file_destroy(&branch_base_depot_path);
	}
	close(child_p4.out);
	finish_command(&child_p4);
	list_for_each(pos, new_branches) {
		struct depot_file_pair_t *dp;
		struct strbuf sb_ellipsis_match = STRBUF_INIT;
		struct strbuf local_branch_name = STRBUF_INIT;
		struct strbuf p4_remote_branch_name = STRBUF_INIT;
		dp = list_entry(pos, struct depot_file_pair_t, lhead);
		strbuf_addbuf(&sb_ellipsis_match, &dp->a.depot_path_file);
		strbuf_strip_suffix(&sb_ellipsis_match, "/");
		LOG_GITP4_DEBUG("Local Branch found: %s (prefix to be ignored: %s\n",
				sb_ellipsis_match.buf, common_depot_base_name.buf);

		if (starts_with(sb_ellipsis_match.buf, common_depot_base_name.buf))
			strbuf_remove(&sb_ellipsis_match, 0, common_depot_base_name.len);

		if (local_branch_pattern)
			strbuf_addf(&local_branch_name, "%s", local_branch_pattern);
		else
			strbuf_addf(&local_branch_name, "...");

		if ((p = memmem(local_branch_name.buf, local_branch_name.len, "...", 3)))
			strbuf_splice(&local_branch_name, p - local_branch_name.buf, 3,
					sb_ellipsis_match.buf, sb_ellipsis_match.len);

		_strbuf_ch_translate(&p4_remote_branch_name, &local_branch_name, '/', '_');
		_strbuf_insert_str(&local_branch_name, "refs/heads/");
		_strbuf_insert_str(&p4_remote_branch_name, "refs/remotes/p4/");
		LOG_GITP4_DEBUG("Local branch: %s p4 remote: %s\n",
				local_branch_name.buf, p4_remote_branch_name.buf);
		if (!branch_exists(local_branch_name.buf) &&
				!branch_exists(p4_remote_branch_name.buf)) {
			p4discover_branches_find_p4_parent(dp, &sub_file_name);
			LOG_GITP4_DEBUG("diff2 %s...@%u %s...@%u",
					dp->a.depot_path_file.buf, dp->a.chg_rev,
					dp->b.depot_path_file.buf, dp->b.chg_rev);
			if (!p4_check_identical_branches(dp))
				LOG_GITP4_DEBUG(" (Not identical branches, skipped)\n");
			else {
				LOG_GITP4_DEBUG(" (Identical branches)\n");
				LOG_GITP4_DEBUG("git branch will be created\n");
				p4create_new_p4_branch(&local_branch_name, dp);
				git_update_ref(p4_remote_branch_name.buf, local_branch_name.buf);
			}
		}
		else {
			LOG_GITP4_DEBUG("Local branch for %s already exists\n",
					local_branch_name.buf);
		}
		strbuf_release(&sb_ellipsis_match);
		strbuf_release(&local_branch_name);
		strbuf_release(&p4_remote_branch_name);
	}
_leave:
	str_dict_destroy(&map);
	strbuf_release(&sub_file_name);
	strbuf_release(&common_depot_base_name);
	return 0;
}

static int p4discover_branches_cmd_run(command_t *pcmd, int gargc, const char **gargv)
{
	struct list_head list_branch_depots = LIST_HEAD_INIT(list_branch_depots);
	struct list_head *pos;
	const char *local_branch_pattern = NULL;
	if (gargc <= 1)
		goto _leave;
	if (gargc > 2)
		local_branch_pattern = gargv[2];

	p4discover_branches_find_branches(&list_branch_depots, gargv[1], local_branch_pattern);
	list_for_each(pos, &list_branch_depots) {
		struct depot_file_pair_t *dp;
		dp = list_entry(pos, struct depot_file_pair_t, lhead);
		if (IS_LOG_DEBUG_ALLOWED) {
			fprintf(p4_verbose_debug.fp, "Branch ");
			depot_file_printf(p4_verbose_debug.fp, &dp->a);
			fprintf(p4_verbose_debug.fp, " Parent ");
			depot_file_printf(p4_verbose_debug.fp, &dp->b);
			fprintf(p4_verbose_debug.fp, "\n");
		}
	}
_leave:
	list_depot_files_pair_destroy(&list_branch_depots);
	return 0;
}

void p4discover_branches_cmd_init(struct command_t *pcmd)
{
	strbuf_init(&pcmd->strb_usage, 256);
	strbuf_addf(&pcmd->strb_usage, "this command will try to find new branches and its corresponding parent commit");
	strbuf_addf(&pcmd->strb_usage, "Usage: git-p4 discover-branch p4-pattern");
	pcmd->needs_git = 0;
	pcmd->verbose = 0;
	pcmd->run_fn = p4discover_branches_cmd_run;
	pcmd->deinit_fn = p4_cmd_default_deinit;
	pcmd->data = 0;
}


static int p4fetch_fast_import(struct list_head *l, const char *ref)
{
	struct child_process git_fast_import = CHILD_PROCESS_INIT;
	argv_array_push(&git_fast_import.args, "git");
	argv_array_push(&git_fast_import.args, "fast-import");
	git_fast_import.in = -1;
	if (start_command(&git_fast_import)) {
		die("cannot start git fast-import");
	}
	p4export_list_changes(git_fast_import.in, l, ref);
	close(git_fast_import.in);
	return finish_command(&git_fast_import);
}

int p4_fetch_refs(const char *ref_prefix)
{
	struct hashmap map;
	struct hashmap_iter hm_iter;
	struct hashmap_entry *entry;
	int res = 0;
	str_dict_init(&map);
	p4_refs_in_git(ref_prefix, &map);
	if (IS_LOG_DEBUG_ALLOWED) {
		LOG_GITP4_DEBUG("p4 references:\n");
		str_dict_print(p4_verbose_debug.fp, &map);
	}
	hashmap_iter_init(&map, &hm_iter);
	while ((entry = hashmap_iter_next(&hm_iter))) {
		struct hashmap settings_map;
		struct strbuf sb = STRBUF_INIT;
		const char *depot_path = NULL;
		int changelist;
		const keyval_t *kw = container_of(entry, const keyval_t, ent);
		struct child_process child_p4 = CHILD_PROCESS_INIT;
		struct hashmap p4_change;
		struct depot_changelist_desc_t *change_elem = NULL;
		LIST_HEAD(list_of_changes);
		str_dict_init(&settings_map);
		str_dict_init(&p4_change);
		change_elem = malloc(sizeof(struct depot_changelist_desc_t));
		INIT_DEPOT_CHANGELIST_DESC(change_elem);
		change_elem->change_source = CHANGE_SRC_GIT;
		strbuf_addbuf(&change_elem->changelist_or_commit, &kw->val);
		list_add_tail(&change_elem->list, &list_of_changes);
		extract_log_message(kw->val.buf, &sb);
		extract_p4_settings_git_log(&settings_map, sb.buf);
		depot_path = str_dict_get_value(&settings_map, "depot-paths");
		changelist = atoi(str_dict_get_value(&settings_map, "change"));
		if (IS_LOG_DEBUG_ALLOWED) {
			fprintf(p4_verbose_debug.fp, "%s...@%d\n", depot_path, changelist);
		}
		child_p4.out = -1;
		argv_array_push(&child_p4.args, "changes");
		argv_array_push(&child_p4.args, "-r");
		argv_array_pushf(&child_p4.args, "%s...@%d,#head", depot_path, changelist + 1);
		p4_start_command(&child_p4);
		while (py_marshal_parse(&p4_change, child_p4.out)) {
			if (!str_dict_has(&p4_change, "code"))
				continue;
			if (str_dict_strcmp(&p4_change, "code", "stat"))
				continue;
			if (!str_dict_has(&p4_change, "change"))
				continue;
			if (IS_LOG_DEBUG_ALLOWED) {
				fprintf(p4_verbose_debug.fp, "fetching %s...@=%s\n", depot_path,
						str_dict_get_value(&p4_change, "change"));
			}
			change_elem = malloc(sizeof(struct depot_changelist_desc_t));
			INIT_DEPOT_CHANGELIST_DESC(change_elem);
			change_elem->change_source = CHANGE_SRC_P4;
			strbuf_addstr(&change_elem->depot_base, depot_path);
			strbuf_addstr(&change_elem->changelist_or_commit,
						str_dict_get_value(&p4_change, "change"));
			add_list_files_from_changelist(NULL, change_elem,
					depot_path,
					atoi(change_elem->changelist_or_commit.buf),
					GITP4_CHANGE_STAMP_REF);
			list_add_tail(&change_elem->list, &list_of_changes);
		}
		finish_command(&child_p4);
		res = p4fetch_fast_import(&list_of_changes, kw->key.buf);
		list_depot_changelist_desc_destroy(&list_of_changes);
		str_dict_destroy(&p4_change);
		str_dict_destroy(&settings_map);
		strbuf_release(&sb);
		if (res)
			break;
	}
	str_dict_destroy(&map);
	return res;
}

static int p4fetch_cmd_run(command_t *pcmd, int argc, const char **argv)
{
#if 0
	struct option options[] = {
		OPT_END()
	};
#endif
	return p4_fetch_refs("refs/remotes/p4/");
}


/**
 * Read the contents of a given file descriptor after discarding
 * any previous data in the strbuf
 */
static ssize_t strbuf_overwrite_read(struct strbuf *sb, FILE *fp, size_t sz)
{
	strbuf_release(sb);
	return strbuf_fread(sb, sz, fp);
}

static void reencode_strbuf_iconv(struct strbuf *out, const struct strbuf *in, iconv_t conv)
{
	char *str_out;
	size_t outsz;
	strbuf_reset(out);
	str_out = reencode_string_iconv(in->buf, in->len, conv, 0, &outsz);
	if (!str_out)
		die("OOM in reencode_string_iconv");
	strbuf_add(out, str_out, outsz);
	free(str_out);
}

const char *p4_type_to_str(p4_format_types_t p4_type)
{
	switch(p4_type) {
		case P4_FORMAT_TEXT_TYPE:
			return "text";
		case P4_FORMAT_BIN_TYPE:
			return "bin";
		case P4_FORMAT_UTF8_TYPE:
			return "utf-8";
		case P4_FORMAT_UTF16_TYPE:
			return "utf-16";
		case P4_FORMAT_LINK_TYPE:
			return "link";
		default:
			break;
	}
	return "Unknown";
}



struct git_file_info {
	p4_format_types_t p4_type;
	uintmax_t size;
	struct md5_id md5;
};

struct git_p4_file_stats{
	uintmax_t n_files[P4_FORMAT_MAX_TYPE];
	uint64_t sz_per_type[P4_FORMAT_MAX_TYPE];
};

static void git_p4_file_stats_init(struct git_p4_file_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
}

static void git_p4_file_stats_add_info(struct git_p4_file_stats *stats,
		struct git_file_info *f_info)
{
	const p4_format_types_t p4_fmt_type = f_info->p4_type;
	const uintmax_t sz = f_info->size;
	if (f_info->p4_type >= P4_FORMAT_MAX_TYPE)
		return;
	stats->n_files[p4_fmt_type]++;
	stats->sz_per_type[p4_fmt_type] += sz;
}

static void fprint_gitp4_stats(FILE *fp, struct git_p4_file_stats *stats)
{
	int i;
	for (i = P4_FORMAT_UNKNOWN_TYPE; i < P4_FORMAT_MAX_TYPE; i++) {
		fprintf(fp, "total size: %10" PRIu64 "\tnumber_of_files: %8" PRIuMAX "\t%s\n",
				stats->sz_per_type[i], stats->n_files[i],
				p4_type_to_str(i));
	}
}

static int compute_md5_from_git(FILE *fp, p4_format_types_t p4_file_type, struct git_file_info *finfo)
{
	md5_ctx_t md5_ctx;
	struct strbuf sb = STRBUF_INIT;
	struct strbuf sb_reenc = STRBUF_INIT;
	struct strbuf sb_line = STRBUF_INIT;
	uintmax_t sz_remaining = 0;
	const char *skip_bom = NULL;
	int res = 0;
	iconv_t icd = NULL;
	md5_init(&md5_ctx);
	strbuf_getwholeline(&sb_line, fp, '\n');
	strbuf_trim(&sb_line);
	struct strbuf **report_first = strbuf_split(&sb_line, ' ');
	struct strbuf **report_end = report_first;
	while(*report_end)
		report_end ++;
	if (!report_first)
		die("No stats line from cat-file");
	if ((report_end - report_first) < 3) {
		res = -1;
		goto _err;
	}
	if (strcmp((*(report_first + 1))->buf, "blob ") != 0) {
		res = -1;
		goto _err;
	}
	else {
		struct strbuf *p_sb_size = *(report_first + 2);
		char *end = p_sb_size->buf + p_sb_size->len;
		sz_remaining = strtoumax(p_sb_size->buf, &end, 10);
	}
	finfo->p4_type = p4_file_type;
	finfo->size = sz_remaining;
	LOG_GITP4_INFO("%s\n", sb_line.buf);
	switch (p4_file_type) {
	case P4_FORMAT_UTF16_TYPE:
		icd = iconv_open("utf8", "utf16");
		break;
	case P4_FORMAT_UTF8_TYPE:
		skip_bom = "\xEF\xBB\xBF";
		break;
	default:
		break;
	}
	while (sz_remaining > 0) {
		ssize_t sz = strbuf_overwrite_read(&sb, fp, sz_remaining > 8192 ? 8192:sz_remaining);
		const char *buf = sb.buf;
		size_t len = sb.len;
		if (0 == sz)
			die ("Unexpected EOF");
		if (0 >= sz) {
			if (EAGAIN == errno)
				continue;
			else if (EINTR == errno)
				continue;
			else
				die("read from git show failed");
		}
		sz_remaining -= sz;
		while (skip_bom) {
			if (*skip_bom == '\0') {
				skip_bom = NULL;
				break;
			}
			if (len == 0)
				break;
			if (*skip_bom == *buf) {
				skip_bom++;
				buf++;
				len--;
			}
			else
				skip_bom = NULL;
		}
		if (icd) {
			reencode_strbuf_iconv(&sb_reenc, &sb, icd);
			buf = sb_reenc.buf;
			len = sb_reenc.len;
		}
		md5_update(&md5_ctx, (uint8_t *)buf, len);
	}
	assert(sz_remaining == 0);
	strbuf_getwholeline(&sb_line, fp, '\n');
	switch (p4_file_type) {
	case P4_FORMAT_LINK_TYPE:
		LOG_GITP4_DEBUG("Appending extra \\n to link \n");
		md5_update(&md5_ctx, (uint8_t *)"\n", 1);
		break;
	default:
		break;
	}
	if (icd)
		iconv_close(icd);
	md5_final(finfo->md5.md5, &md5_ctx);
_err:
	if (report_first)
		strbuf_list_free(report_first);
	strbuf_release(&sb_line);
	strbuf_release(&sb_reenc);
	strbuf_release(&sb);
	return res;
}

static uintmax_t create_list_of_p4_file_from_changelist(struct list_head *plist, const struct depot_change_range_t *prng)
{
	struct child_process child_p4 = CHILD_PROCESS_INIT;
	struct hashmap map;
	uintmax_t n_files = 0;
	str_dict_init(&map);
	child_p4.out = -1;
	argv_array_push(&child_p4.args, "fstat");
	argv_array_push(&child_p4.args, "-Ol");
	argv_array_pushf(&child_p4.args, "%s...@%d", prng->depot_path.buf, prng->start_changelist);
	p4_start_command(&child_p4);
	while (py_marshal_parse(&map, child_p4.out))
	{
		struct md5_id md5;
		if (str_dict_strcmp(&map, "code", "stat"))
			continue;
		if (str_dict_has(&map, "digest")) {
			get_md5_hex(str_dict_get_value(&map, "digest"), &md5);
			if (IS_LOG_DEBUG_ALLOWED) {
				fprintf(p4_verbose_debug.fp, "Digest %s %s\n",
						str_dict_get_value(&map, "digest"),
						md5_to_hex(&md5));
			}
		}
		if (strstr(str_dict_get_value(&map, "headAction"), "delete") == NULL) {
			list_depot_files_add(plist,
					str_dict_get_value(&map, "depotFile"),
					prng->start_changelist,
					0,
					p4type2mode(str_dict_get_value(&map, "headType")),
					p4type2bintype(str_dict_get_value(&map, "headType")),
					md5);
			n_files++;
		}
	}
	close(child_p4.out);
	str_dict_destroy(&map);
	finish_command(&child_p4);
	return n_files;
}

static int get_p4_settings_by_commit(struct hashmap *p4_settings, const char *commit_sha1)
{
		struct strbuf sb = STRBUF_INIT;
		str_dict_reset(p4_settings);
		extract_log_message(commit_sha1, &sb);
		extract_p4_settings_git_log(p4_settings, sb.buf);
		strbuf_release(&sb);
		if (!str_dict_has(p4_settings, "depot-paths") ||
				!str_dict_has(p4_settings, "change"))
			return 1;
		return 0;
}

static int p4fsck_by_commit(const char *commit_sha1)
{
	struct hashmap p4_settings;
	LIST_HEAD(cl_depot_files);
	struct list_head *liter;
	struct depot_change_range_t chg_range = DEPOT_CHANGE_RANGE_INIT;
	struct child_process git_cat_file = CHILD_PROCESS_INIT;
	FILE *fp_out;
	struct git_p4_file_stats git_p4_stats;
	uintmax_t git_n_files_dont_match = 0;
	git_p4_file_stats_init(&git_p4_stats);
	str_dict_init(&p4_settings);
	if (get_p4_settings_by_commit(&p4_settings, commit_sha1) != 0)
		goto _err;
	fprintf(stdout, "checking commit: %s p4: %s...@%s\n",
			commit_sha1,
			str_dict_get_value(&p4_settings, "depot-paths"),
			str_dict_get_value(&p4_settings, "change"));
	strbuf_addstr(&chg_range.depot_path, str_dict_get_value(&p4_settings, "depot-paths"));
	chg_range.start_changelist = atoi(str_dict_get_value(&p4_settings, "change"));
	chg_range.end_changelist = chg_range.start_changelist;
	git_cat_file.in = -1;
	git_cat_file.out = -1;
	argv_array_push(&git_cat_file.args, "git");
	argv_array_push(&git_cat_file.args, "cat-file");
	argv_array_push(&git_cat_file.args, "--batch");
	if (start_command(&git_cat_file)) {
		die("cannot start git show");
	}
	fp_out = fdopen(git_cat_file.out, "r");
	uintmax_t p4_n_files = create_list_of_p4_file_from_changelist(&cl_depot_files, &chg_range);
	uintmax_t git_n_files = 0;
	fprintf(stdout, "Total Files\n");
	list_for_each(liter, &cl_depot_files) {
		struct git_file_info f_info;
		struct depot_file_t *dp;
		struct strbuf sb_git_path = STRBUF_INIT;
		dp = list_entry(liter, struct depot_file_t, lhead);
		const char *sub_path = NULL;
		if (!starts_with(dp->depot_path_file.buf, chg_range.depot_path.buf))
			continue;
		sub_path = dp->depot_path_file.buf + chg_range.depot_path.len;
		strbuf_addf(&sb_git_path, "%s:", commit_sha1);
		wildcard_decode(&sb_git_path, sub_path);
		strbuf_addstr(&sb_git_path, "\n");
		write_in_full(git_cat_file.in, sb_git_path.buf, sb_git_path.len);
		LOG_GITP4_INFO("%s %s\n", p4_type_to_str(dp->bin_type), sub_path);
		if (compute_md5_from_git(fp_out, dp->bin_type, &f_info) != 0) {
			fprintf(stdout, "\r%s [Missing]\n", sub_path);
		}
		else {
			git_n_files ++;
			git_p4_file_stats_add_info(&git_p4_stats, &f_info);
			if (md5cmp(&dp->hash, &f_info.md5)) {
				fprintf(stdout, "\r%s [KO] p4:%s/git:%s\n", sub_path, md5_to_hex(&dp->hash), md5_to_hex(&f_info.md5));
				git_n_files_dont_match ++;
			}
		}
		fprintf(stdout, "\rchecked: %10" PRIuMAX "/ total: %10" PRIuMAX,
				git_n_files, p4_n_files);
		strbuf_release(&sb_git_path);
	}
	fprintf(stdout, "\nTotal checked: %" PRIuMAX " failed %" PRIuMAX "\n",
			git_n_files, git_n_files_dont_match);
	fprint_gitp4_stats(stdout, &git_p4_stats);
	close(git_cat_file.in);
	fclose(fp_out);
	finish_command(&git_cat_file);
_err:
	list_depot_files_destroy(&cl_depot_files);
	depot_change_range_destroy(&chg_range);
	str_dict_destroy(&p4_settings);
	if (git_n_files_dont_match)
		return 1;
	return 0;
}

static int p4fsck_cmd_run(command_t *pcmd, int argc, const char **argv)
{
	struct option options[] = {
		OPT_END()
	};
	struct child_process child_git = CHILD_PROCESS_INIT;
	struct strbuf sbline = STRBUF_INIT;
	FILE *fp;
	int res = 0;
	argc = parse_options(argc, argv, NULL, options, NULL, 0);
	child_git.git_cmd = 1;
	child_git.out = -1;
	argv_array_push(&child_git.args, "rev-list");
	for (;argc; argc--) {
		argv_array_push(&child_git.args, *argv++);
	}
	if (start_command(&child_git)) {
		die("cannot start git rev-list");
	}
	fp = xfdopen(child_git.out, "r");
	while (strbuf_getline(&sbline, fp) == 0) {
		if ((p4fsck_by_commit(sbline.buf)) != 0)
			res = 1;
	}
	fclose(fp);
	strbuf_release(&sbline);
	return res;
}

static void p4fsck_cmd_init(struct command_t *pcmd)
{
	strbuf_init(&pcmd->strb_usage, 256);
	strbuf_addf(&pcmd->strb_usage, "check the integrety of the git repo with p4 depot");
	strbuf_addf(&pcmd->strb_usage, "Usage: git-p4 fsck depot-path CL");
	pcmd->needs_git = 0;
	pcmd->verbose = 0;
	pcmd->run_fn = p4fsck_cmd_run;
	pcmd->deinit_fn = p4_cmd_default_deinit;
	pcmd->data = NULL;
}

static int p4cherry_pick_cmd_run(command_t *pcmd, int argc, const char **argv)
{
	struct option options[] = {
		OPT_END()
	};
	struct depot_change_range_t chg_range = DEPOT_CHANGE_RANGE_INIT;
	struct strbuf commit_sha1 = STRBUF_INIT;
	struct child_process git_format_patch = CHILD_PROCESS_INIT;
	argc = parse_options(argc, argv, NULL, options, NULL, 0);
	if (p4format_patch_parse(argc, argv, &chg_range) != 0) {
		die("Error parsing changelists");
	}
	if (IS_LOG_DEBUG_ALLOWED)
		print_change_range(p4_verbose_debug.fp, &chg_range);
	if (p4export_change_commit(&commit_sha1, &chg_range))
		die("p4 export failed");
	argv_array_push(&git_format_patch.args, "git");
	argv_array_push(&git_format_patch.args, "cherry-pick");
	argv_array_pushf(&git_format_patch.args, "%s", commit_sha1.buf);
	if (start_command(&git_format_patch))
		die("cannot start git format-patch");
	depot_change_range_destroy(&chg_range);
	return finish_command(&git_format_patch);
}

void p4cherry_pick_cmd_init(struct command_t *pcmd)
{
	strbuf_init(&pcmd->strb_usage, 256);
	strbuf_addf(&pcmd->strb_usage, "cherry pick a p4 CL");
	strbuf_addf(&pcmd->strb_usage, "Usage: git-p4 cherry-pick [base p4 path] [CL]");
	pcmd->needs_git = 0;
	pcmd->verbose = 0;
	pcmd->run_fn = p4cherry_pick_cmd_run;
	pcmd->deinit_fn = p4_cmd_default_deinit;
	pcmd->data = NULL;
}

static int p4fast_export_cmd_run(command_t *pcmd, int argc, const char **argv)
{
	struct option options[] = {
		OPT_END()
	};
	struct depot_change_range_t chg_range = DEPOT_CHANGE_RANGE_INIT;
	argc = parse_options(argc, argv, NULL, options, NULL, 0);
	if (p4format_patch_parse(argc, argv, &chg_range) != 0) {
		die("Error parsing changelists");
	}
	if (IS_LOG_DEBUG_ALLOWED)
		print_change_range(p4_verbose_debug.fp, &chg_range);
	p4export_change(STDOUT_FILENO, &chg_range);
	depot_change_range_destroy(&chg_range);
	return 0;
}

void p4fast_export_cmd_init(struct command_t *pcmd)
{
	strbuf_init(&pcmd->strb_usage, 256);
	strbuf_addf(&pcmd->strb_usage, "fast-export a p4 CL");
	strbuf_addf(&pcmd->strb_usage, "Usage: git-p4 fast-export [base p4 path] [CL]");
	pcmd->needs_git = 0;
	pcmd->verbose = 0;
	pcmd->run_fn = p4fast_export_cmd_run;
	pcmd->deinit_fn = p4_cmd_default_deinit;
	pcmd->data = NULL;
}

void p4format_patch_cmd_init(struct command_t *pcmd)
{
	strbuf_init(&pcmd->strb_usage,0);
	pcmd->needs_git = 0;
	pcmd->verbose = 0;
	pcmd->run_fn = p4format_patch_cmd_run;
	pcmd->deinit_fn = p4_cmd_default_deinit;
	pcmd->data = NULL;
}

void p4fetch_cmd_init(struct command_t *pcmd)
{
	strbuf_init(&pcmd->strb_usage, 0);
	pcmd->needs_git = 0;
	pcmd->verbose = 0;
	pcmd->run_fn = p4fetch_cmd_run;
	pcmd->deinit_fn = p4_cmd_default_deinit;
	pcmd->data = NULL;
}

typedef struct command_list_t {
	const char *gitp4cmd;
	void (*cmd_init)(struct command_t *pcmd);
} command_list_t;

const command_list_t cmd_lst[] = 
{
	{"debug", p4debug_cmd_init},
	{"submit", p4submit_cmd_init},
	{"shelve", p4shelve_cmd_init},
	{"format-patch", p4format_patch_cmd_init},
	{"discover-branches", p4discover_branches_cmd_init},
	{"cherry-pick", p4cherry_pick_cmd_init},
	{"fast-export", p4fast_export_cmd_init},
	{"fetch", p4fetch_cmd_init},
	{"fsck", p4fsck_cmd_init}
};

void print_usage(FILE *fp, const char *progname)
{
	int i;
	fprintf(fp,"usage: %s <command> [options]\n", progname);
	fprintf(fp,"\n");
	fprintf(fp,"valid commands:");
	for (i=0;i<ARRAY_SIZE(cmd_lst);i++)
	{
		fprintf(fp," %s",cmd_lst[i].gitp4cmd);
		if ((ARRAY_SIZE(cmd_lst) - i) > 1)
		{
			fprintf(fp,",");
		}
	}
	fprintf(fp,"\n");
	fprintf(fp,"\n");
	fprintf(fp,"Try %s <command> --help for command specific help.\n",progname);
	fprintf(fp,"\n");
}

command_t *cmd_init_by_name(const char *p4cmd, command_t *cmd)
{
	int i;
	for (i=0; i<ARRAY_SIZE(cmd_lst); i++)
	{
		if (strcmp(cmd_lst[i].gitp4cmd,p4cmd) == 0)
		{
			cmd_lst[i].cmd_init(cmd);
			return cmd;
		}
	}
	return NULL;
}

int cmd_main(int argc, const char **argv)
{
	command_t cmd;
	const char *prog_name = argv[0];
	const char *p4cmd_name = NULL;
	int debuglevel = 0;
	int res = 0;
	struct option options[] = {
		OPT_COUNTUP('d', NULL, &debuglevel, N_("Debug level, the more -d the higher the debug level")),
		OPT_END()
	};
	setup_git_directory();
	argc = parse_options(argc, argv, NULL, options, NULL, PARSE_OPT_STOP_AT_NON_OPTION);
	p4_verbose_init(&p4_verbose_debug, debuglevel, NULL);
	if (!argc)
	{
		print_usage(stderr,prog_name);
		exit(2);
	}
	LOG_GITP4_DEBUG("git working directory: %s\n", get_git_work_tree());
	p4cmd_name = argv[0];
	if (cmd_init_by_name(p4cmd_name,&cmd) == NULL)
	{
		fprintf(stderr,"unknown command %s\n",p4cmd_name);
		fprintf(stderr,"\n");
		print_usage(stderr,prog_name);
		exit(2);
	}
	res = cmd.run_fn(&cmd, argc, argv);
	p4_cmd_destroy(&cmd);
	p4usermap_cache_destroy();
	return res;
}

