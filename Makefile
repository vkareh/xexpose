PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man/man1

CC ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
CFLAGS  += $(shell pkg-config --cflags x11 xcomposite xrender xfixes xft xinerama cairo cairo-xlib)
LDFLAGS += $(shell pkg-config --libs x11 xcomposite xrender xfixes xft xinerama cairo cairo-xlib) -lm

all: xexpose

xexpose: xexpose.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f xexpose

install: xexpose
	install -Dm755 xexpose $(DESTDIR)$(BINDIR)/xexpose
	install -Dm644 xexpose.1 $(DESTDIR)$(MANDIR)/xexpose.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/xexpose
	rm -f $(DESTDIR)$(MANDIR)/xexpose.1

help:
	@echo "Targets:"
	@echo "  all       Build xexpose (default)"
	@echo "  clean     Remove build artifacts"
	@echo "  install   Install xexpose to $(DESTDIR)$(BINDIR)"
	@echo "  uninstall Remove xexpose from $(DESTDIR)$(BINDIR)"
	@echo ""
	@echo "Variables:"
	@echo "  PREFIX=$(PREFIX)  BINDIR=$(BINDIR)  MANDIR=$(MANDIR)"

.PHONY: all clean install uninstall help
