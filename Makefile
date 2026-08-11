include config.mk

SUBDIRS = obbar oblist

all:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d; done

clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done

install: all
	@for d in $(SUBDIRS); do $(MAKE) -C $$d install; done

uninstall:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d uninstall; done

.PHONY: all clean install uninstall
