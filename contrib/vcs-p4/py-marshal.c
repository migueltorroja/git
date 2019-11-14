#include "git-compat-util.h"
#include "strbuf-dict.h"
#include "py-marshal.h"

#define PY_MARSHAL_TYPE_DICT    '{'
#define PY_MARSHAL_TYPE_STRING  's'
#define PY_MARSHAL_TYPE_INT     'i'
#define PY_MARSHAL_TYPE_NULL    '0'
#define PY_MARSHAL_TYPE_EOF     'E'

static int read_int32_t(int fd, int32_t *v)
{
	*v = 0;
	uint8_t bytes[4];
	ssize_t nr;
	nr = read_in_full(fd, bytes, sizeof(bytes));
	if (nr < sizeof(bytes)) 
		die("Error reading long\n");
	*v  = bytes[0];
	*v |= bytes[1] << 8;
	*v |= bytes[2] << 16;
	*v |= bytes[3] << 24;
	return 0;
}

struct hashmap *py_marshal_parse(struct hashmap *map, int fd)
{
#define PY_MARSHAL_WAIT_FOR_KEY     (0)
#define PY_MARSHAL_WAIT_FOR_VAL     (1)
	int32_t vi32,len;
	char c;
	ssize_t nr;
	int state = PY_MARSHAL_WAIT_FOR_KEY;
	keyval_t *kw = NULL;
	struct hashmap *mapres = NULL;

	for (;;)
	{
		nr = read_in_full(fd, &c, sizeof(c));
		if (nr < 0)
			die("Error reading from fd: %d", fd);
		if (nr == 0) {
				assert(NULL == kw);
				assert(NULL == mapres);
				goto _leave;
		}
		switch(c)
		{
			case PY_MARSHAL_TYPE_STRING:
				read_int32_t(fd,&len);
				if (state == PY_MARSHAL_WAIT_FOR_KEY)
				{
					kw = keyval_init(NULL);
					keyval_append_key_f(kw,fd, len);
					state = PY_MARSHAL_WAIT_FOR_VAL;
				}
				else
				{
					keyval_append_val_f(kw,fd, len);
					str_dict_put_kw(mapres, kw);
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
				read_int32_t(fd,&vi32);
				strbuf_addf(&kw->val,"%d",vi32);
				str_dict_put_kw(mapres, kw);
				kw = NULL;
				state = PY_MARSHAL_WAIT_FOR_KEY;
				break;
			case PY_MARSHAL_TYPE_NULL:
				assert(NULL == kw);
				assert(NULL != mapres);
				if (!str_dict_strcmp(mapres, "code", "error")) {
					const char *data_str = str_dict_get_value(mapres, "data");
					fprintf(stderr, "%s", data_str);
				}
				return mapres;
				break;
			case PY_MARSHAL_TYPE_DICT:
				// Do Nothing
				assert(NULL == mapres); 
				mapres = map;
				str_dict_reset(mapres);
				break;
			default:
				die("Not supported\n");
				goto _leave;
		}
	}
	assert(NULL == kw);
_leave:
	return NULL;
}

