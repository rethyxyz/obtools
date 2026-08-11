# obtools - shared build config

VERSION = 0.1
PREFIX = /usr/local

INCS = `pkg-config --cflags x11 xft imlib2 fontconfig xrandr`
LIBS = `pkg-config --libs x11 xft imlib2 fontconfig xrandr` -lm

CFLAGS = -std=c99 -pedantic -Wall -Wextra -Os $(INCS) -DVERSION=\"$(VERSION)\"
LDFLAGS = $(LIBS)

CC = cc
