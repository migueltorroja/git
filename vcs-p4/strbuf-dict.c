#include "git-compat-util.h"
#include "strbuf-dict.h"

static ssize_t strbuf_read_in_full(struct strbuf *sb, int fd, size_t size);
static int keyval_cmp(const void *userdata,
		const struct hashmap_entry *entry,
		const struct hashmap_entry *entry_or_key,
		const void *keydata);

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

void keyval_append_key_f(keyval_t *kw, int fd, size_t n)
{
	if (!n)
		return;
	if (strbuf_read_in_full(&kw->key, fd, n) <= 0)
		die("Error appending key reading fd:%d size:%d", fd, (unsigned int) n);
}

void keyval_append_val_f(keyval_t *kw, int fd, size_t n)
{
	if (!n)
		return;
	if (strbuf_read_in_full(&kw->val, fd, n) <= 0)
		die("Error appending val reading fd:%d size:%d", fd, (unsigned int) n);
}

void keyval_print(FILE *fp, keyval_t *kw)
{
	size_t i;
	if (NULL == fp)
		fp = stdout;
	fprintf(fp,"'%.*s' (len:%"PRIuMAX"): ", (int)kw->key.len, kw->key.buf, kw->val.len);
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

void keyval_copy(keyval_t *dst, const keyval_t *src)
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

void str_dict_init(struct hashmap *map)
{
	hashmap_init(map, keyval_cmp, NULL, 0);
}

void str_dict_destroy(struct hashmap *map)
{
	struct hashmap_iter hm_iter;
	struct keyval_t *kv;
	hashmap_iter_init(map, &hm_iter);
	while ((kv = str_dict_iter_next(&hm_iter)))
		keyval_release(kv);
	hashmap_free(map);
}


void str_dict_reset(struct hashmap *map)
{
	str_dict_destroy(map);
	str_dict_init(map);
}

void str_dict_put_kw(struct hashmap *map, keyval_t *kw)
{
	struct hashmap_entry *prev_entry = NULL;
	hashmap_entry_init(&kw->ent, strhash(kw->key.buf));
	while ((prev_entry = hashmap_remove(map, &kw->ent, kw->key.buf)))
		keyval_release(container_of(prev_entry, keyval_t, ent));
	hashmap_put(map, &kw->ent);
}

void str_dict_set_key_valf(struct hashmap *map, const char *key, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	keyval_t *kw = keyval_init(NULL);
	strbuf_addstr(&kw->key, key);
	strbuf_vaddf(&kw->val, fmt, ap);
	str_dict_put_kw(map,kw);
	va_end(ap);
}

void str_dict_set_key_val(struct hashmap *map, const char *key, const char *val)
{
	str_dict_set_key_valf(map, key, "%s", val);
}

const keyval_t *str_dict_get_kw(struct hashmap *map, const char *str)
{
	struct hashmap_entry *entry;
	entry = hashmap_get_from_hash(map, strhash(str), str);
	return container_of(entry, keyval_t, ent);
}

const char *str_dict_get_value(struct hashmap *map, const char *str)
{
	const keyval_t *kw;
	kw = str_dict_get_kw(map,str);
	if (NULL == kw)
		return NULL;
	return kw->val.buf;
}

const char *str_dict_get_valuef(struct hashmap *map, const char *fmt, ...)
{
	struct strbuf key = STRBUF_INIT;
	va_list ap;
	const char *r = NULL;
	va_start(ap, fmt);
	strbuf_vaddf(&key, fmt, ap);
	r = str_dict_get_value(map, key.buf);
	strbuf_release(&key);
	va_end(ap);
	return r;
}

void str_dict_print(FILE *fp, struct hashmap *map)
{
	if (NULL == fp)
		fp = stdout;
	if (hashmap_get_size(map))
	{
		fprintf(fp,"{");
		struct hashmap_iter hm_iter;
		struct keyval_t *kv;
		hashmap_iter_init(map, &hm_iter);
		while ((kv = str_dict_iter_next(&hm_iter))) {
			keyval_print(fp, kv);
			fprintf(fp,", ");
		}
		fprintf(fp,"}\n");
	}
}

void str_dict_copy_kw(struct hashmap *dst, const keyval_t *kw)
{
	keyval_t *copykw = keyval_init(NULL);
	keyval_copy(copykw, kw);
	str_dict_put_kw(dst, copykw);
}

void str_dict_copy(struct hashmap *dst, struct hashmap *src)
{
	str_dict_reset(dst);
	struct keyval_t *kv;
	struct hashmap_iter hm_iter;
	hashmap_iter_init(src, &hm_iter);
	while ((kv = str_dict_iter_next(&hm_iter))) {
		str_dict_copy_kw(dst, kv);
	}
}

int str_dict_strcmp(struct hashmap *map, const char *key, const char *valcmp)
{
	const char *val_str = str_dict_get_value(map, key);
	if (!val_str) {
		if (!valcmp)
			return 0;
		return 1;
	}
	else if (!valcmp)
		return 1;
	return strcmp(val_str, valcmp);
}

int str_dict_has_all(struct hashmap *map, const char **keys, size_t nkeys)
{
	const char **key_end = keys + nkeys;
	for (;keys != key_end; keys++) {
		if (NULL == str_dict_get_kw(map, *keys))
			return 0;
	}
	return nkeys > 0 ? 1:0;;
}

int str_dict_has(struct hashmap *map, const char *key)
{
	return str_dict_has_all(map, &key, 1);
}

static ssize_t strbuf_read_in_full(struct strbuf *sb, int fd, size_t size)
{
	ssize_t res;
	size_t oldalloc = sb->alloc;
	strbuf_grow(sb, size);
	res = read_in_full(fd, sb->buf + sb->len, size);
	if (res > 0)
		strbuf_setlen(sb, sb->len+res);
	else if (oldalloc == 0)
		strbuf_release(sb);
	return res;

}

static int keyval_cmp(const void *userdata,
		const struct hashmap_entry *entry,
		const struct hashmap_entry *entry_or_key,
		const void *keydata)
{
	const struct keyval_t *e1 = container_of(entry, const struct keyval_t, ent);
	const struct keyval_t *e2 = container_of(entry_or_key, const struct keyval_t, ent);
	return strcmp(e1->key.buf, keydata ? keydata : e2->key.buf);
}

