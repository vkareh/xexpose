CC ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
CFLAGS  += $(shell pkg-config --cflags x11 xcomposite xrender xfixes xft cairo cairo-xlib)
LDFLAGS += $(shell pkg-config --libs x11 xcomposite xrender xfixes xft cairo cairo-xlib) -lm

xexpose: xexpose.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f xexpose

install: xexpose
	install -Dm755 xexpose $(DESTDIR)/usr/bin/xexpose

.PHONY: clean install
