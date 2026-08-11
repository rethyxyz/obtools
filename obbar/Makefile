include ../config.mk

BIN = obbar
SRC = obbar.c
OBJ = $(SRC:.c=.o)

all: $(BIN)

config.h:
	cp config.def.h $@

$(OBJ): config.h ../config.mk

$(BIN): $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -f $(BIN) $(OBJ)

install: all
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

.PHONY: all clean install uninstall
