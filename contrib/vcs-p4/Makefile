all:: git-remote-p4$X git-p4$X

CC = cc
RM = rm -f
MV = mv

BASIC_INCLUDES = -I../../
CFLAGS = -g -O2 -Wall
CFLAGS += $(BASIC_INCLUDES)
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

git-remote-p4$X: remote-p4.o $(LIBS)
	$(QUIET_LINK)$(CC) $(CFLAGS) $(LDFLAGS) $(EXTLIBS) -o $@ remote-p4.o $(LIBS)

git-p4$X: p4.o $(LIBS)
	$(QUIET_LINK)$(CC) $(CFLAGS) $(LDFLAGS) $(EXTLIBS) -o $@ p4.o $(LIBS)
