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

test_expect_success 'basic git pfc submit' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "All work and no play makes Jack a dull boy\n" > chapter1.txt &&
		git add chapter1.txt &&
		git commit -m "Shiny commit" &&
		git pfc submit &&
		git update-ref refs/remotes/p4/master HEAD~1 &&
		git p4 sync &&
		git diff HEAD..p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc fsck' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		number_of_git_files=`git ls-files | wc -l` &&
		git pfc fsck HEAD~1..HEAD >git_fsck_result.txt &&
		grep -e "^Total checked: $number_of_git_files failed 0" git_fsck_result.txt
	)
'

test_expect_success 'git pfc fsck fail' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "\nA new line at ened\n" >> file2 &&
		git add file2 && git commit --amend -c HEAD &&
		number_of_git_files=`git ls-files | wc -l` &&
		test_must_fail git pfc fsck HEAD~1..HEAD >git_fsck_result.txt &&
		grep -e "^Total checked: $number_of_git_files failed 1" git_fsck_result.txt
	)
'

test_expect_success 'git pfc fsck unicode' '
	(
		cd "$cli"/mainbranch &&
		output_utf8_text > text_utf8.txt &&
		p4 add -t utf8 text_utf8.txt &&
		output_utf16_text > text_utf16.txt &&
		p4 add -t utf16 text_utf16.txt &&
		output_utf8_bom_text > text_utf8_bom.txt &&
		p4 add -t utf8 text_utf8_bom.txt &&
		p4 submit -d "some unicode files"
	) &&
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		number_of_git_files=`git ls-files | wc -l` &&
		git pfc fsck HEAD~1..HEAD >git_fsck_result.txt &&
		grep -e "^Total checked: $number_of_git_files failed 0" git_fsck_result.txt
	)
'

test_expect_success 'git pfc cherry-pick' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git && (
		cd "$cli"/mainbranch
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
		git pfc cherry-pick //depot/mainbranch/ "$prev_cl" &&
		git pfc cherry-pick //depot/mainbranch/ "$last_cl" &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt &&
		git reset --hard p4/master
	)
'

