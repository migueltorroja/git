#!/bin/sh

test_description='basic tests for testing the python marshall parser used in git-remote-p4'

. ./test-lib.sh

test_expect_success 'basic py marshal parsing' '
	test-py-marshal out_marshal_1 | test-py-marshal in_marshal_1 &&
	test-py-marshal out_marshal_2 | test_must_fail test-py-marshal in_marshal_1
'

test_expect_success 'basic strbuf dict' '
	test-py-marshal basic_strbuf_dict
'

test_done
