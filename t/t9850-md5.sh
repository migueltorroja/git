#!/bin/sh

test_description='basic tests for the md5 checksum used by vcs-p4'

. ./test-lib.sh

test_expect_success 'md5 basic strings' '
	test-md5 </dev/null >actual &&
	grep d41d8cd98f00b204e9800998ecf8427e actual &&
	echo -n "a" | test-md5 >actual &&
	grep 0cc175b9c0f1b6a831c399e269772661 actual &&
	echo -n "abcdefghijklmnopqrstuvwxyz" | test-md5 >actual &&
	grep c3fcd3d76192e4007dfb496cca67e13b actual &&
	echo -n "some random test" | test-md5 </dev/null >actual &&
	test_must_fail grep 0123456789abcdefghijklmnopqrstuv actual
'


test_done