test_expect_success 'git pfc cherry-pick unicode+x' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$cli"/mainbranch &&
		output_utf16_text > xtext_utf16.txt &&
		p4 add -t utf16+x xtext_utf16.txt &&
		p4 submit -d "Submit a utf16 file with exec flag"
	) &&
	(
		cd "$git" &&
		git p4 sync &&
		last_cl=`extract_changelist_from_commit p4/master` &&
		git pfc cherry-pick //depot/mainbranch/ "$last_cl" &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc shelve' '
	test_when_finished cleanup_git &&
	(
		cd "$cli"/mainbranch &&
		output_shopping_list_v1 > shopping_list.txt &&
		p4 add shopping_list.txt && p4 submit -d "shopping list v1"
	) &&
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	(
		cd "$git" &&
		output_shopping_list_plus_Olive_oil > shopping_list.txt &&
		git add shopping_list.txt && git commit -m "Adding Olive oil" &&
		git pfc submit && git p4 sync &&
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
		git pfc shelve && shelve_cl=`p4 changes -s pending -m1 | sed -e "s/[^ ]\+ \([0-9]\+\) .*/\1/"` &&
		p4 print -q -o shelve_shopping_list_v2.txt //depot/mainbranch/shopping_list.txt@="$shelve_cl" &&
		test_cmp shelve_shopping_list_v2.txt shopping_list.txt &&
		git checkout master && git pfc cherry-pick "$shelve_cl" &&
		output_shopping_list_plus_Eggs_Olive_oil > shopping_list_v4.txt &&
		test_cmp shopping_list_v4.txt shopping_list.txt
	)
'

test_expect_success 'git pfc shelve add' '
	test_when_finished cleanup_git &&
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	(
		cd "$git" &&
		output_shopping_list_v1 > shopping_list_to_be_shelved.txt &&
		git add shopping_list_to_be_shelved.txt &&
		git commit -m "A new file to be shelved" &&
		git pfc shelve &&
		shelve_cl=`p4 changes -s pending -m1 | sed -e "s/[^ ]\+ \([0-9]\+\) .*/\1/"` &&
		p4 print -q -o shopping_list_shelved.txt //depot/mainbranch/shopping_list_to_be_shelved.txt@="$shelve_cl" &&
		test_cmp shopping_list_shelved.txt shopping_list_to_be_shelved.txt
	)
'

test_expect_success 'git pfc cherry-pick a shelve with a deleted file' '
	test_when_finished cleanup_git &&
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	(
		cd "$git" &&
		git checkout -b shelve_branch &&
		git rm file1 &&
		git commit -m "A deleted file in a shelve " &&
		git pfc shelve &&
		git checkout master &&
		shelve_cl=`p4 changes -s pending -m1 | sed -e "s/[^ ]\+ \([0-9]\+\) .*/\1/"` &&
		git pfc cherry-pick "$shelve_cl" &&
		git diff master shelve_branch  >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc shelve add file in a new dir' '
	test_when_finished cleanup_git &&
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	(
		cd "$git" &&
		mkdir shelve &&
		output_shopping_list_v1 > shelve/shopping_list.txt &&
		git add shelve/shopping_list.txt &&
		git commit -m "A new file to be shelved" &&
		git pfc shelve &&
		shelve_cl=`p4 changes -s pending -m1 | sed -e "s/[^ ]\+ \([0-9]\+\) .*/\1/"` &&
		p4 print -q -o shelve/shopping_list_shelve.txt //depot/mainbranch/shelve/shopping_list.txt@="$shelve_cl" &&
		test_cmp shelve/shopping_list_shelve.txt shelve/shopping_list.txt
	)
'

test_expect_success 'git pfc shelve add file and format-patch' '
	test_when_finished cleanup_git &&
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	(
		cd "$git" &&
		git checkout -b shelve_branch &&
		mkdir shelve &&
		output_shopping_list_v1 > shelve/shopping_list.txt &&
		git add shelve/shopping_list.txt &&
		git commit -m "A new file to be shelved" &&
		git pfc shelve &&
		shelve_cl=`p4 changes -s pending -m1 | sed -e "s/[^ ]\+ \([0-9]\+\) .*/\1/"` &&
		git checkout master &&
		git pfc format-patch "$shelve_cl" &&
		git am 0001*patch &&
		git diff master shelve_branch  >diff.txt &&
		test_line_count = 0 diff.txt
	)
'


test_expect_success 'git pfc submit new file' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "a new file" > new_file.txt &&
		git add new_file.txt && git commit -m "A new file commit" &&
		git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc new file twice' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "a new file" > new_file_1.txt &&
		git add new_file_1.txt && git commit -m "A new file 1 commit" &&
		git pfc submit &&
		test_must_fail git pfc submit
	)
'

test_expect_success 'git pfc submit binary' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "\377\277\277\277\000\000\000\000" > file.bin &&
		printf "\004\003\275\242\262\033\344\300" >> file.bin &&
		git add file.bin && git commit -m "A binary file" &&
		git pfc submit &&
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
		git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc submit new file with exec flag' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "#! /bin/sh\n" > exec.sh &&
		chmod 755 exec.sh &&
		git add exec.sh && git commit -m "An exec script" &&
		git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc submit change mode' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "#! /bin/sh\n\necho Hello\n" > script.sh &&
		chmod 644 script.sh &&
		git add script.sh && git commit -m "A script" &&
		chmod 755 script.sh &&
		git add script.sh && git commit -m "Set exec flag" &&
		git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc submit contents change and change mode' '
	(
		cd "$cli"/mainbranch &&
		printf "#! /bin/sh\n" > HelloWorld.sh &&
		chmod 644 HelloWorld.sh &&
		p4 add HelloWorld.sh &&
		p4 submit -d "hello world scripts"
	) &&
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		printf "echo Hello World\n" >> HelloWorld.sh &&
		chmod 755 HelloWorld.sh &&
		git add HelloWorld.sh && git commit -m "Hello + 0755 mode" &&
		git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc submit from exec to non exec' '
	(
		cd "$cli"/mainbranch &&
		printf "#! /bin/sh\n" > AnotherScript.sh &&
		chmod 755 AnotherScript.sh &&
		p4 add AnotherScript.sh &&
		p4 submit -d "A very useful script"
	) &&
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		chmod 644 AnotherScript.sh &&
		git add AnotherScript.sh && git commit -m "from 0755 to 0644" &&
		git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc submit deleted file' '
	(
		cd "$cli"/mainbranch &&
		printf "File about to be deleted" >temporal_file.txt &&
		p4 add temporal_file.txt &&
		p4 submit -d "Temporal file"
	)&&
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		git rm temporal_file.txt &&
		git commit -m "Short is the life of a temporal file" &&
		git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc submit readd file' '
	(
		cd "$cli"/mainbranch &&
		printf "File about to be deleted" >temporal_file_1.txt &&
		p4 add temporal_file_1.txt &&
		p4 submit -d "Temporal file"
	)&&
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		git rm temporal_file_1.txt &&
		git commit -m "Short is the life of a temporal file" &&
		printf "A not so temporal file" >temporal_file_1.txt &&
		git add temporal_file_1.txt &&
		git commit -m "Readding temporal file" &&
		git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc submit renamed folder' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		mkdir -p oldname_dir &&
		output_shopping_list_v1 > oldname_dir/shopping_list.txt &&
		output_utf8_text > oldname_dir/chapter1.txt &&
		git add oldname_dir/shopping_list.txt &&
		git add oldname_dir/chapter1.txt &&
		git commit -m "old name dir" &&
		git mv oldname_dir renamed_dir &&
		git commit -m "Renamed dir" &&
		git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc cherry-pick symlink' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$cli"/mainbranch &&
		mkdir -p topdir/subdir1/subdir2/subdir3 &&
		printf "A message\n" > topdir/subdir1/subdir2/subdir3/sssfile.txt &&
		p4 add topdir/subdir1/subdir2/subdir3/sssfile.txt &&
		cd topdir &&
		ln -s subdir1/subdir2/subdir3/sssfile.txt linkedfile.txt &&
		p4 add linkedfile.txt &&
		p4 submit -d "A new symlink"
	) &&
	(
		cd "$git" &&
		cl=`p4 changes -m1 | sed -e "s/[^ ]\+ \([0-9]\+\) .*/\1/"` &&
		git pfc cherry-pick "$cl" &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc cherry-pick change stamp' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		cl=`p4 changes -m1 //depot/mainbranch/... | sed -e "s/[^ ]\+ \([0-9]\+\) .*/\1/"` &&
		git reset --hard HEAD~1 &&
		git diff HEAD p4/master >diff.txt &&
		test_must_fail test_line_count = 0 diff.txt &&
		git pfc cherry-pick "$cl" &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt &&
		git log -1 | grep "git-p4-cherry-pick.*//depot/mainbranch.*${cl}" &&
		git log -1 | grep -v "git-p4:.*depot-paths.*change = "
	)
