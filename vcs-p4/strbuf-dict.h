#ifndef STRBUF_DICT_H
#define STRBUF_DICT_H
#include "strbuf.h"
#include "hashmap.h"

typedef struct keyval_t{
	struct hashmap_entry ent;
	int    self_alloc;
	struct strbuf key;
	struct strbuf val;
} keyval_t;

keyval_t *keyval_init(keyval_t *kw);
void keyval_copy(keyval_t *dst, const keyval_t *src);
void keyval_print(FILE *fp, keyval_t *kw);
void keyval_release(keyval_t *kw);
void keyval_append_key_f(keyval_t *kw, int fd, size_t n);
void keyval_append_val_f(keyval_t *kw, int fd, size_t n);

void str_dict_init(struct hashmap *map);
static inline keyval_t *str_dict_iter_next(struct hashmap_iter *hm_iter) {
	return container_of_or_null(hashmap_iter_next(hm_iter), keyval_t, ent);
}
void str_dict_destroy(struct hashmap *map);
void str_dict_reset(struct hashmap *map);
void str_dict_put_kw(struct hashmap *map, keyval_t *kw);
__attribute__((format (printf,3,4))) void str_dict_set_key_valf(struct hashmap *map, const char *key, const char *fmt, ...);
void str_dict_set_key_val(struct hashmap *map, const char *key, const char *val);
const keyval_t *str_dict_get_kw(struct hashmap *map, const char *str);
const char *str_dict_get_value(struct hashmap *map, const char *str);
__attribute__((format (printf,2,3))) const char *str_dict_get_valuef(struct hashmap *map, const char *fmt, ...);
void str_dict_print(FILE *fp, struct hashmap *map);
void str_dict_copy_kw(struct hashmap *dst, const keyval_t *kw);
void str_dict_copy(struct hashmap *dst, struct hashmap *src);
int str_dict_strcmp(struct hashmap *map, const char *key, const char *valcmp);
int str_dict_has_all(struct hashmap *map, const char **keys, size_t nkeys);
int str_dict_has(struct hashmap *map, const char *key);
void str_dict_remove_key(struct hashmap *map, const char *key);
#endif
