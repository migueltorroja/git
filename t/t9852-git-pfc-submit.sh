#!/bin/sh

test_description='basic git pfc tests'

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


test_expect_success 'start p4d' '
	start_p4d
'

test_expect_success 'add p4 files' '
	(
		cd "$cli" &&
		echo file1 >file1 &&
		p4 add file1 &&
		p4 submit -d "file1" &&
		echo file2 >file2 &&
		p4 add file2 &&
		p4 submit -d "file2"
	)
'

test_expect_success 'basic git pfc submit' '
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		git update-ref -d refs/remotes/p4/HEAD &&
		git update-ref -d refs/remotes/p4/master &&
		echo "All work and no play makes Jack a dull boy" > chapter1.txt &&
		git add chapter1.txt &&
		git commit -m "Shiny commit" &&
		GIT_DIR="$git/.git" git pfc submit &&
		git update-ref refs/remotes/p4/master HEAD~1 &&
		git p4 sync &&
		git diff HEAD..p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc fsck' '
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		number_of_git_files=`git ls-files | wc -l` &&
		git pfc fsck HEAD~1..HEAD >git_fsck_result.txt &&
		grep -e "^Total checked: $number_of_git_files failed 0" git_fsck_result.txt
	)
'

test_expect_success 'git pfc fsck fail' '
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "\nA new line at ened\n" >> file2 &&
		git add file2 && git commit --amend -c HEAD &&
		number_of_git_files=`git ls-files | wc -l` &&
		git pfc fsck HEAD~1..HEAD >git_fsck_result.txt &&
		grep -e "^Total checked: $number_of_git_files failed 1" git_fsck_result.txt
	)
'

test_expect_success 'git pfc fsck unicode' '
	(
		cd "$cli" &&
		output_utf8_text > text_utf8.txt &&
		p4 add -t utf8 text_utf8.txt &&
		output_utf16_text > text_utf16.txt &&
		p4 add -t utf16 text_utf16.txt &&
		output_utf8_bom_text > text_utf8_bom.txt &&
		p4 add -t utf8 text_utf8_bom.txt &&
		p4 submit -d "some unicode files"
	) &&
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		number_of_git_files=`git ls-files | wc -l` &&
		git pfc fsck HEAD~1..HEAD >git_fsck_result.txt &&
		grep -e "^Total checked: $number_of_git_files failed 0" git_fsck_result.txt
	)
'

test_done

