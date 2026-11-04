CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PKGS    := gtk+-3.0 gtk-layer-shell-0
SRC     := main.c common.c indicator.c dashboard.c tray.c

panelless: $(SRC) panelless.h
	$(CC) $(CFLAGS) $(SRC) -o $@ $$(pkg-config --cflags --libs $(PKGS))

# GNU install replaces the file, so this is safe while panelless is running.
install: panelless
	install -Dm755 panelless $(HOME)/.local/bin/panelless

clean:
	rm -f panelless

.PHONY: install clean
