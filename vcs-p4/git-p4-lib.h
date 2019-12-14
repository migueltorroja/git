#ifndef GIT_P4_LIB_H_
#define GIT_P4_LIB_H_

int cmd_git_pfc(int argc, const char **argv);

int p4_fetch_refs(const char *ref_prefix);
int p4_fetch_update_ref(int fd_out, const char *ref, const char *prev_commit, const char *depot_path, int start_changelist);
#endif
