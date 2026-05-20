CC ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
CFLAGS  += $(shell pkg-config --cflags x11 xcomposite xrender xfixes xft)
LDFLAGS += $(shell pkg-config --libs x11 xcomposite xrender xfixes xft) -lm

xexpose: xexpose.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f xexpose

install: xexpose
	install -Dm755 xexpose $(DESTDIR)/usr/bin/xexpose

.PHONY: clean install
