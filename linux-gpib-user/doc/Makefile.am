# doc/Makefile.am
# copyright (C) 2003 by Frank Mori Hess
# email : fmhess@users.sourceforge.net
#
#   This Makefile.am is free software; you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation; either version 2 of the License, or
#   (at your option) any later version.

docs = linux-gpib.pdf

SGML = linux-gpib.sgml gpib_version.txt

EXTRA_DIST = linux-gpib.sgml fdl.xml gpib_version.txt $(docs) $(doc_DATA) $(manpages)


if BUILD_DOCS
all-local: $(docs)
else
all-local:
endif

if BUILD_HTML
doc_DATA = doc_html/*
else
doc_DATA =
endif

if BUILD_MANPAGES
man1_MANS = man/man1/*
man3_MANS = man/man3/*
man5_MANS = man/man5/*
man8_MANS = man/man8/*
manpages = man/man1/* man/man3/* man/man5/* man/man8/*
man/man1/* man/man3/* man/man5/* man/man8/*: man
man_stylesheet = $(DOCBOOK_XSL)
else
manpages =
endif

docdir = $(datadir)/doc/@PACKAGE@/html

maintainer-clean-local:
	$(RM) gpib_version.txt
	$(RM) intEntities.dtf
	$(RM) -r $(docs) doc_html man

linux-gpib.pdf: $(SGML)
	if test `basename $(SGML2PDF_PATH)` = "jw" ; then \
		$(SGML2PDF_PATH) -f docbook -b pdf $(srcdir)/linux-gpib.sgml ; \
	else  \
		$(SGML2PDF_PATH) $(srcdir)/linux-gpib.sgml -F sgml -P table.in.float=none -o linux-gpib.pdf ; \
	fi

doc_html/*: $(SGML)
	{ $(JW_PATH) -f docbook -b html -o doc_html -V '%use-id-as-filename%' $(srcdir)/linux-gpib.sgml; } && { touch doc_html/* ; }

man: $(SGML)
	{ $(SGML2XML_PATH) -x no-expand-internal -x no-internal-decl -x preserve-case  $(srcdir)/linux-gpib.sgml | \
	$(XSLTPROC_PATH) --param man.authors.section.enabled 0 --param man.output.in.separate.dir 1 \
	--xinclude $(man_stylesheet) -; } && { touch man; }

gpib_version.txt: 
	if [ ! -f gpib_version.txt ] || [ `echo $(VERSION)` != `cat gpib_version.txt` ]; then \
		echo $(VERSION) > gpib_version.txt; \
	fi
