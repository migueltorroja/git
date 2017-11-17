/*
 *  git-p4 (C porting)
 *
 *  Copyright (C) 2017 Miguel Torroja <miguel.torroja@gmail.com>
 */
#include "git-compat-util.h"
#include "builtin.h"
#include "strbuf.h"
#include "hashmap.h"
#include "parse-options.h"
#include "gettext.h"
#include "config.h"
#include "run-command.h"



#define PY_MARSHAL_TYPE_DICT    '{'
#define PY_MARSHAL_TYPE_STRING  's'
#define PY_MARSHAL_TYPE_INT     'i'
#define PY_MARSHAL_TYPE_NULL    '0'
#define PY_MARSHAL_TYPE_EOF     'E'

typedef struct keyval_t{
	struct hashmap_entry ent;
	int    self_alloc;
	struct strbuf key;
	struct strbuf val;
} keyval_t;

keyval_t *keyval_init(keyval_t *kw);
void keyval_copy(keyval_t *dst, keyval_t *src);
struct hashmap *py_marshal_parse(FILE *fp);
static void py_dict_destroy(struct hashmap *map);
void keyval_print(FILE *fp, keyval_t *kw);
void keyval_release(keyval_t *kw);
void keyval_append_key_f(keyval_t *kw, FILE *fp, size_t n);

static void wildcard_encode(struct strbuf *sb);

struct command_t;

typedef void (*run_type) (struct command_t *pcmd,int gargc, const char **gargv);
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



struct p4_verbose_debug_t {
	unsigned int verbose_level;
	FILE *fp;
};

#define P4_VERBOSE_CRITICAL_LEVEL	(0)
#define P4_VERBOSE_INFO_LEVEL		(1)
#define P4_VERBOSE_DEBUG_LEVEL		(2)

#define P4_VERBOSE_INIT {0,NULL}

struct p4_verbose_debug_t p4_verbose_debug = P4_VERBOSE_INIT;

void p4_verbose_init(struct p4_verbose_debug_t *p4verbose, int level, FILE *fp)
{
	p4verbose->verbose_level = level;
	if (fp)
		p4verbose->fp = fp;
	else
		p4verbose->fp = stderr;
}