'


test_expect_success 'git pfc submit symlink' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		mkdir -p topdir/subdir1/subdir2/subdir3 &&
		printf "A message\n" > topdir/subdir1/subdir2/subdir3/realfile.txt &&
		git add topdir/subdir1/subdir2/subdir3/realfile.txt &&
		cd topdir &&
		ln -s subdir1/subdir2/subdir3/realfile.txt tofile.txt &&
		git add tofile.txt &&
		git commit -m "A linked file" &&
		git pfc submit &&
		git p4 sync &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

test_expect_success 'git pfc discover branches' '
	(
		cd "cli"/mainbranch &&
		mkdir build &&
		printf "all:\n" > build/Makefile &&
		p4 add build/Makefile &&
		p4 submit -d "a new file in a new dir"
	) &&
	p4 integrate //depot/mainbranch/... //depot/branch1/... &&
	p4 submit -d "A new branch to submit" //depot/branch1/... &&
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		last_cl=`extract_changelist_from_commit p4/master` &&
		git pfc -ddd discover-branches //depot/.../build/Makefile@"$last_cl",#head &&
		git branch -r --list p4/doesnt_exist > n_branches.txt && test_line_count = 0 n_branches.txt &&
		git branch -r --list p4/branch1 > n_branches.txt && test_line_count = 1 n_branches.txt &&
		last_branch_cl=`extract_changelist_from_commit p4/branch1` &&
		prev_branch_cl=`extract_changelist_from_commit p4/branch1~1` &&
		test "$prev_branch_cl" -lt "$last_branch_cl" &&
		test `extract_depotpath_from_commit p4/branch1` = //depot/branch1/ &&
		test `extract_depotpath_from_commit p4/branch1~` = //depot/mainbranch/ &&
		test "$prev_branch_cl" -eq "$last_cl"
	)
