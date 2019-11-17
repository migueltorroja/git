all:: git-remote-p4$X git-p4$X test-md5$X

CC = cc
RM = rm -f
MV = mv

GIT_INCLUDES = -I../../
CFLAGS = -g -O2 -Wall
CPPFLAGS = $(GIT_INCLUDES)
LDFLAGS =
EXTLIBS = -lz

include ../../config.mak.uname
-include ../../config.mak.autogen
-include ../../config.mak

GIT_LIB = ../../libgit.a
XDIFF_LIB = ../../xdiff/lib.a
COMMON_MAIN = ../../common-main.o

QUIET_SUBDIR0 = +$(MAKE) -C # space to separate -C and subdir
QUIET_SUBDIR1 =

ifndef NO_PTHREADS
	EXTLIBS += -lpthread
endif


ifneq ($(findstring $(MAKEFLAGS),s),s)
ifndef V
	QUIET_CC      = @echo '   ' CC $@;
	QUIET_LINK    = @echo '   ' LINK $@;
	QUIET_SUBDIR0 = +@subdir=
	QUIET_SUBDIR1 = ;$(NO_SUBDIR) echo '   ' SUBDIR $$subdir; \
	                $(MAKE) $(PRINT_DIR) -C $$subdir
endif
endif

LIBS = $(GIT_LIB) $(XDIFF_LIB) $(COMMON_MAIN)
P4LIB_OBJS = py-marshal.o strbuf-dict.o
REMOTE_P4_OBJS = remote-p4.o
GIT_P4_OBJS = p4.o
TEST_MD5_OBJS = mddriver.o md5.o

OBJECTS = $(P4LIB_OBJS) $(REMOTE_P4_OBJS) $(GIT_P4_OBJS) $(TEST_MD5_OBJS)

dep_check = $(shell $(CC) $(CFLAGS) $(CPPFLAGS)\
	-c -MF /dev/null -MQ /dev/null -MMD -MP \
	-x c /dev/null -o /dev/null 2>&1; \
	echo $$?)

dep_files := $(foreach f,$(OBJECTS),$(dir $f).depend/$(notdir $f).d)
dep_dirs := $(addsuffix .depend,$(sort $(dir $(OBJECTS))))

ifeq ($(dep_check),0)
missing_dep_dirs := $(filter-out $(wildcard $(dep_dirs)),$(dep_dirs))
dep_file = $(dir $@).depend/$(notdir $@).d
dep_args = -MF $(dep_file) -MQ $@ -MMD -MP
dep_files_present := $(wildcard $(dep_files))
ifneq ($(dep_files_present),)
include $(dep_files_present)
endif
else
dep_dirs =
missing_dep_dirs =
dep_args =
endif

%.o:%.c $(missing_dep_dirs)
	$(QUIET_CC)$(CC) -o $*.o -c $(dep_args) $(CFLAGS) $(CPPFLAGS) $<


git-remote-p4$X: $(REMOTE_P4_OBJS) $(P4LIB_OBJS) $(LIBS)
	$(QUIET_LINK)$(CC) $(CFLAGS) $(LDFLAGS) $(EXTLIBS) -o $@ $(REMOTE_P4_OBJS) $(LIBS) $(P4LIB_OBJS)

git-p4$X: $(GIT_P4_OBJS) $(P4LIB_OBJS) $(LIBS)
	$(QUIET_LINK)$(CC) $(CFLAGS) $(LDFLAGS) $(EXTLIBS) -o $@ $(GIT_P4_OBJS) $(LIBS) $(P4LIB_OBJS)

test-md5$X: $(TEST_MD5_OBJS)
	$(QUIET_LINK)$(CC) $(CFLAGS) $(LDFLAGS) $(EXTLIBS) -o $@ $^

clean:
	$(RM) -fr $(OBJECTS) $(dep_dirs)
	$(RM) -f git-remote-p4$X
	$(RM) -f git-p4$X
	$(RM) -f test-md5$X
