# barless

A tiny desktop shell for Hyprland in C, **GTK3 + gtk-layer-shell only**. One process, **one window**.

That window is the indicator: time, CPU temperature, battery arrow / watts (OLED-muted, nudges itself, dodges the pointer).
When the active workspace has no windows it **expands into the card**: the indicator row is swapped for the card in the
same window (no second surface, no second process), growing from the top-right corner. When a window appears the film
plays backwards, the card widgets are destroyed and the window shrinks back to the indicator. There is never a second clock,
so no `hide_on_empty`.

Card, top to bottom: big halftone clock + calendar (today in the accent colour) · volume / brightness bars side by side
(icon at the start, number at the end) · stats on one line (`cpu 12%  temp 54°C  ▼ 0 B/s ▲ 0 B/s`) · small buttons
(tray icons first, then your `[button:...]` groups).

## Build

```
sudo pacman -S gtk3 gtk-layer-shell
make && make install        # ~/.local/bin/barless
```

`exec-once = barless`. Open the card at start (to design it): `./barless --dashboard`. Sensors: `./barless --info`.
Switch Hyprland's own layer animation off for the window, or it slides in on top of the card's film:

```lua
hl.layer_rule({ name = "barless-indicator", match = { namespace = "barless-indicator" }, no_anim = true })
```

## Config (`~/.config/barless/config.ini`, `style.css`)

`[dashboard]`: `width margin margin_top section_gap interval radius bg card_alpha` · `clock_align stats_align button_align` ·
`button_size button_gap columns show_labels terminal tray_buttons tray_icon` · `time_format calendar calendar_side
calendar_fraction calendar_week_start calendar_font_px accent` · `halftone halftone_skip halftone_dot|fog|sharp|stagger`
and per section `<section>.halftone_*` (sections `clock sliders stats buttons`; `dot=0` = shader off) ·
`expand_ms card_delay_ms element_delay_ms` · `sliders slider_gap slider_format slider_icon_pos slider_value_pos volume_* brightness_*`
· `stats` (`cpu,temp,net,battery`, order = order shown) `stats_gap`.

While expanded the window is named `dashboard` (`window#dashboard`), otherwise `indicator`, so the two looks never mix.
CSS names: `.card`, `#clock #sliders #stats #buttons`, `#bigclock`, `#calendar .cal-head .cal-today`,
`scale.slider trough/highlight`, `.slider-icon .slider-val` (`.filled`), `.stat-key .stat-val`, `button.action`.
`columns=0` fits as many buttons per row as the card is wide.

Removed (ignored): `date_*`, `key_align`, `value_align`, `panel_fill`, `movable`, `*_chart*`, `slider_layout/side/fraction/height/padding/bar_width`,
`notifications*`, `hide_on_empty`.

## Source

`main.c` args · `common.c` config + defaults + sensors · `indicator.c` the window, indicator, Hyprland events, expand/collapse ·
`card.c` card sections, halftone, sliders, animation · `tray.c` StatusNotifier host.
