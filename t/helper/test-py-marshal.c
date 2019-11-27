#include "git-compat-util.h"
#include "cache.h"
#include "strbuf.h"
#include "vcs-p4/strbuf-dict.h"
#include "vcs-p4/py-marshal.h"


#define PY_MARSHAL_TYPE_DICT_START '{'
#define PY_MARSHAL_TYPE_STRING     's'
#define PY_MARSHAL_TYPE_INT        'i'
#define PY_MARSHAL_TYPE_NULL       '0'


static void write_32i_le(struct strbuf *sb, int32_t v)
{
	strbuf_addch(sb, (v ) & 0xFF);
	strbuf_addch(sb, (v >> 8) & 0xFF);
	strbuf_addch(sb, (v >> 16) & 0xFF);
	strbuf_addch(sb, (v >> 24) & 0xFF);
}

static void py_marshal_gen_start_dict(struct strbuf *sb)
{
	strbuf_addch(sb, PY_MARSHAL_TYPE_DICT_START);
}

static void py_marshal_gen_stop_dict(struct strbuf *sb)
{
	strbuf_addch(sb, PY_MARSHAL_TYPE_NULL);
}

static void py_marshal_add_int(struct strbuf *sb, int32_t v)
{
	strbuf_addch(sb, PY_MARSHAL_TYPE_INT);
	write_32i_le(sb, v);
}

static void py_marshal_add_str(struct strbuf *sb, const char *str)
{
	size_t sz = strlen(str);
	strbuf_addch(sb, PY_MARSHAL_TYPE_STRING);
	write_32i_le(sb, sz);
	strbuf_add(sb, str, sz);
}

static void py_marshal_add_key_sval(struct strbuf *sb, const char *k, const char *v)
{
	py_marshal_add_str(sb, k);
	py_marshal_add_str(sb, v);
}

static void py_marshal_add_key_ival(struct strbuf *sb, const char *k, int32_t v)
{
	py_marshal_add_str(sb, k);
	py_marshal_add_int(sb, v);
}

const struct {
	const char *key;
	const char *val;
} list_of_key_svals[] = {
	{"user", "John Smith"},
	{"town", "Springfield"},
};

const struct {
	const char *key;
	int32_t val;
} list_of_key_ivals[] = {
	{"ext", 1234},
	{"phone", 55555555},
};


static int out_marshal_1(void)
{
	struct strbuf sb = STRBUF_INIT;
	size_t sz;
	py_marshal_gen_start_dict(&sb);
	for (sz = 0; sz < ARRAY_SIZE(list_of_key_svals); sz++) {
		py_marshal_add_key_sval(&sb, list_of_key_svals[sz].key,
				list_of_key_svals[sz].val);
	}
	for (sz = 0; sz < ARRAY_SIZE(list_of_key_ivals); sz++) {
		py_marshal_add_key_ival(&sb, list_of_key_ivals[sz].key,
				list_of_key_ivals[sz].val);
	}
	py_marshal_gen_stop_dict(&sb);
	write_in_full(STDOUT_FILENO, sb.buf, sb.len);
	strbuf_release(&sb);
	return 0;
}

static int out_marshal_2(void)
{
	struct strbuf sb = STRBUF_INIT;
	py_marshal_gen_start_dict(&sb);
	py_marshal_add_key_sval(&sb, "city", "Nashville");
	py_marshal_add_key_sval(&sb, "state", "Tennesse");
	py_marshal_add_key_ival(&sb, "population", 692587);
	py_marshal_gen_stop_dict(&sb);
	write_in_full(STDOUT_FILENO, sb.buf, sb.len);
	strbuf_release(&sb);
	return 0;
}

static int in_marshal_1(void)
{
	int res = 1;
	struct hashmap map;
	struct hashmap *pres;
	size_t i;
	struct strbuf intbuf = STRBUF_INIT;
	str_dict_init(&map);
	pres = py_marshal_parse(&map, STDIN_FILENO);
	if (!pres)
		goto _err;
	for (i = 0; i < ARRAY_SIZE(list_of_key_svals); i++) {
		const char *key = list_of_key_svals[i].key;
		const char *val_expected = list_of_key_svals[i].val;
		const char *val_parsed = str_dict_get_value(pres, key);
		if (!val_parsed) {
			fprintf(stderr, "error getting value for key %s\n", key);
			goto _err;
		}
		if (strcmp(val_expected, val_parsed) != 0) {
			fprintf(stderr, "error validating key: %s\n", key);
			goto _err;
		}
	}
	for (i = 0; i < ARRAY_SIZE(list_of_key_ivals); i++) {
		strbuf_reset(&intbuf);
		const char *key = list_of_key_ivals[i].key;
		const char *val_parsed = str_dict_get_value(pres, key);
		strbuf_addf(&intbuf, "%d", list_of_key_ivals[i].val);
		if (!val_parsed) {
			fprintf(stderr, "error getting value for key %s\n", key);
			goto _err;
		}
		if (strcmp(intbuf.buf, val_parsed) != 0) {
			fprintf(stderr, "error validating key: %s\n", key);
			goto _err;
		}
	}
	res = 0;
_err:
	strbuf_release(&intbuf);
	str_dict_destroy(&map);
	return res;
}

int cmd_main(int argc, const char **argv)
{
	if (argc < 2)
		return 1;
	if (strcmp(argv[1], "out_marshal_1") == 0) {
		return out_marshal_1();
	}
	if (strcmp(argv[1], "out_marshal_2") == 0) {
		return out_marshal_2();
	}
	if (strcmp(argv[1], "in_marshal_1") == 0) {
		return in_marshal_1();
	}
	return 1;

}
