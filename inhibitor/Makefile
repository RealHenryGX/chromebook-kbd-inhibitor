CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra

LIBEVDEV_CFLAGS := $(shell $(PKG_CONFIG) --cflags libevdev)
LIBEVDEV_LIBS := $(shell $(PKG_CONFIG) --libs libevdev)

.PHONY: all clean

all: tablet-inhibitd

tablet-inhibitd: tablet-inhibitd.c
	$(CC) $(CPPFLAGS) $(LIBEVDEV_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBEVDEV_LIBS) $(LDLIBS)

clean:
	rm -f tablet-inhibitd
