TOPDIR = ..

include $(TOPDIR)/Preamble.make

BIN_PROGRAMS = pstack-gdb

INCLUDES = $(GLIB_CFLAGS)

DIST_FILES = pstack-gdb.c

DEFINES = -DPSTACK_VERSION=\"$(VERSION)\"

pstack-gdb: pstack-gdb.o
	$(LINK) $(GLIB_LIBS)

include $(TOPDIR)/Postamble.make
