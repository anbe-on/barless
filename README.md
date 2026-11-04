# panelless

An OLED-friendly, always-on minimal indicator and empty-workspace dashboard for Linux and Hyprland, built with GTK 3 and gtk-layer-shell.

```
       [ 14:32   48°   ▲ 15.2W ]   <-- Always-on indicator (or hides when empty)
  ____________________________________
 |                                    |
 |           14:32                    |   <-- Clean dashboard on empty workspaces
 |           Wednesday, 4 November    |
 |  --------------------------------  |
 |   cpu                          12% |
 |   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ |   <-- Universal charts (CPU, Temp, Net, Bat)
 |   temp                         48° |       with continuous history tracking
 |   net       ▼ 1.2MB/s  ▲ 180KB/s   |
 |   -------------------------------- |
 |   [Menu] [Wifi] [Bluetooth] [Lock] |   <-- Customizable action buttons
 |____________________________________|
```

---

## Highlights

- **OLED Care & True Black**: Transparent background ensures zero pixel power draw on OLED screens. Features subtle pixel shifting (`shift_px` every `shift_every` seconds) to eliminate burn-in risks.
- **Hover Dodge**: Moves to the top-center when your cursor touches it, preventing interference with top-right window controls or tabs.
- **Smart Battery Triangle Indicator**: Directional indicator (▲ charging, ▼ discharging, ● idle/full) directly filled with the colored battery charge level, paired with real-time wattage.
- **Workspace-Aware Auto Dashboard**: Auto-opens the dashboard when the active workspace has no windows and stops when a window appears (Hyprland IPC).
- **Hide Indicator on Empty Workspace (Toggleable)**: Optionally hide the indicator while on an empty workspace (`hide_on_empty = true`) so all stats are cleanly displayed on the big panel without redundancy.
- **Universal Graphs with Persistent Tracking**: Real-time line charts for Network, CPU, Temperature, and Battery. Samples are recorded continuously in a shared memory ring buffer, so graphs **never reset** when you switch workspaces or reopen the panel.
- **Configurable Background & Transparency**: Easily adjust window and card background colors or alpha transparencies directly from `config.ini` or `style.css`.
- **Built-in System Tray**: StatusNotifierItem watcher that preserves tray app ownership across dashboard opens and closes.
- **Live CSS Hot Reload**: Edit `style.css` and see visual changes immediately on save.

---

## Installation & Requirements

### Dependencies

- `gtk+-3.0`
- `gtk-layer-shell-0`
- `gcc` or `clang`
- `make`
- `pkg-config`
- A Nerd Font (e.g., `GeistMono Nerd Font`) for glyph icons.

On Arch Linux / Omarchy:
```bash
sudo pacman -S base-devel gtk3 gtk-layer-shell
```

On Debian / Ubuntu:
```bash
sudo apt install build-essential libgtk-3-dev libgtk-layer-shell-dev pkg-config
```

On Fedora:
```bash
sudo dnf install gcc make gtk3-devel gtk-layer-shell-devel pkgconf-pkg-config
```

### Building & Installing

```bash
# Clone the repository
git clone https://github.com/your-username/panelless.git
cd panelless

# Build the executable
make

# Install to ~/.local/bin/panelless
make install
```

---

## Usage

```bash
# Run panelless (indicator daemon + auto-dashboard on empty workspaces)
panelless

# Run only the dashboard (useful for previewing design tweaks)
panelless --dashboard

# Print detected hardware sensors, battery, and Hyprland IPC status
panelless --info

# Show help options
panelless --help
```

### Testing & Simulation

You can preview battery states and CSS color transitions without draining hardware battery:
```bash
PANELLESS_FAKE_BAT="18 discharging 7.5" panelless
```
Format: `"<capacity 0-100> <charging|discharging|idle> <watts>"`

---

## Configuration Guide

Configuration files are located at `~/.config/panelless/`:
- `config.ini`: Layout, sensor intervals, toggles, and button actions.
- `style.css`: Colors, fonts, margins, and visual styles (hot-reloaded on save).
- `position.ini`: Auto-generated when middle-click dragging the dashboard to save its screen position.

Default configuration files are created automatically on first run if they do not exist.

### 1. Configuring Stats (`stats = ...`)

In `~/.config/panelless/config.ini` under `[dashboard]`:
```ini
[dashboard]
# Which stats to show and in what order (comma-separated: cpu, temp, net, battery).
# To hide any metric, simply remove it from the list:
stats=cpu,temp,net,battery
```

Examples:
- Only CPU and Net: `stats=cpu,net`
- Custom order: `stats=battery,temp,cpu,net`

---

### 2. Configuring Universal Graphs

Panelless supports line charts under any metric. Historical values are recorded continuously every second in a shared memory ring buffer (`$XDG_RUNTIME_DIR/panelless/history.bin`), ensuring **graphs keep track of values and do not reset** when switching workspaces or reopening the dashboard.

In `~/.config/panelless/config.ini` under `[dashboard]`:
```ini
[dashboard]
# Toggle line charts under stats:
net_chart=true
cpu_chart=true
temp_chart=true
battery_chart=false

# Global seconds of history displayed in charts (default: 30)
chart_seconds=30

# Global chart height in pixels (default: 36)
chart_height=36

# Individual overrides (optional):
# net_chart_seconds=60
# cpu_chart_seconds=45
# temp_chart_seconds=30
# cpu_chart_height=40
```

