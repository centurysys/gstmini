CC ?= gcc
AR ?= ar
PKG_CONFIG ?= pkg-config

PREFIX ?= /usr/local

CFLAGS ?= -O2 -Wall -Wextra -fPIC
CPPFLAGS += -Iinclude $(shell $(PKG_CONFIG) --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

SRC := src/gstmini.c
OBJ := build/gstmini.o
STATIC_LIB := build/libgstmini.a
SHARED_LIB := build/libgstmini.so

.PHONY: all clean install examples

all: $(STATIC_LIB) $(SHARED_LIB)

build:
	mkdir -p build

$(OBJ): $(SRC) include/gstmini.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(OBJ)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(OBJ)
	$(CC) -shared -o $@ $^ $(LDLIBS)

examples: all
	$(CC) $(CPPFLAGS) $(CFLAGS) examples/pull_view.c -Lbuild -lgstmini $(LDLIBS) -Wl,-rpath,'$$ORIGIN/../build' -o build/gstmini-pull-view

install: all
	install -d $(DESTDIR)$(PREFIX)/include
	install -d $(DESTDIR)$(PREFIX)/lib
	install -m 0644 include/gstmini.h $(DESTDIR)$(PREFIX)/include/gstmini.h
	install -m 0644 $(STATIC_LIB) $(DESTDIR)$(PREFIX)/lib/libgstmini.a
	install -m 0755 $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/libgstmini.so

clean:
	rm -rf build
