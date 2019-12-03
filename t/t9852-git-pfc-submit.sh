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
		printf "All work and no play makes Jack a dull boy\n" > chapter1.txt &&
		git add chapter1.txt &&
		git commit -m "Shiny commit" &&
		GIT_DIR="$git/.git" git pfc submit &&
		git update-ref refs/remotes/p4/master HEAD~1 &&
		git p4 sync &&
		git diff HEAD..p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_failure 'basic git pfc submit (no git dir)' '
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "All work and no play makes Jack a dull boy\n" >> chapter1.txt &&
		git add chapter1.txt &&
		git commit -m "A little bit more scary" &&
		git pfc submit &&
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

test_expect_success 'git pfc cherry-pick' '
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git && (
		cd "$cli"
		mkdir dir1 &&
		printf "file1" > dir1/file.txt &&
		printf "#! /bin/sh\n" > dir1/run.sh &&
		chmod 755 dir1/run.sh &&
		printf "int main (int argc, char **argv) { return 0;}" > dir1/main.c &&
		mkdir quijote &&
		output_utf8_text > quijote/chapter1.txt &&
		p4 add -t utf8 quijote/chapter1.txt &&
		p4 add dir1/file.txt dir1/run.sh dir1/main.c &&
		p4 submit -d "a miscellaneous set of files" &&
		p4 delete quijote/chapter1.txt &&
		p4 submit -d "deleting a file"
	) &&
	(
		cd "$git" &&
		git p4 sync &&
		last_cl=`extract_changelist_from_commit p4/master` &&
		prev_cl=`extract_changelist_from_commit p4/master~1` &&
		git pfc cherry-pick //depot/ "$prev_cl" &&
		git pfc cherry-pick //depot/ "$last_cl" &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt &&
		git reset --hard p4/master
	)

'
test_expect_success 'git pfc shelve' '
	test_when_finished cleanup_git &&
	(
		cd "$cli" &&
		output_shopping_list_v1 > shopping_list.txt &&
		p4 add shopping_list.txt && p4 submit -d "shopping list v1"
	) &&
	git p4 clone --dest="$git" //depot/@all &&
	(
		cd "$git" &&
		output_shopping_list_plus_Olive_oil > shopping_list.txt &&
		git add shopping_list.txt && git commit -m "Adding Olive oil" &&
		GIT_DIR="$git"/.git git pfc submit && git p4 sync &&
		git diff p4/master~1..p4/master &&
		git show p4/master:shopping_list.txt &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt &&
		git reset --hard p4/master
	) &&
	(
		cd "$git" &&
		git checkout -b shelve_branch HEAD~1 && output_shopping_list_plus_Eggs > shopping_list.txt &&
		git add shopping_list.txt && git commit -m "Adding Eggs" &&
		GIT_DIR="$git"/.git git pfc shelve && shelve_cl=`p4 changes -s pending -m1 | sed -e "s/[^ ]\+ \([0-9]\+\) .*/\1/"` &&
		p4 print -q -o shelve_shopping_list_v2.txt //depot/shopping_list.txt@="$shelve_cl" &&
		test_cmp shelve_shopping_list_v2.txt shopping_list.txt &&
		git checkout master && git pfc cherry-pick "$shelve_cl" &&
		output_shopping_list_plus_Eggs_Olive_oil > shopping_list_v4.txt &&
		test_cmp shopping_list_v4.txt shopping_list.txt
	)
'


test_expect_success 'git pfc submit new file' '
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "a new file" > new_file.txt &&
		git add new_file.txt && git commit -m "A new file commit" &&
		GIT_DIR="$git"/.git git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_failure 'git pfc new file twice' '
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "a new file" > new_file_1.txt &&
		git add new_file_1.txt && git commit -m "A new file 1 commit" &&
		GIT_DIR="$git"/.git git pfc submit &&
		test_must_fail git --git-dir="$git"/.git pfc submit
	)
'

test_expect_success 'git pfc submit binary' '
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "\377\277\277\277\000\000\000\000" > file.bin &&
		printf "\004\003\275\242\262\033\344\300" >> file.bin &&
		git add file.bin && git commit -m "A binary file" &&
		GIT_DIR="$git"/.git git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	) &&
	(
		cd "$git" &&
		git reset --hard p4/master &&
		printf "\377\277\277\277\000\000\000\000" >> file.bin &&
		printf "\004\003\275\242\262\033\344\300" >> file.bin &&
		git add file.bin && git commit -m "A bigger binary file" &&
		GIT_DIR="$git"/.git git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_failure 'git pfc submit new file with exec flag' '
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "#! /bin/sh\n" > exec.sh &&
		chmod 755 exec.sh &&
		git add exec.sh && git commit -m "An exec script" &&
		GIT_DIR="$git"/.git git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_failure 'git pfc submit change mode' '
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "#! /bin/sh\n\necho Hello\n" > script.sh &&
		chmod 644 script.sh &&
		git add script.sh && git commit -m "A script" &&
		chmod 755 script.sh &&
		git add script.sh && git commit -m "Set exec flag" &&
		GIT_DIR="$git"/.git git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc submit deleted file' '
	(
		cd "$cli" &&
		printf "File about to be deleted" >temporal_file.txt &&
		p4 add temporal_file.txt &&
		p4 submit -d "Temporal file"
	)&&
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		git rm temporal_file.txt &&
		git commit -m "Short is the life of a temporal file" &&
		GIT_DIR="$git"/.git git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc submit readd file' '
	(
		cd "$cli" &&
		printf "File about to be deleted" >temporal_file_1.txt &&
		p4 add temporal_file_1.txt &&
		p4 submit -d "Temporal file"
	)&&
	git p4 clone --dest="$git" //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		git rm temporal_file_1.txt &&
		git commit -m "Short is the life of a temporal file" &&
		printf "A not so temporal file" >temporal_file_1.txt &&
		git add temporal_file_1.txt &&
		git commit -m "Readding temporal file" &&
		GIT_DIR="$git"/.git git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_failure 'git pfc fetch' '
	git p4 clone --dest="$git" --max-changes=1 //depot/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		git pfc fetch &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'


test_done