- **Net chart**: Two lines (solid line for download, dimmer line for upload) with auto-scaling (10 KB/s floor).
- **CPU chart**: Real-time CPU % (scaled 0–100%).
- **Temp chart**: Real-time package temperature in °C.
- **Battery chart**: Battery charge capacity % (0–100%).

---

### 3. Indicator Configuration & Triangle Battery

In `~/.config/panelless/config.ini` under `[indicator]`:
```ini
[indicator]
# Position from top-right corner in pixels
margin_top=6
margin_right=10

# Seconds between battery and temperature sensor reads
interval=5

# OLED Care: nudge the indicator by shift_px every shift_every seconds (0 = off)
shift_every=300
shift_px=2

# Hover dodge: jumps to top-center when cursor touches it
dodge=true
dodge_ms=1200

# Sensor display toggles
show_temp=true
show_watts=true

# Battery Look:
# - arrow    (default): colored level meter rendered directly on the up/down triangle;
#                       ▲ = charging, ▼ = discharging, ● = full/idle on AC.
# - square   : classic battery level square.
# - both     : square meter + separate directional arrow glyph.
battery_style=arrow

# Battery state thresholds (maps to CSS classes .ok, .mid, .low)
battery_mid=50
battery_low=20

# Temperature thresholds (maps to CSS classes .warm and .hot on #temp)
temp_warm=65
temp_hot=85

# Unfilled portion transparency of the battery shape (0.0 to 1.0)
track_alpha=0.25

# Corner radius of the battery square if battery_style=square
radius=0

# When active workspace has no windows and the dashboard opens,
# set to true to hide the indicator so all stats are shown only on the dashboard:
hide_on_empty=false

# Window background color or transparency:
bg=transparent
# Or specify alpha directly:
# bg_alpha=0.0
```

---

### 4. Background and Transparency Configuration

Both the indicator and dashboard windows, as well as cards, support customizable backgrounds and alpha transparency:

#### In `config.ini`:
```ini
[indicator]
# Set background color or transparency:
bg=transparent
# Or e.g.: bg=rgba(22, 22, 29, 0.85)
# Or e.g.: bg_alpha=0.2

[dashboard]
# Window background:
bg=transparent
# Or e.g.: bg=rgba(22, 22, 29, 0.75)
# Or e.g.: bg_alpha=0.0

# Card background transparency (0.0 = fully transparent, 1.0 = solid):
card_alpha=0.06
```

#### In `style.css`:
You can also customize background colors and cards directly in CSS:
```css
window#indicator {
  background-color: transparent;
}

window#dashboard {
  background-color: transparent;
}

.card {
  background-color: alpha(@foreground, 0.08);
  border-radius: 8px;
}
```

---

### 5. Buttons Configuration

Add, remove, or reorder launcher buttons in `config.ini`. Each button is defined in its own `[button:identifier]` block:

```ini
[button:menu]
icon=󰅀
label=Menu
cmd=omarchy-menu-alternative
tooltip=App Launcher

[button:wifi]
icon=󰤨
label=Wifi
cmd=omarchy-launch-wifi

[button:bluetooth]
icon=󰂯
label=Bluetooth
cmd=omarchy-launch-bluetooth

[button:btop]
icon=󰓅
label=btop
cmd=btop
terminal=true    # Launches inside terminal defined by `terminal` setting

[button:lock]
icon=
label=Lock
cmd=hyprlock

[button:sleep]
icon=󰤄
label=Sleep
cmd=systemctl suspend

[button:shutdown]
icon=
label=Shutdown
cmd=systemctl poweroff
```

Options per button:
- `icon`: Nerd Font glyph.
- `label`: Text label next to icon (set `[dashboard] show_labels=false` for icon-only mode).
- `cmd`: Command to execute. For pipelines, use `sh -c '...'`.
- `terminal`: Set `true` to run inside a terminal emulator (`xdg-terminal-exec`).
- `hide`: Set `true` to keep the button definition without displaying it.
- `tooltip`: Tooltip displayed on hover.

---

### 6. Styling Reference (`style.css`)

CSS styles are hot-reloaded automatically whenever you save `~/.config/panelless/style.css`.

Key CSS selectors:
- `#indicator`: Top indicator window.
- `#indicator #time`: Clock label in indicator.
- `#indicator #temp`: Temperature label in indicator (`.warm`, `.hot`).
- `#indicator #battery`: Battery meter widget (`.ok`, `.mid`, `.low`).
- `#indicator #watts`: Wattage label (`.ok`, `.mid`, `.low`).
- `#dashboard`: Dashboard window.
- `.card`: Card container boxes.
- `#bigclock`: Large dashboard clock.
- `#date`: Dashboard date display.
- `.stat-key`, `.stat-val`: Stat labels and values.
- `.chart`, `#netchart`, `#cpuchart`, `#tempchart`, `#batterychart`: Line chart colors.
- `button.action`: Action buttons and `:hover` state.

---

## License

MIT License. See [LICENSE](LICENSE) for details.
