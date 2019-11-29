#include "git-compat-util.h"
#include "cache.h"
#include "strbuf.h"
#include "vcs-p4/strbuf-dict.h"
#include "vcs-p4/py-marshal.h"


#define PY_MARSHAL_TYPE_DICT_START '{'
#define PY_MARSHAL_TYPE_STRING     's'
#define PY_MARSHAL_TYPE_INT        'i'
#define PY_MARSHAL_TYPE_NULL       '0'


struct kw_pair {
	const char *key;
	const char *val_s;
	int32_t     val_i;
};

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


const struct kw_pair key_vals_test_1[] = {
	{"user", "John Smith", 0},
	{"town", "Springfield", 0},
	{"ext", NULL, 1234},
	{"phone", NULL, 55555555},
};

const struct kw_pair key_vals_test_2[] = {
	{"city", "Nashville", 0},
	{"state", "Tennesse", 0},
	{"population", 0, 692587},
};

static int out_marshal(int fd, const struct kw_pair *p_kv, size_t sz)
{
	struct strbuf sb = STRBUF_INIT;
	const struct kw_pair *p_end = p_kv + sz;
	py_marshal_gen_start_dict(&sb);
	for (; p_kv != p_end; p_kv++)
	{
		if (p_kv->val_s)
			py_marshal_add_key_sval(&sb, p_kv->key,
					p_kv->val_s);
		else
			py_marshal_add_key_ival(&sb, p_kv->key,
					p_kv->val_i);
	}
	py_marshal_gen_stop_dict(&sb);
	write_in_full(STDOUT_FILENO, sb.buf, sb.len);
	strbuf_release(&sb);
	return 0;
}

static int check_marshal_values(struct hashmap *map, const struct kw_pair *p_kv, size_t sz)
{
	const struct kw_pair *p_end = p_kv + sz;
	struct strbuf intbuf = STRBUF_INIT;
	int res = 1;
	for (; p_kv != p_end; p_kv++)
	{
		const char *key = p_kv->key;
		const char *val_parsed = str_dict_get_value(map, key);
		if (!val_parsed) {
			fprintf(stderr, "error getting value for key %s\n", key);
			goto _err;
		}
		if (p_kv->val_s) {
			if (strcmp(p_kv->val_s, val_parsed) != 0) {
				fprintf(stderr, "error validating key: %s\n", key);
				goto _err;
			}
		}
		else {
			strbuf_reset(&intbuf);
			strbuf_addf(&intbuf, "%d", p_kv->val_i);
			if (strcmp(intbuf.buf, val_parsed) != 0) {
				fprintf(stderr, "error validating key: %s\n", key);
				goto _err;
			}
		}
	}
	res = 0;
_err:
	strbuf_release(&intbuf);
	return res;
}

static int out_marshal_1(void)
{
	return out_marshal(STDOUT_FILENO, key_vals_test_1, ARRAY_SIZE(key_vals_test_1));
}

static int out_marshal_2(void)
{
	return out_marshal(STDOUT_FILENO, key_vals_test_2, ARRAY_SIZE(key_vals_test_2));
}

static int in_marshal_1(void)
{
	int res = 1;
	struct hashmap map;
	str_dict_init(&map);
	if (py_marshal_parse(&map, STDIN_FILENO) == NULL)
		goto _err;
	res = check_marshal_values(&map, key_vals_test_1, ARRAY_SIZE(key_vals_test_1));
_err:
	str_dict_destroy(&map);
	return res;
}

static int basic_strbuf_dict()
{
	int res = 1;
	struct hashmap map;
	str_dict_init(&map);
	str_dict_set_key_val(&map, "city", "Paris");
	if (!str_dict_has(&map, "city"))
		goto _err;
	if (!str_dict_get_value(&map, "city"))
		goto _err;
	if (strcmp("Paris", str_dict_get_value(&map, "city")) != 0)
		goto _err;
	str_dict_reset(&map);
	if (str_dict_get_value(&map, "city"))
		goto _err;
	res = 0;
_err:
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
	if (strcmp(argv[1], "basic_strbuf_dict") == 0) {
		return basic_strbuf_dict();
	}
	return 1;
}