'


test_expect_success 'git pfc fetch' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		last_cl=`extract_changelist_from_commit p4/master` &&
		prev_cl=`extract_changelist_from_commit p4/master~1` &&
		test $prev_cl -lt $last_cl &&
		git update-ref refs/remotes/p4/master HEAD~1 &&
		this_cl=`extract_changelist_from_commit p4/master` &&
		test $prev_cl -eq $this_cl &&
		git pfc fetch &&
		this_cl=`extract_changelist_from_commit p4/master` &&
		test $last_cl -eq $this_cl &&
		git pfc fsck p4/master~1..p4/master
	)
'

test_expect_success 'git pfc fetch after discover branches' '
	p4 integrate //depot/mainbranch/... //depot/otherbranches/branch2/... &&
	p4 submit -d "A new branch to submit" //depot/otherbranches/branch2/... &&
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		last_cl=`extract_changelist_from_commit p4/master` &&
		git pfc -ddd discover-branches //depot/.../build/Makefile@"$last_cl",#head &&
		git branch -r --list p4/doesnt_exist > n_branches.txt && test_line_count = 0 n_branches.txt &&
		git branch -r --list p4/otherbranches_branch2 > n_branches.txt && test_line_count = 1 n_branches.txt &&
		last_branch_cl=`extract_changelist_from_commit p4/otherbranches_branch2` &&
		prev_branch_cl=`extract_changelist_from_commit p4/otherbranches_branch2~1` &&
		test "$prev_branch_cl" -lt "$last_branch_cl" &&
		test `extract_depotpath_from_commit p4/otherbranches_branch2` = //depot/otherbranches/branch2/ &&
		test `extract_depotpath_from_commit p4/otherbranches_branch2~` = //depot/mainbranch/ &&
		test "$prev_branch_cl" -eq "$last_cl" &&
		git checkout otherbranches/branch2 &&
		printf "\nclean:\n" >build/Makefile &&
		git add build/Makefile &&
		git commit -m "A new target in the Makefile" &&
		git pfc submit &&
		git pfc fetch &&
		git diff HEAD p4/otherbranches_branch2 >diff.txt &&
		test_line_count = 0 diff.txt
	)
'

#test_expect_failure 'git pfc submit dir with previous file name' '
#	(
#		cd "$cli"/mainbranch &&
#		mkdir -p a_subdir &&
#		output_shopping_list_v1 > a_subdir/shopping_list &&
#		p4 add a_subdir/shopping_list &&
#		p4 submit -d "another shopping list"
#	) &&
#	git p4 clone --dest="$git" //depot/mainbranch/@all &&
#	test_when_finished cleanup_git &&
#	(
#		cd "$git" &&
#		git rm a_subdir/shopping_list &&
#		mkdir -p a_subdir/shopping_list &&
#		output_shopping_list_v1 > a_subdir/shopping_list/shopping_list.txt &&
#		git add a_subdir/shopping_list/shopping_list.txt &&
#		git commit -m "shopping list now in a another subdir" &&
#		git pfc submit &&
#		git p4 sync &&
#		git diff HEAD p4/master >diff.txt &&
#		test_line_count = 0 diff.txt
#	)
#'


test_expect_success 'git pfc fetch' '
	git p4 clone --dest="$git" //depot/mainbranch/@all &&
	test_when_finished cleanup_git &&
	(
		cd "$git" &&
		first_commit=`git rev-list --reverse p4/master | sed -ne "1,1p"` &&
		git update-ref refs/remotes/p4/master "$first_commit" &&
		git pfc fetch &&
		git diff HEAD p4/master >diff.txt &&
		test_line_count = 0 diff.txt
	)
'


test_done

