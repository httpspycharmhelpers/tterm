CC      ?= cc
CFLAGS  ?= -O2 -std=c11
CFLAGS  += -Wall -Wextra
PKG      = freetype2 libpng
CFLAGS  += $(shell pkg-config --cflags $(PKG))
LDLIBS  += $(shell pkg-config --libs $(PKG)) -lm

PREFIX  ?= /data/data/com.termux/files/usr
CFLAGS  += -DPREFIX_FONT=\"$(PREFIX)/share/fonts/TTF\"

BIN = tterm
SRC = src/tterm.c

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(BIN)

install: $(BIN)
	install -Dm755 $(BIN) $(PREFIX)/bin/tterm

.PHONY: all clean install
