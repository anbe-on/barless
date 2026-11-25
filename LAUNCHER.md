# Launcher prototype

    bind = SUPER, space, exec, barless --launcher          # Hyprland: toggles it
    ... | barless --dmenu -p "Prompt" --width 600         # lines in, chosen line out (exit 1 on Esc)

One search box over: apps (.desktop via GIO), settings (the `["label"]="command"` lines of your
omarchy-menu-alternative, `a/b/` shown as `a › b`), bins (omarchy-* executables, only when you type).
Empty search: recent first, then apps, then settings. Enter runs it, Esc or a click outside closes it,
Up/Down, Ctrl+n/p, Ctrl+j/k, Tab, PageUp/PageDown move. History: ~/.local/state/barless/launcher-history.

Entries that call walker (`omarchy-launch-walker ...`) are skipped; `present_terminal` and `open_in_editor`
are mapped to omarchy-launch-floating-terminal-with-presentation / omarchy-launch-editor.
Pickers (replace Elephant's Lua menus): the menu entries that pointed at `menus:omarchythemes` /
`menus:omarchyBackgroundSelector` (or "Style › Theme" / "Style › Background", added if missing) open a list with an
image preview on the right. Themes: `~/.config/omarchy/themes` + `$OMARCHY_PATH/themes` (default
~/.local/share/omarchy/themes), preview = `<theme>/preview.png`; Enter runs `omarchy-theme-set <name>`.
Backgrounds: `~/.config/omarchy/current/theme/backgrounds` + `~/.config/omarchy/backgrounds/<current theme>`;
Enter runs `omarchy-theme-bg-set '<path>'`. The current one is preselected, Up/Down changes the preview
(debounced, decoded asynchronously), Backspace on an empty search goes back, Esc closes.
Direct keys: `barless --launcher themes` / `barless --launcher backgrounds`.
(`ls ~/.local/share/omarchy/bin | grep theme`) and adjust pick_themes()/pick_bgs() if they differ.

Hyprland: `no_anim` layer rules for `barless-indicator` (already) and `barless-menu` (the --dmenu window).

## Options ([launcher] in config.ini)

    enabled=true            false: `barless --launcher` does nothing; the indicator and card are untouched
    mode=inline             inline: the indicator travels to the middle (one window)
                            separate: the launcher is a short-lived process of its own (`barless --menu`), it can
                            never affect the indicator. No travel animation. Good while testing.
    idle_timeout_s=120      a launcher nobody touches closes itself (the keyboard grab cannot stay stuck)
    placeholder=...         text of the empty search box
    show_kind=true          the "app / setting / command" tag
    hide=libreoffice, foo   entries containing one of these words are left out
    pin=Firefox, Files      these labels come first, in this order
    card=true               the card (clock, calendar, sliders, stats, buttons) on top of the search box,
                            exactly the card
    card_sections=clock,sliders,stats,buttons
    card_hide_when_typing=false   (true: hide the card while you type)
    max_results=100              (most rows listed, 5..200; 100 if the key is not there)
    bar_height_percent=45        (height of the sliding accent bar, % of a row)
    dmenu_width=0                (barless --dmenu is as wide as the card; walker's --width is ignored. Set a number to override)
    runner=true                  (list every program in your PATH once you type 2+ letters; "> cmd args" runs a line as typed)
    calculator=true              (type 2+2*3, (12+3)^2, sqrt(81) ... : Enter copies the result with wl-copy)
    bin_dirs=~/.local/share/omarchy/bin:$PATH   (where programs are listed from; $PATH = all your PATH folders)
    open_speed=1.0               (2 = the whole opening twice as fast, on top of the [dashboard] expand_ms etc.)
    corner_px=3                  (the four dots on the selected row)
    alpha=0.65              background opacity while it is the launcher (replaces [dashboard] card_alpha, e.g. 0.35 on the plain card). 0 = same as the card
    [launcher:entry:name]   your own entry: label=..., cmd=...  (hide=true to switch it off)

The card inside the launcher uses the same [dashboard] settings and [button:...] groups as the normal card.
A button closes the launcher after it ran, except one whose command contains `--launcher`.
The panel opens like the card: from its top right corner towards the bottom left.
In mode=separate the card has no tray buttons (the tray belongs to the running barless).

The panel is a `.card` too: it takes your .card CSS and the [dashboard] card keys, so no extra CSS is needed.
It is as wide as the card ([dashboard] width) unless [launcher] width is set.
Optional selectors: #lentry, #llist row(:selected), .lkind, #lpreview.
Keep running barless alive: run it as a systemd user service with Restart=on-failure.

**The shape.** The card and the launcher are one shape that changes position and size like a fluid: it springs out of the
indicator to its place (and back on Esc, a click outside, or the key again). Each of its four edges is a spring, so it
stretches towards where it goes and overshoots a little before it settles. `[dashboard] morph_ms` (300) is how long it takes,
`morph_bounce` (0.5) the overshoot, `morph_stretch` (0.5) the stretch; `morph_ms=0` turns it off and brings back the box
growing from its corner. While it moves nothing else runs; when it has settled it costs nothing. The window is as big as the screen
while the shape moves, and for the card for as long as it is shown, but it takes clicks only on the card.

The opening is the card's own film (expand_ms / card_delay_ms / element_delay_ms from [dashboard]): the panel grows from
the top right, the card's elements fade in one by one, then the search box and the rows.


## Runner and calculator

- Type part of a program name (2+ letters): programs from `bin_dirs` are listed after apps and settings.
  `omarchy-*` programs rank above the rest. Enter runs it; **Ctrl+Enter** runs it in a terminal (btop, nmtui ...).
- `> any command --with args` (or `known-program args`) shows a "Run:" row on top.
- Maths shows a `= result` row on top; Enter copies it. No units or currencies.
- Both are plain text work at the moment you type: nothing runs in the background.
