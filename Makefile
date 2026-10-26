PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
DESTDIR ?=

PKG_CONFIG ?= pkg-config
PKGS = gtk4 gio-unix-2.0

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -D_DEFAULT_SOURCE
PKG_CFLAGS != $(PKG_CONFIG) --cflags $(PKGS)
PKG_LIBS != $(PKG_CONFIG) --libs $(PKGS)

OBJS = src/main.o src/window.o src/dir.o src/file.o src/thumbs.o src/places.o src/ops.o src/settings.o src/dnd.o

all: howl

howl: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(PKG_LIBS)

$(OBJS): src/howl.h

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PKG_CFLAGS) -c -o $@ $<

install: howl
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(DATADIR)/applications
	install -m 755 howl $(DESTDIR)$(BINDIR)/howl
	install -m 644 howl.desktop $(DESTDIR)$(DATADIR)/applications/howl.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/howl $(DESTDIR)$(DATADIR)/applications/howl.desktop

clean:
	rm -f howl $(OBJS)

.PHONY: all install uninstall clean