__attribute__((format (vprintf,2,3)))
static void p4_verbose_vprintf(int loglevel, const char *fmt, va_list ap)
{
	if (loglevel > p4_verbose_debug.verbose_level)
		return;
	if (p4_verbose_debug.verbose_level)
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

static int p4keyval_cmp(const void *userdata,
		const void *entry,
		const void *entry_or_key,
		const void *keydata)
{
	struct keyval_t *e1 = (struct keyval_t *) entry;
	struct keyval_t *e2 = (struct keyval_t *) entry_or_key;
	return strcmp(e1->key.buf, keydata ? keydata : e2->key.buf); 
}



static void py_dict_init(struct hashmap *map)
{
	hashmap_init(map, p4keyval_cmp, NULL, 0);
}

static void py_dict_destroy(struct hashmap *map)
{
	struct hashmap_iter hm_iter;
	hashmap_iter_init(map, &hm_iter);
	keyval_t *kw;
	while ((kw = hashmap_iter_next(&hm_iter)))
		keyval_release(kw);
	hashmap_free(map,0);
}


static void py_dict_reset(struct hashmap *map)
{
	py_dict_destroy(map);
	py_dict_init(map);
}

static void py_dict_put_kw(struct hashmap *map, keyval_t *kw)
{
	keyval_t *prev_kw = NULL;
	hashmap_entry_init(kw,strhash(kw->key.buf));
	while ((prev_kw = hashmap_remove(map,kw, kw->key.buf)))
		keyval_release(prev_kw);
	hashmap_put(map,kw);
}

static void py_dict_set_key_val(struct hashmap *map, const char *key, const char *val)
{
	keyval_t *kw = keyval_init(NULL);
	strbuf_addf(&kw->key,"%s",key);
	strbuf_addf(&kw->val,"%s",val);
	py_dict_put_kw(map,kw);
}

static const char *py_dict_get_value(struct hashmap *map, const char *str)
{
	keyval_t *kw;
	kw = hashmap_get_from_hash(map, strhash(str), str);
	if (NULL == kw)
		return NULL;
	return kw->val.buf;
}

static void py_dict_print(FILE *fp, struct hashmap *map)
{
	if (NULL == fp)
		fp = stdout;
	if (hashmap_get_size(map))
	{
		fprintf(fp,"{");
		struct hashmap_iter hm_iter;
		keyval_t *kw;
		hashmap_iter_init(map, &hm_iter);
		kw = hashmap_iter_next(&hm_iter);
		assert(NULL != kw); 
		for (;;) {
			keyval_print(fp, kw);
			kw = hashmap_iter_next(&hm_iter);
			if (kw)
				fprintf(fp,", ");
			else
				break;
		}
		fprintf(fp,"}\n");
	} 
}

static void py_dict_copy(struct hashmap *dst, struct hashmap *src)
{
	py_dict_reset(dst);
	keyval_t *kw;
	struct hashmap_iter hm_iter;
	hashmap_iter_init(src, &hm_iter);
	while ((kw = hashmap_iter_next(&hm_iter))) {
		keyval_t *copykw = keyval_init(NULL);
		keyval_copy(copykw, kw);
		py_dict_put_kw(dst, copykw);
	}
}

/* function that runs the p4 with the python marshal output format
 * Each new py dict created is passed to a callback
 * it returns the exit status of the p4 command process 
 */

static int p4_cmd_run(const char **argv, const char *dir, void (*cb) ( struct hashmap *map, void *datain), void *data)
{
	struct child_process child_p4 = CHILD_PROCESS_INIT;
	struct hashmap *map;
	FILE *fp;
	argv_array_push(&child_p4.args, "p4");
	argv_array_push(&child_p4.args, "-G");
	argv_array_pushv(&child_p4.args, argv);
	child_p4.out = -1;
	if (dir) {
		child_p4.dir = dir;
		argv_array_pushf(&child_p4.env_array, "PWD=%s",dir);
	}
	if (start_command(&child_p4))
		die("cannot start p4 process");
	fp = fdopen(child_p4.out,"r");
	while (NULL != (map = py_marshal_parse(fp)))
	{
		if (cb)
			cb(map,data);
		py_dict_destroy(map);
		free(map);
	}
	fclose(fp);
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
	else if (!strcmp(legacy_type,"xutf16")) {
		strbuf_addf(base_type, "utf16");
		strbuf_addf(mods,"+x");
	}
	else {
		die("p4 type not recognized: %s", legacy_type);
	}
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
	const char *p4type_str = py_dict_get_value(map, "type");
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
	if (ends_with(mode_type.buf, "755"))
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
	perm_val = py_dict_get_value(map, "perm");
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

static void p4debug_print_cb(struct hashmap *map, void *data)
{
	FILE *fp = (FILE *) data;
	py_dict_print(fp, map);
}

static void p4debug_cmd_run(command_t *pcmd, int gargc, const char **gargv)
{
	struct argv_array dargs = ARGV_ARRAY_INIT;
	const char **args = gargv+1;
	gargc --;
	for (;gargc; gargc--,args++) {
		argv_array_push(&dargs, *args);
	}
	p4_cmd_run(dargs.argv, NULL, p4debug_print_cb,stdout);
	argv_array_clear(&dargs);
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

static void p4_branches_in_git_cb(struct strbuf *sb_line, void *arg)
{
	struct hashmap *map = (struct hashmap *) arg;
	keyval_t *kw;
	strbuf_trim(sb_line);
	if (strncmp(sb_line->buf,"p4/",3) != 0)
		return;
	if (strncmp(sb_line->buf,"p4/HEAD",7) == 0)
		return;
	kw = keyval_init(NULL);
	strbuf_addstr(&kw->key,sb_line->buf+3);
	parse_revision(&kw->val,sb_line->buf);
	py_dict_put_kw(map,kw);
}

int p4_branches_in_git(struct hashmap *map, int local_branches)
{
	struct argv_array gitargs = ARGV_ARRAY_INIT;
	argv_array_push(&gitargs, "rev-parse");
	argv_array_push(&gitargs, "--symbolic");
	if (local_branches)
		argv_array_push(&gitargs, "--branches");
	else
		argv_array_push(&gitargs, "--remotes");
	if (git_cmd_read_pipe_line(gitargs.argv, p4_branches_in_git_cb, map) != 0)
		die("Error quering p4 branches listn");
	argv_array_clear(&gitargs);
	return 0;
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

int p4_local_branches_in_git(struct hashmap *map)
{
	return p4_branches_in_git(map, 1);
}

int p4_remote_branches_in_git(struct hashmap *map)
{
	return p4_branches_in_git(map, 0);
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
	struct strbuf *sb, *key, *val;
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
					py_dict_put_kw(map,kw);
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
		py_dict_reset(p4settings);
		extract_log_message(strb_commit->buf,&sb);
		rout = extract_p4_settings_git_log(p4settings, sb.buf);
		depot_path = py_dict_get_value(p4settings, "depot-paths");
	}
	else
		rout = -1;
	if (!depot_path)
		rout = -1;
	strbuf_release(&sb);
	return rout;
}

static void get_branch_by_depot(int local , struct hashmap *branch_by_depot_dict)
{
	struct hashmap map;
	struct hashmap_iter hm_iter;
	keyval_t *kw;
	py_dict_init(&map);
	p4_branches_in_git(&map,local);
	hashmap_iter_init(&map, &hm_iter);
	while ((kw = hashmap_iter_next(&hm_iter))) {
		struct strbuf sb = STRBUF_INIT;
		struct hashmap settings_map;
		const char *depot_path = NULL;
		py_dict_init(&settings_map);
		extract_log_message(kw->val.buf,&sb);
		LOG_GITP4_DEBUG("git log message:\n%s\n", sb.buf);
		extract_p4_settings_git_log(&settings_map, sb.buf);
		if (IS_LOG_DEBUG_ALLOWED)
			py_dict_print(p4_verbose_debug.fp, &settings_map);
		depot_path = py_dict_get_value(&settings_map,"depot-paths");
		if (depot_path) {
			struct strbuf remote_branch = STRBUF_INIT;
			strbuf_addf(&remote_branch,"remotes/p4/%s",kw->key.buf);
			py_dict_set_key_val(branch_by_depot_dict,depot_path, remote_branch.buf);
			strbuf_release(&remote_branch);

		}
		strbuf_release(&sb);
		py_dict_destroy(&settings_map);
	}
	py_dict_destroy(&map);
}

int find_upstream_branch_point(int local,struct strbuf *upstream, struct hashmap *p4settings )
{
	struct hashmap branch_bydepot_map;
	struct strbuf parent_commit = STRBUF_INIT;
	int ret = -1;
	py_dict_init(&branch_bydepot_map);
	get_branch_by_depot(local, &branch_bydepot_map);
	if (IS_LOG_DEBUG_ALLOWED) {
		LOG_GITP4_DEBUG("Branch depot map: \n");
		py_dict_print(p4_verbose_debug.fp,&branch_bydepot_map);
	}
	if (find_p4_parent_commit(&parent_commit,p4settings) == 0) {
		const char *upstream_val;
		const char *depot_path = py_dict_get_value(p4settings,"depot-paths");
		upstream_val = py_dict_get_value(&branch_bydepot_map,depot_path);
		if (upstream_val != NULL) {
			strbuf_reset(upstream);
			strbuf_addf(upstream,"%s",upstream_val);
			ret = 0;
		}
		else
			ret = -1;
	}
	strbuf_release(&parent_commit);
	py_dict_destroy(&branch_bydepot_map);
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
	argv_array_push(&child_apply.args, "apply");
	argv_array_push(&child_apply.args, "--check");
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
	depot_file = py_dict_get_value(map, "depotFile");
	data_str = py_dict_get_value(map, "data");
	code_str = py_dict_get_value(map, "code");
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
		strbuf_addstr(argout_client_path, py_dict_get_value(map, "path"));
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
	struct depot_client_path_t arg_cb = {NULL, client_path};
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

static int p4_sync_dir(const char *client_path)
{
	const char *cmd_list[] = { "sync", "...", NULL};
	return p4_cmd_run(cmd_list, client_path, NULL, NULL);
}

static int dir_exists(const char *path)
{
	struct stat st;
	if (stat(path, &st))
		return 0;
	if ((st.st_mode & S_IFMT) == S_IFDIR)
		return 1;
	return 0;
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


typedef struct p4_user_map_t {
	struct strbuf my_p4_user_id;
	struct hashmap users;
	struct hashmap emails;
	int user_map_from_perfroce_server;
} p4_user_map_t;



static void p4usermap_get_id_cb(struct hashmap *map, void *argout)
{
	const char *user_str = NULL;
	struct strbuf *sb_out = (struct strbuf *) argout;
	user_str = py_dict_get_value(map,"User");
	if (user_str) {
		strbuf_reset(sb_out);
		strbuf_addstr(sb_out, user_str);
	}
}

void p4usermap_get_id(struct p4_user_map_t *user_map, struct strbuf *sb_id)
{
	struct hashmap p4_dict;
	py_dict_init(&p4_dict);
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
	struct strbuf sb_user = STRBUF_INIT;
	if ((!user) || (!email) || (!full_name))
		return;
	strbuf_addf(&sb_user,"%s <%s>", full_name, email);
	py_dict_set_key_val(&p4_user_map->users, user, sb_user.buf);
	py_dict_set_key_val(&p4_user_map->emails, email, user);
	strbuf_release(&sb_user);
}

static void p4usermap_update_users_info_cb(struct hashmap *map, void *arg1)
{
	struct p4_user_map_t *p4_user_map = (struct p4_user_map_t *) arg1;
	const char *user = py_dict_get_value(map, "User");
	const char *email = py_dict_get_value(map, "Email");
	const char *full_name = py_dict_get_value(map, "FullName");
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
	py_dict_init(&user_map->users);
	py_dict_init(&user_map->emails);
	p4usermap_update_users_info(user_map);
	git_config(p4usermap_git_config, user_map);
}

void p4usermap_deinit(struct p4_user_map_t *user_map)
{
	py_dict_destroy(&user_map->users);
	py_dict_destroy(&user_map->emails);
	strbuf_release(&user_map->my_p4_user_id);
}

enum { CONFLICT_ASK, CONFLICT_SKIP, CONFLICT_QUIT, CONFLICT_UNKNOWN};

struct p4submit_data_t {
	struct strbuf origin;
	struct strbuf branch; 
	struct strbuf depot_path;
	struct strbuf client_path;
	struct strbuf diff_opts;
	struct p4_user_map_t p4_user_map;
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
	p4usermap_deinit(&p4submit_options.p4_user_map);
	strbuf_release(&p4submit_options.origin);
	strbuf_release(&p4submit_options.branch);
	strbuf_release(&p4submit_options.depot_path);
	strbuf_release(&p4submit_options.client_path);
	strbuf_release(&p4submit_options.diff_opts);
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
	py_dict_set_key_val(map, "src_mode", str_sp_list[0]->buf);
	py_dict_set_key_val(map, "dst_mode", str_sp_list[1]->buf);
	py_dict_set_key_val(map, "src_sha1", str_sp_list[2]->buf);
	py_dict_set_key_val(map, "dst_sha1", str_sp_list[3]->buf);
	if (str_tab_list[0]->len < 1)
		die("Unexpected length for status field");
	else if (str_tab_list[0]->len > 1)
		py_dict_set_key_val(map, "status_score", str_tab_list[0]->buf + 1);
	strbuf_setlen(str_tab_list[0],1);
	py_dict_set_key_val(map, "status", str_tab_list[0]->buf);
	py_dict_set_key_val(map, "src", str_tab_list[1]->buf);
	if ((str_tab_list[0]->buf[0] == 'C') ||
			(str_tab_list[0]->buf[0] == 'R')) {
		if (n < 5)
			die("Error parsing diff %s", str_sp_list[4]->buf);
		py_dict_set_key_val(map, "dst", str_tab_list[4]->buf);
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
	py_dict_init(&fm->exec_bit_changed);
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
	py_dict_destroy(&fm->exec_bit_changed);
	string_list_clear(&fm->all_files, 0);
}


static void p4submit_full_depot_path(const char *sub_path, struct strbuf *full_depot_path)
{
	strbuf_reset(full_depot_path);
	if (!p4submit_options.depot_path.len)
		return;
	strbuf_addbuf(full_depot_path, &p4submit_options.depot_path);
	if (p4submit_options.depot_path.buf[p4submit_options.depot_path.len - 1] != '/')
		strbuf_addstr(full_depot_path, "/");
	strbuf_addstr(full_depot_path, sub_path);
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

static void wildcard_decode(struct strbuf *sb)
{
	die("Not implemented");
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

static int p4_has_move_command(void)
{
	return 0;
}

static int is_git_mode_exec_changed(const char *src_mode, const char *dst_mode)
{
	return (ends_with(src_mode,"755") != ends_with(dst_mode,"755"));
}

static void prepare_p4submit_template_cb(struct hashmap *map, void *arg)
{
	struct hashmap *dst = (struct hashmap *) arg;
	const char *codestr=py_dict_get_value(map, "code");
	if (!codestr)
		return;
	if (!strcmp(codestr, "stat"))
		py_dict_copy(dst, map);
}

static void prepare_p4submit_template(unsigned int changelist, struct strbuf *template)
{
	struct hashmap p4settings;
	struct hashmap change_entry;
	struct strbuf upstream = STRBUF_INIT;
	struct argv_array p4args = ARGV_ARRAY_INIT;
	keyval_t *kw;
	struct hashmap_iter hm_iter;
	struct string_list file_list = STRING_LIST_INIT_DUP;
	struct string_list_item *item;
	const char *depot_path = NULL;
	const char *field_names[] = { "Change", "Client", "User", "Status", "Description", "Jobs", NULL};
	const char **pfield;
	py_dict_init(&change_entry);
	py_dict_init(&p4settings);
	if (find_upstream_branch_point(0, &upstream, &p4settings) < 0)
		die("Error findind upstream");
	depot_path = py_dict_get_value(&p4settings, "depot-paths");
	if (!depot_path)
		die("No depot basis depot path found!");
	argv_array_push(&p4args, "change");
	argv_array_push(&p4args, "-o");
	if (changelist)
		argv_array_pushf(&p4args, "%u", changelist);
	p4_cmd_run(p4args.argv, NULL, prepare_p4submit_template_cb, &change_entry);
	if (!py_dict_get_value(&change_entry, "code"))
		die("Failed to decode output of p4 change -o");
	hashmap_iter_init(&change_entry, &hm_iter);
	while ((kw = hashmap_iter_next(&hm_iter))) {
		if (!starts_with(kw->key.buf, "File"))
			continue;
		if (starts_with(kw->val.buf,depot_path))
			string_list_append(&file_list, kw->val.buf);
	}
	strbuf_reset(template);
	strbuf_addstr(template, "# A Perforce Change Specification.\n");
	strbuf_addstr(template, "#\n");
	strbuf_addstr(template, "#  Change:      The change number. 'new' on a new changelist.\n");
	strbuf_addstr(template, "#  Date:        The date this specification was last modified.\n");
	strbuf_addstr(template, "#  Client:      The client on which the changelist was created.  Read-only.\n");
	strbuf_addstr(template, "#  User:        The user who created the changelist.\n");
	strbuf_addstr(template, "#  Status:      Either 'pending' or 'submitted'. Read-only.\n");
	strbuf_addstr(template, "#  Type:        Either 'public' or 'restricted'. Default is 'public'.\n");
	strbuf_addstr(template, "#  Description: Comments about the changelist.  Required.\n");
	strbuf_addstr(template, "#  Jobs:        What opened jobs are to be closed by this changelist.\n");
	strbuf_addstr(template, "#               You may delete jobs from this list.  (New changelists only.)\n");
	strbuf_addstr(template, "#  Files:       What opened files from the default changelist are to be added\n");
	strbuf_addstr(template, "#               to this changelist.  You may delete files from this list.\n");
	strbuf_addstr(template, "#               (New changelists only.)\n");
	for (pfield = field_names; *pfield; pfield++) {
		const char *val = py_dict_get_value(&change_entry, *pfield);
		struct string_list field_lines = STRING_LIST_INIT_DUP;
		if (!val)
			continue;
		strbuf_addf(template,"\n%s:", *pfield);
		if (!strcmp(*pfield,"Description"))
			strbuf_addch(template,'\n');
		string_list_split(&field_lines, val, '\n', -1);
		for_each_string_list_item(item, &field_lines)
			strbuf_addf(template, "\t%s\n", item->string);
		string_list_clear(&field_lines, 0);
	}
	if (file_list.nr) {
		strbuf_addch(template,'\n');
		strbuf_addstr(template,"Files:\n");
	}
	for_each_string_list_item(item, &file_list) {
		strbuf_addf(template,"\t%s\n", item->string);
	}
	strbuf_release(&upstream);
	py_dict_destroy(&p4settings);
	py_dict_destroy(&change_entry);
	argv_array_clear(&p4args);
	string_list_clear(&file_list, 0);
}

void prepare_log_message(struct strbuf *out,
		struct strbuf *template, struct strbuf *message)
{
	struct string_list template_lines = STRING_LIST_INIT_DUP;
	struct string_list_item *item;
	int in_description_section = 0;
	strbuf_reset(out);
	string_list_split(&template_lines, template->buf, '\n', -1);
	for_each_string_list_item(item, &template_lines) {
		if (starts_with(item->string, "#")) {
			strbuf_addf(out, "%s\n", item->string);
			continue;
		}
		if (in_description_section) {
			if (starts_with(item->string, "Files:"))
				in_description_section = 0;
			else
				continue;
		}
		else {
			if (starts_with(item->string, "Description:")) {
				struct string_list message_lines = STRING_LIST_INIT_DUP;
				struct string_list_item *item_m;
				in_description_section = 1;
				strbuf_addf(out, "%s\n", item->string);
				string_list_split(&message_lines, message->buf, '\n', -1);
				for_each_string_list_item(item_m, &message_lines) {
					strbuf_addf(out, "\t%s\n", item_m->string);
				}
				string_list_clear(&message_lines,0);
				continue;
			}
		}
		strbuf_addf(out, "%s\n", item->string);
	}
	string_list_clear(&template_lines, 0);
}

void dump_p4_log(FILE *fp, const char *commit_id, unsigned int changelist)
{
	struct strbuf description = STRBUF_INIT;
	struct strbuf template = STRBUF_INIT;
	struct strbuf outlog = STRBUF_INIT;
	extract_log_message(commit_id, &description);
	strbuf_trim(&description);
	prepare_p4submit_template(changelist, &template);
	prepare_log_message(&outlog, &template, &description);
	strbuf_write(&outlog, fp);
	strbuf_release(&description);
	strbuf_release(&template);
	strbuf_release(&outlog);
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
	const char *src_sha1, *dst_sha1;
	const char *cli_path = p4submit_options.client_path.buf;
	py_dict_init(&map);
	parse_diff_tree_entry(&map, l->buf);
	val = py_dict_get_value(&map, "status");
	if (val && val[0] && val[1] == '\0')
		modifier = val[0];
	else
		die("Wrong diff line parsed (status) %s",l->buf);
	val = py_dict_get_value(&map, "src");
	if (!val)
		die("Wrong diff line parsed (src) %s",l->buf);
	strbuf_addstr(&src_path, val);
	val = py_dict_get_value(&map, "dst");
	if (val)
		strbuf_addstr(&dst_path, val);
	src_sha1 = py_dict_get_value(&map, "src_sha1");
	dst_sha1 = py_dict_get_value(&map, "dst_sha1");
	src_mode = py_dict_get_value(&map, "src_mode");
	dst_mode = py_dict_get_value(&map, "dst_mode");
	if (IS_LOG_DEBUG_ALLOWED) {
		LOG_GITP4_DEBUG("Converted git info to dict: ");
		py_dict_print(p4_verbose_debug.fp, &map);
	}
	switch (modifier) {
	case 'M':
		p4_edit(cli_path, src_path.buf,0);
		if (is_git_mode_exec_changed(src_mode, dst_mode))
			py_dict_set_key_val(&files_to_update->exec_bit_changed, src_path.buf, dst_mode);
		string_list_insert(&files_to_update->edited, src_path.buf);
		break;
	case 'A':
		string_list_insert(&files_to_update->added, src_path.buf);
		py_dict_set_key_val(&files_to_update->exec_bit_changed, src_path.buf, dst_mode);
		if (0120000 == strtol(dst_mode, NULL, 8))
			string_list_insert(&files_to_update->symlinks, src_path.buf);
		string_list_remove(&files_to_update->deleted, src_path.buf, 0);
		break;
	case 'D':
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
			py_dict_set_key_val(&files_to_update->exec_bit_changed, dst_path->buf, dst_mode);
		}
#ifdef GIT_WINDOWS_NATIVE
		chmod(dst_path->buf, S_IWRITE);
#endif
		unlink(dst_path->buf);
		string_list_insert(&files_to_update->added, dst_path->buf);
#else
#warning "To be implemented"
#endif
		break;
	case 'R':
#warning "To be implemented"
		break;
	case 'T':
		string_list_insert(&files_to_update->type_changed, src_path.buf);
		break;
	default:
		die("Unknown modifier %c for %s", modifier, src_path.buf);
		break;
	}
	py_dict_destroy(&map);
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
	keyval_t *kw;
	p4_files_modified_init(&files_to_update);
	argv_array_push(&gitargs, "diff-tree");
	argv_array_push(&gitargs, "-r");
	if (p4submit_options.diff_opts.len)
		argv_array_push(&gitargs, p4submit_options.diff_opts.buf);
	argv_array_pushf(&gitargs, "%s^", commit_id);
	argv_array_push(&gitargs, commit_id);
	fprintf(stdout,"Applying");
	git_print_short_log(stdout, commit_id);
	git_cmd_read_pipe_line(gitargs.argv, p4submit_apply_cb, &files_to_update);
	argv_array_clear(&gitargs);
	if (git_apply_commit(commit_id,p4submit_options.client_path.buf, 0)) {
#warning "To be implemented"
		LOG_GITP4_CRITICAL("Error applying commit %s\n", commit_id);
		goto leave;
	}
	for_each_string_list_item(item,&files_to_update.type_changed) {
		p4_edit(cli_path, item->string, 1);
	}
	for_each_string_list_item(item,&files_to_update.added) {
		p4_add(cli_path, item->string);
	}
	for_each_string_list_item(item,&files_to_update.deleted) {
		p4_revert(cli_path, item->string);
		p4_delete(cli_path, item->string);
	}
	hashmap_iter_init(&files_to_update.exec_bit_changed, &hm_iter);
	while ((kw = hashmap_iter_next(&hm_iter))) {
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
leave:
	strbuf_release(&user_id);
	p4_files_modified_destroy(&files_to_update);
	return 0;
}

void p4submit_cmd_run(struct command_t *pcmd, int argc, const char **argv)
{
	int i;
	struct hashmap map;
	struct strbuf strb_master = STRBUF_INIT;
	struct strbuf commits = STRBUF_INIT;
	const char *origin = NULL;
	const char *branch = NULL;
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
		strbuf_reset(&p4submit_options.origin);
		strbuf_addf(&p4submit_options.origin, "%s",origin);
	}

	if (branch) {
		strbuf_reset(&p4submit_options.branch);
		strbuf_addf(&p4submit_options.branch, "%s",branch);
	}
	if (IS_LOG_DEBUG_ALLOWED) {
		py_dict_init(&map);
		p4_local_branches_in_git(&map);
		LOG_GITP4_DEBUG("Local:\n");
		py_dict_print(p4_verbose_debug.fp, &map);
		py_dict_destroy(&map);
		py_dict_init(&map);
		p4_remote_branches_in_git(&map);
		LOG_GITP4_DEBUG("Remotes:\n");
		py_dict_print(p4_verbose_debug.fp, &map);
		py_dict_destroy(&map);
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
		struct strbuf upstream = STRBUF_INIT;
		struct hashmap dict_map;
		py_dict_init(&dict_map);
		if (find_upstream_branch_point(0, &upstream, &dict_map) == 0) {
			strbuf_reset(&p4submit_options.depot_path);
			strbuf_addf(&p4submit_options.depot_path,"%s", py_dict_get_value(&dict_map,"depot-paths"));
			if (p4submit_options.origin.len == 0)
				strbuf_addf(&p4submit_options.origin, "%s", upstream.buf);
			LOG_GITP4_DEBUG("Upstream: %s\n",upstream.buf);
			if (IS_LOG_DEBUG_ALLOWED)
				py_dict_print(p4_verbose_debug.fp, &dict_map);
			LOG_GITP4_DEBUG("Upstream: %s\n",p4submit_options.origin.buf);
			LOG_GITP4_DEBUG("depot-path: %s\n", p4submit_options.depot_path.buf);
		}
		strbuf_release(&upstream);
		py_dict_destroy(&dict_map);
	} while(0);

	if (p4submit_options.update_shelve_cl)
		p4submit_options.shelve = 1;

	if (p4submit_options.preserve_user &&
			!p4_has_admin_permissions(p4submit_options.depot_path.buf))
		die("Cannot preserve user names without p4 super-user or admin permissions");
	p4_where(p4submit_options.depot_path.buf,&p4submit_options.client_path);
	if (!p4submit_options.client_path.len)
		die("Error: Cannot locate perforce checkout of %s in client view", p4submit_options.depot_path.buf);
	fprintf(stdout, "Perforce checkout for depot path %s located at %s\n",
			p4submit_options.depot_path.buf,
			p4submit_options.client_path.buf);
	if (!p4submit_options.dry_run) {
		fprintf(stdout, "Synchronizing p4 checkout...\n");
		p4_sync_dir(p4submit_options.client_path.buf);
		if (!dir_exists(p4submit_options.client_path.buf))
			die("Directory does not exist %s\n",p4submit_options.client_path.buf);
	}
	else {
		fprintf(stdout, "Would synchronize p4 checkout in %s\n", p4submit_options.client_path.buf);
	}
	if (p4_nfiles_opened(p4submit_options.client_path.buf))
		die("You have files opened with perforce! Close them before starting the sync.");
	git_list_commits(p4submit_options.origin.buf, strb_master.buf, &commits);
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
			else
				p4submit_apply(commit_id);
		}
		strbuf_list_free(strb_list);
	}
	strbuf_release(&strb_master);
	strbuf_release(&commits);
}


static const char *p4usermap_get_user_by_email(struct p4_user_map_t *p4_user_map, const char *email)
{
	return py_dict_get_value(&p4submit_options.p4_user_map.emails,email);
}

static void p4submit_user_for_commit(const char *commit, struct strbuf *user, struct strbuf *email)
{
	const char *git_cmd_list[] = {"log", "--max-count=1", "--format=%ae", commit, NULL};
	const char *_usr;
	git_cmd_read_pipe_full(git_cmd_list, email);
	strbuf_trim(email);
	strbuf_reset(user);
	_usr = p4usermap_get_user_by_email(&p4submit_options.p4_user_map, email->buf);
	if (_usr)
		strbuf_addstr(user,_usr);
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

void p4submit_cmd_init(struct command_t *pcmd)
{
	strbuf_init(&pcmd->strb_usage,0);
	pcmd->needs_git = 0;
	pcmd->verbose = 0;
	pcmd->run_fn = p4submit_cmd_run;
	pcmd->deinit_fn = p4submit_cmd_deinit;
	pcmd->data = NULL;
	memset(&p4submit_options, 0, sizeof(p4submit_options));
	strbuf_init(&p4submit_options.origin, 0);
	strbuf_init(&p4submit_options.branch, 0);
	strbuf_init(&p4submit_options.depot_path, 0);
	strbuf_init(&p4submit_options.client_path, 0);
	strbuf_init(&p4submit_options.diff_opts, 0);
	git_config(p4submit_git_config,NULL);
	p4usermap_init(&p4submit_options.p4_user_map);
}

void p4shelve_cmd_init(struct command_t *pcmd)
{
	p4submit_cmd_init(pcmd);
	p4submit_options.shelve = 1;
}



keyval_t *keyval_init(keyval_t *kw)
{
	if (kw) {
		kw->self_alloc = 0;
	}
	else {
		kw = malloc(sizeof(keyval_t));
		if (!kw) die("Not enough memory\n");
		kw->self_alloc = 1;
	}
	strbuf_init(&kw->key,0);
	strbuf_init(&kw->val,0);
	return kw;
}

void keyval_append_key_f(keyval_t *kw, FILE *fp, size_t n)
{
	while (n)
	{
		ssize_t res;
		res = strbuf_fread(&kw->key, n, fp);
		if ((res < 0) && (errno != EAGAIN) ) {
			die("Error appending key reading fp\n");
		}
		else if (res > 0) {
			n -= res;
		}
	}
}


void keyval_append_val_f(keyval_t *kw, FILE *fp, size_t n)
{
	while (n)
	{
		ssize_t res;
		res = strbuf_fread(&kw->val, n, fp);
		if ((res < 0) && (errno != EAGAIN) ) {
			die("Error appending val reading fp\n");
		}
		else if (res > 0) {
			n -= res;
		}
	}
}


void keyval_print(FILE *fp, keyval_t *kw)
{
	size_t i;
	if (NULL == fp)
		fp = stdout;
	fprintf(fp,"'%.*s': ", (int)kw->key.len, kw->key.buf);
	fprintf(fp,"'");
	for (i=0;i<kw->val.len;i++) {
		char c = kw->val.buf[i];
		switch (c) {
			case '\'':
				fputs("\\'", fp);
				break;
			case '\n':
				fputs("\\n", fp);
				break;
			case '\t':
				fputs("\\t", fp);
				break;
			case '\r':
				fputs("\\r", fp);
				break;
			default:
				if (isprint(c)) {
					fputc(c, fp);
				}
				break;
		}
	}
	fprintf(fp, "'");
}

void keyval_copy(keyval_t *dst, keyval_t *src)
{
	strbuf_reset(&dst->key);
	strbuf_reset(&dst->val);
	strbuf_addbuf(&dst->key, &src->key);
	strbuf_addbuf(&dst->val, &src->val);
}

void keyval_release(keyval_t *kw)
{
	strbuf_release(&kw->key);
	strbuf_release(&kw->val);
	if (kw->self_alloc)
		free(kw);
}

static int fread_int32_t(FILE *fp, int32_t *v)
{
	*v = 0;
	uint8_t bytes[4];
	if (0 == fread(bytes,sizeof(bytes), 1, fp))
	{
		die("Error reading long\n");
	}
	*v  = bytes[0];
	*v |= bytes[1] << 8;
	*v |= bytes[2] << 16;
	*v |= bytes[3] << 24;
	return 0;
}

struct hashmap *py_marshal_parse(FILE *fp)
{
#define PY_MARSHAL_WAIT_FOR_KEY     (0)
#define PY_MARSHAL_WAIT_FOR_VAL     (1)
	int32_t vi32,len;
	int c; 
	int state = PY_MARSHAL_WAIT_FOR_KEY;
	keyval_t *kw = NULL;
	struct hashmap *map = NULL;

	for (;;)
	{ 
		c = fgetc(fp);
		switch(c)
		{
			case EOF:
				assert(NULL == kw);
				assert(NULL == map);
				goto _leave;
			case PY_MARSHAL_TYPE_STRING:
				fread_int32_t(fp,&len);
				if (state == PY_MARSHAL_WAIT_FOR_KEY)
				{
					kw = keyval_init(NULL);
					keyval_append_key_f(kw,fp, len);
					state = PY_MARSHAL_WAIT_FOR_VAL;
				}
				else
				{
					keyval_append_val_f(kw,fp, len);
					py_dict_put_kw(map, kw);
					kw = NULL;
					state = PY_MARSHAL_WAIT_FOR_KEY;
				}
				break;
			case PY_MARSHAL_TYPE_INT:
				// Converting integer to string
				// In order to keep the key/val simple we convert to string always as there are
				// very few integers reported by p4/python marshal interface and having strings
				// is more generic
				assert(state == PY_MARSHAL_WAIT_FOR_VAL);
				fread_int32_t(fp,&vi32);
				strbuf_addf(&kw->val,"%d",vi32);
				py_dict_put_kw(map, kw);
				kw = NULL;
				state = PY_MARSHAL_WAIT_FOR_KEY;
				break;
			case PY_MARSHAL_TYPE_NULL:
				assert(NULL == kw);
				assert(NULL != map);
				return map;
				break;
			case PY_MARSHAL_TYPE_DICT:
				// Do Nothing
				assert(NULL == map); 
				map = malloc(sizeof(struct hashmap));
				py_dict_init(map);
				break;
			default:
				LOG_GITP4_CRITICAL("Not supported: %d\n",c);
				die("Not supported\n"); 
				goto _leave;
		}
	}
	assert(NULL == kw);
_leave:
	if (NULL != map) {
		py_dict_destroy(map);
		free(map);
	}
	return NULL;
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
	p4cmd_name = argv[0];
	if (cmd_init_by_name(p4cmd_name,&cmd) == NULL)
	{
		fprintf(stderr,"unknown command %s\n",p4cmd_name);
		fprintf(stderr,"\n");
		print_usage(stderr,prog_name);
		exit(2);
	}
	cmd.run_fn(&cmd, argc, argv);
	p4_cmd_destroy(&cmd);
	return 0;
}

