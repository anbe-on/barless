CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PKGS    := gtk+-3.0 gtk-layer-shell-0
SRC     := main.c common.c indicator.c card.c tray.c

barless: $(SRC) barless.h
	$(CC) $(CFLAGS) $(SRC) -o $@ $$(pkg-config --cflags --libs $(PKGS)) -lm

# GNU install replaces the file, so this is safe while barless is running.
install: barless
	install -Dm755 barless $(HOME)/.local/bin/barless

clean:
	rm -f barless panelless

.PHONY: install clean
