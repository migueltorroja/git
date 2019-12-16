#!/bin/sh

test_description='basic git remote p4 tests'

. ./lib-git-p4.sh

output_utf8_text() {
printf "CAP\303\215TULO PRIMERO\n\n"
printf "Que trata de la condici\303\263n y ejercicio del famoso y valiente "
printf "hidalgo don Quijote de la Mancha\n\n"
printf "En un lugar de la Mancha, de cuyo nombre no quiero acordarme, no ha "
printf "mucho tiempo que viv\303\255a un hidalgo de los de lanza en "
printf "astillero, adarga antigua, roc\303\255n flaco y galgo corredor. Una "
printf "olla de algo m\303\241s vaca que carnero, salpic\303\263n las "
printf "m\303\241s noches, duelos y quebrantos los s\303\241bados, lentejas "
printf "los viernes, alg\303\272n palomino de a\303\261adidura los domingos, "
printf "consum\303\255an las tres partes de su hacienda."
}

output_utf8_bom_text() {
	printf "\357\273\277"
	output_utf8_text
}

output_utf16_text() {
	output_utf8_text | iconv -f UTF-8 -t UTF-16
}

output_shopping_list_v1() {
cat << EOF
Bread
Carrots
Orange juice
Milk
Mustard
Toothpaste
Coffee filters
EOF
}

output_shopping_list_plus_Eggs() {
cat << EOF
Bread
Carrots
Orange juice
Milk
Eggs
Mustard
Toothpaste
Coffee filters
EOF
}

output_shopping_list_plus_Olive_oil() {
cat << EOF
Bread
Carrots
Orange juice
Milk
Mustard
Toothpaste
Coffee filters
Olive oil
EOF
}

output_shopping_list_plus_Eggs_Olive_oil() {
cat << EOF
Bread
Carrots
Orange juice
Milk
Eggs
Mustard
Toothpaste
Coffee filters
Olive oil
EOF
}


extract_changelist_from_commit() {
	git log -1 $1 | sed -ne 's/.*git-p4.*change = \([0-9]\+\)[^0-9]/\1/p'
}

extract_depotpath_from_commit() {
	git log -1 $1 | sed -ne 's/.*git-p4.*depot-paths[^=]*=[^=]"\([^"]\+\)".*/\1/p'
}

test_expect_success 'start p4d' '
	start_p4d
'

test_expect_success 'add p4 files' '
	(
		mkdir "$cli"/mainbranch &&
		cd "$cli"/mainbranch &&
		echo file1 >file1 &&
		p4 add file1 &&
		p4 submit -d "file1" &&
		echo file2 >file2 &&
		p4 add file2 &&
		p4 submit -d "file2"
	)
'

test_expect_success 'git remote p4 clone' '
	git clone p4://depot/mainbranch/ "$git" &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		git pfc fsck origin/master~1..origin/master &&
		git show-ref
	)
'

test_expect_success 'git remote p4 fetch' '
	git clone p4://depot/mainbranch/ "$git" &&
	test_when_finished cleanup_git &&
	(
		cd "$cli"/mainbranch &&
		p4 edit file1 &&
		printf "One more line\n" >>file1 &&
		p4 submit -d "one more line" file1
	) &&
	(
		cd "$git" &&
		git fetch &&
		cl=`p4 changes -m1 | sed -e "s/[^ ]\+ \([0-9]\+\) .*/\1/"` &&
		last_fetch_cl=`extract_changelist_from_commit origin/master` &&
		test "$cl" -eq "$last_fetch_cl" &&
		git pfc fsck origin/master~1..origin/master
	)
'

test_done

