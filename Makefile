TOPDIR = .

include $(TOPDIR)/Preamble.make

SUBDIRS = src

DIST_FILES = \
	COPYING		\
	Config.make.in	\
	Preamble.make	\
	Postamble.make	\
	config.guess	\
	config.sub	\
	configure	\
	configure.in	\
	install-sh	\
	mkinstalldirs

.PHONY: dist dist-bye dist-fresh distclean

dist-bye:
	-rm -rf $(DIST_TOPDIR)

dist-fresh: dist-bye
	$(TOPDIR)/mkinstalldirs $(DIST_TOPDIR)

dist: dist-fresh dist-all
	GZIP=$(GZIP_OPTS) tar chozf $(DIST_TOPDIR).tar.gz $(DIST_TOPDIR)
	$(MAKE) dist-bye

include $(TOPDIR)/Postamble.make
