# squint

squint is an automatic dimmer for the bright parts of your screen. When a white page, a spreadsheet, or a light-mode application appears, squint lowers the brightness of the bright pixels toward a level you choose and leaves dark content alone. It runs inside the picom compositor on X11, works for every application without per-application setup, and changes nothing in the applications themselves.

## Why use squint

squint is for people who find bright screens uncomfortable: glare that causes headaches or eye strain, sensitivity to bright light, or a dark room with a bright monitor. The usual fixes each leave a gap:

- **Monitor brightness and night-light tools** lower everything by the same amount. Dark text and dark themes get harder to read, and a white page is still the brightest thing on the screen. squint dims only what is bright, so dark content stays as it is.
- **Dark mode** depends on each application. Many web pages, spreadsheets, PDF viewers, and documents have none, and browser extensions only cover the browser. squint works on the finished picture, so it covers every application.
- **picom's own `max-brightness` option** caps the average brightness of a whole window with one global value, and it requires picom's damage tracking to be off, so picom repaints the full screen on every frame. squint works per pixel and per region, and it leaves damage tracking on.

What squint adds beyond dimming:

- A new bright window is dimmed in the first frame it is drawn, so it never appears at full brightness.
- A large bright area can have a lower target than a small one. The size measure counts a bright patch as large at a smaller size than a dim one.
- The dimming fades out over a few pixels around a bright region, so the edge is a gradient, not a hard line.
- After a scene change, such as switching from a dark window to a white page, the new content can start at the brightness you were just looking at and rise to its target over a time you set. Scrolling does not trigger this.
- Per-channel color multipliers and a whole-screen tint, so you can reduce blue light in bright areas or tint the screen.
- Shortcut keys and a tuning tool. Settings take effect as you change them.
- An optional [VSS](https://visualsnowsyndrome.com) relief overlay, off by default. It is the `static` setting: moving noise over the screen.
- Optional RGB lines, off by default, for fun. squint finds thin straight lines on screen (window borders, table grid lines, separators) and recolors them with moving color, such as a rainbow or a snake, in the style of a gaming keyboard. The color keeps the brightness of the line it replaces, so it never gets brighter than the screen unless you ask.

## How it works

picom composes the windows into its back buffer as usual. Just before picom presents the frame, squint redraws every monitor pixel from an undimmed copy of the screen, scales down the pixels that are too bright, and picom presents the result. All the work happens on the GPU. Nothing is read back to the CPU.

The steps, in order:

1. **Capture.** squint copies the rectangles picom just wrote into an undimmed copy of the screen. It never copies the rest of the back buffer, because that holds its own dimmed output.
2. **Need.** Each pixel gets a dimming amount from its own linear luminance (Rec. 709 weights). A pixel at or below `target` needs none. A brighter pixel is scaled toward `target`, never below `min_gain`. `knee` rounds the corner at the target.
3. **Size.** With `target_large` below `target`, the target falls as the surroundings get large and bright. squint measures a power mean of luminance over `size_px`, which is `mean(L^size_bias)^(1/size_bias)`. A patch of luminance `L` covering a fraction `C` of that neighborhood measures `L * C^(1/size_bias)`, so a bright patch counts as large at a smaller size than a dim one.
4. **Falloff.** The dimming spreads outward from bright pixels with a straight-line falloff over `falloff_px`, so the area just around a bright region darkens a little too, fading to nothing. It never crosses a monitor edge, and pixels the falloff does not reach are copied bit for bit.
5. **Scene changes.** squint compares each monitor's blurred brightness with the previous frame at 1,024 sample points. When more than `scene_pct` percent of the points changed significantly (by more than 0.03, or by more than 25 percent), that monitor counts as having a scene change. Scrolling a page moves many pixels but barely changes the blurred brightness, so it does not count.
6. **Smoothing.** After a scene change, `darkening_ms` makes dimming fade out slowly when content gets darker, and `brightening_ms` makes newly brighter content start at the brightness and gamma that was shown before and rise to the new level. Each stays active for five times its setting after the scene change. Without a scene change, the screen follows the content at once.
7. **Final pass.** Gamma, the per-channel dim multipliers, and the whole-screen tint are applied. Then the optional RGB lines are recolored, and last the optional static overlay is mixed in.
8. **Lines.** Only with `rgb` on, and only when the screen has changed since the last search. squint searches the undimmed copy for lines (see the RGB lines section) and keeps the result in a mask. The final pass reads that mask on every frame, so the animation costs almost nothing extra.

The brightness measurements use the screen after dimming, so a dimmed window does not look bright to the scene detector.

## Requirements

- Linux with X11
- picom 12.5 with plugin support (`picom --help` lists `--plugins`), running the GLX backend
- OpenGL 3.3 or newer
- To build: gcc, make, pkg-config, and the development files for libev, libGL, libX11, libXrandr, libxcb, and pixman

On Debian or Ubuntu:

```
sudo apt install build-essential pkg-config libev-dev libgl-dev libx11-dev libxrandr-dev libxcb1-dev libpixman-1-dev libxtst-dev
```

`libxtst-dev` is only for the shortcut key test.

The plugin is built against picom 12.5's public headers, which are copied into `third_party/picom/`. picom checks the plugin's API version when it loads, and it refuses a plugin built for a different version.

## Build

```
make
```

This builds `squint.so` (the plugin) and `squintctl` (the settings tool). If the libev headers are not installed, point the build at a directory that contains `ev.h`:

```
make EV_INC=/path/to/include
```

`make test` builds and runs the test program. `make clean` removes the build output.

## Run

Start picom with the plugin:

```
picom --backend glx --plugins /full/path/to/squint.so
```

To start it from i3, replace your picom line in `~/.config/i3/config`:

```
exec --no-startup-id picom --backend glx --plugins /full/path/to/squint.so
```

To go back to plain picom, restart it without `--plugins`:

```
pkill picom; setsid -f picom --backend glx
```

Rebuilding while picom runs is safe, because the build replaces `squint.so` by renaming it. A running picom keeps using the old copy until you restart it.

## Settings

squint reads `~/.config/squint.conf`. Set `SQUINT_CONF` to use another path. The plugin watches the file, so a change takes effect within a frame, with no restart.

Edit the file with the tuning tool:

```
squintctl                 interactive tuning screen
squintctl show            print the config file location and the current settings
squintctl set KEY VALUE   change one setting
squintctl toggle NAME     flip an on/off setting (enabled, brightness, dim_tint, static, rgb, or shortcuts)
```

In the tuning screen, which scrolls when the settings do not fit the terminal (the header shows which rows are in view), up and down select a setting, left and right adjust it by one step, PgUp and PgDn adjust by ten steps, space flips a toggle, Enter edits a shortcut key, `r` resets the selected setting to the value it had when the tool opened, `R` does that for every setting, `d` resets the selected setting to its default, `D` does that for every setting, and `q` quits. A `*` at the left of a row marks a setting that differs from the file as it was when the tool opened. Every change is saved at once. Values outside a setting's range are clamped.

Settings come in groups. Each group has a toggle, and its settings are indented under it in the file and in the tuning screen. The tuning screen dims a group's settings while its toggle is off. The leading spaces are only layout, so the parser ignores them.

The old names `tau_ms` and `rise_ms` still load as `darkening_ms` and `brightening_ms`.

To flip a toggle from i3 instead of squint's own shortcut keys (see below):

```
bindsym $mod+Shift+d exec --no-startup-id squintctl toggle enabled
```

### Defaults and example

Only `enabled` and `brightness` are on by default. `dim_tint`, `static`, `rgb`, and `shortcuts` are off, and you turn each group on when you want it. The numbers are the values the author uses. This is the default configuration, laid out the way the file is written:

```
enabled = true

brightness = true
  target = 0.50
  target_large = 0.35
  size_px = 32
  size_bias = 2.00
  min_gain = 0.05
  knee = 0.10
  darkening_ms = 0
  brightening_ms = 1500
  scene_pct = 33
  falloff_px = 8
  gamma = 1.50

dim_tint = false
  dim_red = 1.00
  dim_green = 1.00
  dim_blue = 1.00
  tint_red = 1.00
  tint_green = 0.80
  tint_blue = 0.50

static = false
  static_amount = 4
  static_grain_px = 16.00
  static_color = 0.50
  static_hz = 60
  static_level = 0.50
  static_contrast = 0.10
  static_clusters = 0.30
  static_scanlines = 0.15
  static_smear = 1.30
  static_jitter_px = 0.50

rgb = false
  rgb_effect = rainbow
  rgb_speed = 0.50
  rgb_luma = 1.00
  rgb_min_luma = 0.00
  rgb_hue = 200
  rgb_hue_span = 360
  rgb_saturation = 1.00
  rgb_length_px = 120
  rgb_gap_px = 200
  rgb_tail = 0.60
  rgb_direction = forward
  rgb_sync = false
  rgb_line_min_px = 48
  rgb_line_thick_px = 3
  rgb_line_contrast = 0.15
  rgb_horizontal = true
  rgb_vertical = true

shortcuts = false
  key_enabled = Super+F1
  key_brightness = Super+F2
  key_dim_tint = Super+F3
  key_static = Super+F4
  key_rgb = Super+F5
  key_rgb_effect = Super+F6
```

A setting missing from the file takes its default. The tint is amber by default (`tint_green` 0.80 and `tint_blue` 0.50), so it shows as soon as you turn `dim_tint` on.

### Master switch

| Setting | Default | Meaning |
|---|---|---|
| `enabled` | true | Turns everything off when false. The screen is copied unchanged. |

### Brightness group

`brightness` switches the dimming on and off. It is on by default. When it is off, none of the dimming work runs.

| Setting | Default | Range | Meaning |
|---|---|---|---|
| `target` | 0.50 | 0.05 to 1 | Linear luminance that bright pixels are dimmed toward. |
| `target_large` | 0.35 | 0.05 to 1 | The target inside large bright areas. At or above `target`, size has no effect. |
| `size_px` | 32 | 16 to 512 | Radius over which brightness is gathered to judge size. Also the scale of the rise. |
| `size_bias` | 2.0 | 1 to 4 | How much brightness outweighs size. 1 is the plain average. |
| `min_gain` | 0.05 | 0.05 to 1 | Lowest gain a pixel can get, so nothing goes fully black. |
| `knee` | 0.10 | 0 to 0.5 | Width of the soft transition around the target. |
| `darkening_ms` | 0 | 0 to 5000 | How slowly dimming fades after content gets darker, after a scene change. 0 is instant. |
| `brightening_ms` | 1500 | 0 to 5000 | Time for newly brighter content to rise from the brightness shown before, after a scene change. 0 is instant. |
| `scene_pct` | 33 | 1 to 100 | Percent of a monitor that must change significantly to count as a scene change. 100 never does. |
| `falloff_px` | 8 | 0 to 128 | Distance over which the strongest dimming fades out around bright pixels. 0 dims only the bright pixels. |
| `gamma` | 1.5 | 0.5 to 3 | Gamma where dimming is strongest. Above 1 darkens midtones, and below 1 lifts them. |

### Dim and tint group

`dim_tint` switches the color multipliers on and off. It is off by default.

| Setting | Default | Range | Meaning |
|---|---|---|---|
| `dim_red`, `dim_green`, `dim_blue` | 1.0 | 0.2 to 1 | Channel multipliers where dimming is strong. They fade in with the dimming, so pixels that are not dimmed keep their colors. |
| `tint_red`, `tint_green`, `tint_blue` | 1.0, 0.8, 0.5 | 0.2 to 1 | Channel multipliers for the whole screen, dimmed or not. There is one tint for all monitors. |

With every multiplier at 1, pixels the dimming does not touch stay bit for bit identical to what picom drew.

### Static group

`static` mixes moving noise over the screen. It is off by default. The noise follows a written description of a static video made for visual snow relief. Whether such static helps is not established, and squint does not claim it does.

Each update draws a fresh, independent pattern with no blending between updates. The default look is a faint, middle-gray static with coarse 16 pixel speckles, a little color, some clustering, faint scan lines, a slight sideways smear, and a small wobble of each row. The description of the video says the speckles are 1 to 3 pixels wide and the static is gray, so for that look set `static_grain_px` to 2, `static_color` to 0, `static_amount` higher, and `static_contrast` to 0.22.

| Setting | Default | Range | Meaning |
|---|---|---|---|
| `static_amount` | 4 | 0 to 100 | How much of the screen is static. 100 is pure static, and lower values mix it over the content. |
| `static_grain_px` | 16.0 | 1 to 16 | Size of one speckle in pixels. |
| `static_color` | 0.5 | 0 to 1 | 0 is gray static. 1 is independent noise in each color channel. |
| `static_hz` | 60 | 1 to 60 | Updates per second. |
| `static_level` | 0.5 | 0 to 1 | Average brightness of the static. 0.5 is middle gray. |
| `static_contrast` | 0.10 | 0 to 0.5 | How far speckles stray from the average. |
| `static_clusters` | 0.3 | 0 to 1 | How much coarse clustering, speckles grouped 3 to 6 pixels wide, is mixed in. |
| `static_scanlines` | 0.15 | 0 to 1 | Darkens every other row for fine horizontal lines. |
| `static_smear` | 1.3 | 1 to 6 | Stretches speckles sideways. 1 is round. |
| `static_jitter_px` | 0.5 | 0 to 4 | Random sideways shift of each row, in pixels. |

While static is on, squint presents `static_hz` frames a second, even when nothing else on screen changes, so the GPU cannot idle.

### RGB group

`rgb` recolors thin straight lines with moving color. It is off by default.

**What counts as a line.** A horizontal line is a band of at most `rgb_line_thick_px` rows of one color, at least `rgb_line_min_px` pixels long. The row on at least one side of the band must differ from it in color by at least `rgb_line_contrast`, the largest difference in any red, green, or blue value as a fraction of 255. A colored line with little brightness contrast, such as a teal border on dark gray, still counts. One side is enough, because a window border or scroll bar often touches a nearly matching color on the other side, and a line on the edge of a monitor has only one side. The search reads the screen as picom drew it, before dimming, tint, static, and the RGB recoloring, so none of those change which pixels are lines. Vertical lines work the same with columns. Where another line of the same color crosses a line, as in a grid, the crossing does not break it: squint bridges crossings up to `rgb_line_thick_px` pixels wide. Two pixels are the same color when no channel differs by more than 5 out of 255. A line stops at the edge of its monitor. A line that crosses two monitors counts on each one separately, so it must be long enough on each.

**What color it gets.** The new color keeps the luminance of the pixel it replaces, measured after dimming and tint, multiplied by `rgb_luma` and capped at white. The hue comes from the effect. When a saturated color cannot be that bright (pure blue tops out near 7 percent luminance), squint mixes it toward gray of the same luminance until it fits, so the luminance is exact and the color gives way. A black line has no luminance to inherit and stays black, unless `rgb_min_luma` sets a floor.

| Setting | Default | Range | Meaning |
|---|---|---|---|
| `rgb_effect` | rainbow | solid, breathe, rainbow, snake, wave, rain | `solid` is one color. `breathe` fades it in and out. `rainbow` moves the hue along each line. `snake` moves a lit segment along each line and leaves the rest alone. `wave` moves ripples of brightness along each line. `rain` is green digital rain, in the style of The Matrix: drops with a pale head and a fading trail fall along each line at three speeds, over a dim green glow. |
| `rgb_speed` | 0.5 | 0.1 to 5 | Cycles per second for `breathe`. For the others, how many lengths (`rainbow`, `wave`) or segment periods (`snake`) pass per second. |
| `rgb_luma` | 1.0 | 0.1 to 4 | Brightness multiplier on the brightness of the pixel being replaced. |
| `rgb_min_luma` | 0 | 0 to 1 | Floor for that brightness before the multiplier, so dark lines can light up. |
| `rgb_hue` | 200 | 0 to 359 | Base hue in degrees: 0 red, 120 green, 240 blue. `rain` is always green and ignores it. |
| `rgb_hue_span` | 360 | 0 to 360 | Degrees of hue that `rainbow` and `snake` sweep. |
| `rgb_saturation` | 1.0 | 0 to 1 | Color strength, up to 1. |
| `rgb_length_px` | 120 | 10 to 1000 | Length of one `rainbow` or `wave` cycle, of the `snake` segment, or of the longest `rain` trail. |
| `rgb_gap_px` | 200 | 0 to 2000 | Unlit pixels between `snake` segments, or the spacing between `rain` drops. |
| `rgb_tail` | 0.6 | 0 to 1 | How softly a `snake` fades toward its tail. 0 is a hard block. |
| `rgb_direction` | forward | forward, backward, alternate | Which way motion runs along a line: right or down, left or up, or reversed on every other line. `alternate` numbers the lines on each monitor separately: horizontal lines from the top, vertical lines from the left, each counted on its own. Odd lines run forward and even lines backward, and every monitor starts again at 1. Rows that touch each other count as one line, so a 3 px band is one line. |
| `rgb_sync` | false | false to true | On, every line moves in step. Off, each line has its own phase. |
| `rgb_line_min_px` | 48 | 8 to 256 | Shortest run that counts as a line. |
| `rgb_line_thick_px` | 3 | 1 to 5 | Thickest line. |
| `rgb_line_contrast` | 0.15 | 0.02 to 1 | Color difference a line needs from the pixels on at least one side: the largest difference in any red, green, or blue value, as a fraction of 255. Light gray grid lines on white differ by about 0.1, so lower this to catch them. |
| `rgb_horizontal`, `rgb_vertical` | true | false to true | Which directions to search. |

`rgb_effect` and `rgb_direction` are names in the file (`rgb_effect = snake`), and the tuning screen steps through them with left and right.

While `rgb` is on with any effect but `solid`, squint presents a frame about every 15 ms, even when nothing else on screen changes, so the GPU cannot idle. The search for lines runs on every frame that picom draws something, and takes 8 to 13 ms on the benchmark setup (see Known limitations).

### Shortcut keys

The shortcuts group lets a key flip each toggle from anywhere, whichever window has focus. The default keys are Super+F1 for `enabled`, Super+F2 for `brightness`, Super+F3 for `dim_tint`, Super+F4 for `static`, Super+F5 for `rgb`, and Super+F6 for the next RGB effect. `shortcuts` is off by default, so squint grabs no key until you turn it on. Turning `shortcuts` off releases them all at once.

| Setting | Flips |
|---|---|
| `key_enabled` | `enabled` |
| `key_brightness` | `brightness` |
| `key_dim_tint` | `dim_tint` |
| `key_static` | `static` |
| `key_rgb` | `rgb` |
| `key_rgb_effect` | `rgb_effect`, which steps to the next effect and wraps from `rain` to `solid` |

In the tuning screen, select a key setting and press Enter, then press the keys you want, for example Super+F1. squint records the chord itself, so it also gets chords a terminal cannot see, such as anything with Super. While it waits, squint holds the keyboard for up to 20 seconds. Escape alone cancels. Press `e` instead to type the chord as text.

A chord is modifiers and one key name, joined by `+`. For example `Ctrl+Alt+s`, `Super+Shift+F5`, or `Super+space`. Modifier names are `Shift`, `Ctrl` (or `Control`), `Alt` (or `Mod1`), `Super` (or `Mod4`), and `Mod3` and `Mod5`. Key names are X keysym names, the ones `xev` prints. Names and modifiers match without regard to case, so `super+f1` works. From the shell:

```
squintctl set key_static Ctrl+Alt+s
squintctl toggle shortcuts         # the shortcut keys only work while this is on
squintctl set key_static ""        # remove the key
```

`squintctl` checks that the keyboard has the key before it saves the chord. Recording needs a running X display. If another program already holds the keyboard, the tuning screen says so, and typing the chord with `e` still works. The plugin grabs the chord on the X root window, so another program that already uses the same chord, such as an i3 binding, blocks it. squint then logs `another program already uses` that chord and leaves it unbound. Num Lock and Caps Lock do not change what a chord matches. A held key triggers once, not on every repeat. The shortcuts keep working while `enabled` is false, so `key_enabled` can turn squint back on.

A shortcut writes the new value to the config file the same way `squintctl` does, so the tuning screen and the file always agree.

## Log

squint writes a log to `/tmp/squint.log`, or to the file named by `SQUINT_LOG`. The log shows the settings on load, the monitors it found, and a statistics line every 300 frames with the GPU time of the copy and the pipeline, the CPU time inside the pipeline, GL error counts, and how many frames picom drew with an empty damage region. After a successful start the log contains `pipeline ready`. If a shader fails to compile, squint logs the error, stops dimming until picom restarts, and leaves picom running.

## Tests

`make test` runs `test/pipeline_test.c`. It creates its own OpenGL 3.3 context on a 1x1 off-screen buffer on the running X server, so it never touches picom or the visible screen. It runs the same GL code the plugin runs, then compares the output with a CPU copy of the same math. The tests cover:

- every pixel against a brute-force model of the falloff, the size measure, the ramp, the smoothing, the scene-change detector, the tint and channel gains, and the static noise;
- exact bit-for-bit copies wherever nothing should change;
- two monitors, monitors that do not line up with the internal blocks, and a screen size that is not a power of two;
- damage shapes from picom, including empty damage and hundreds of small rectangles;
- the config file format, clamping, and the grouped layout;
- all eight combinations of the brightness, dim_tint, and static toggles;
- the RGB line search against a brute-force model, with thickness, length, color contrast on one side or neither, crossing lines, lines on monitor edges and along the seam between monitors, and a check that dimming, tint, and static do not change which pixels are lines;
- the RGB effects: luminance inheritance, the gamut fallback, the minimum luminance, the timing of each effect, the green and pale heads of `rain` and its continuity at the end of a cycle, and `alternate` numbering per monitor and per orientation;
- the shortcut chord parser, the text settings that hold chords, and the enum settings (names, ranges, and the file round trip).

`make test` also runs `test/hotkey_test.c`, which grabs a chord on the running X server and sends key presses with the XTest extension. It checks that a chord fires once, that a repeat inside 0.3 s is ignored, that the wrong modifiers do not fire, that a chord another connection already holds is refused, and that released keys are free to grab again. The chord is Ctrl+Alt plus a spare key (F24, or one of the XF86Launch keys), which the test grabs before sending it. It also checks that key names match without regard to case, and that recording a chord from real key presses gets Super+F1, modifiers in a fixed order, and Ctrl+Escape right, ignores modifiers pressed alone, cancels on Escape, and times out. If the keyboard has none of them, or the server has no XTest extension, the test says so and passes.

Nothing in the suite drives the tuning screen or the shortcut that steps `rgb_effect`. The tuning screen was checked by hand in a pseudo-terminal at several sizes, including a resize. The effect shortcut has not been run in picom.

The pipeline test needs a display and a GPU driver with OpenGL 3.3. The CPU model of the falloff is brute force and slow on large images, so the large tests use falloff 0.

## Known limitations

- **One configuration tested.** squint has run on picom 12.5, i3, an NVIDIA RTX A1000 with the 550 driver, and three monitors. Other drivers, picom versions, and window managers are untested.
- **GLX only.** The plugin registers for picom's `glx` backend. It does not support `egl`.
- **Screenshots and screen shares.** A capture of the whole screen reads the same pixels the monitor shows, so it includes squint's dimming, tint, and static. A capture of a single window reads the application's own picture and is unaffected. This was checked with a test window: a whole-screen screenshot read (157, 141, 116) for a white window, and a window capture read (255, 255, 255).
- **Fullscreen windows that picom unredirects** (with `--unredir-if-possible`) bypass the compositor, so squint cannot dim them.
- **Monitor changes.** squint reads the monitor layout when picom starts and when the root window changes size. If you rearrange monitors without changing the total size, restart picom.
- **Bright shades flatten.** Pixels above `target` all land near the same level, so white and light grays end up close together. `knee` only rounds the corner. A lower `target_large` makes this stronger.
- **Shortcut keys.** They flip a toggle, and `key_rgb_effect` steps through the RGB effects. There is no key for any other setting, and no chord can include a mouse button. The Num Lock and Caps Lock variants of a chord are grabbed but were not tested live, because the test would change your lock state.
- **No per-application rules.** Dimming applies to the screen. There is no list of windows to skip, and there is one tint for all monitors.
- **No vsync.** picom runs without vsync by default. Animated static may tear unless you start picom with `--vsync`. This has not been tested on this setup.
- **GPU cost.** Measured in a standalone benchmark on the RTX A1000 at full clock, with a 3760x2640 root over three monitors, a frame that changes part of the screen takes about 3 to 6 ms, depending on the settings. A frame with nothing new takes under 1 ms. Static takes about 0.8 ms per update in gray and 2 ms in color. Real use with the GPU at a lower clock reads two to four times higher. Power and fan use are not measured.
- **Memory.** squint keeps several full-screen textures, about 16 bytes per pixel of the root window, or about 160 MB for a 3760x2640 root.
- **RGB cost.** The search for lines is full-screen and runs whenever picom draws anything, even a blinking cursor. On the benchmark setup (3840x2640, three monitors) it took 8.3 ms with `rgb_line_min_px` 16, 11.4 ms with 64, and 12.5 ms with 256. The animation itself added about 0.15 ms a frame. Searching only the damaged part of the screen would cut this, and is not done.
- **RGB false positives.** Anything flat and thin that contrasts with one side counts, such as a 2 px colored header bar, a divider, or a progress bar. Text underlines are usually too short. Raise `rgb_line_min_px` or `rgb_line_contrast` to drop weaker matches. A line that is anti-aliased, a gradient, or a different color in each pixel is not found.
- **Line numbers for `alternate`.** Lines are numbered by the rows (or columns) they occupy on a monitor, not one by one. Two horizontal lines side by side in the same rows, such as the top borders of two panes, share a number. The numbering costs about 0.7 ms a frame and runs only while `rgb_direction` is `alternate`.
- **RGB speed unit.** `rgb_speed` is a cycle rate, so changing it while an effect runs makes the animation jump to a new phase.
- **Fixed thresholds.** The scene-change thresholds (0.03 or 25 percent per sample point), the size measure's range (0.2 to 0.5), and the same-color tolerance of the line search (5 out of 255) are constants in `src/pipeline.h`, not settings.
- **No test window.** The original plan included a test window that alternates between black and white. It has not been written.

## Layout

| Path | Contents |
|---|---|
| `src/pipeline.h` | The GL pipeline. It has no dependency on picom. |
| `src/squint.c` | The picom plugin: hooks, the libev timer, config watching, and logging. |
| `src/config.h` | Setting names, ranges, defaults, the file format, and the chord parser, shared by the plugin and the tool. |
| `src/hotkey.h` | Shortcut key grabs on the X root window. |
| `src/keys.h` | Key name lookup that ignores case. |
| `src/capture.h` | Recording a chord from key presses, for `squintctl`. |
| `src/squintctl.c` | The settings tool. |
| `test/pipeline_test.c` | The GL pipeline test and its CPU model. |
| `test/hotkey_test.c` | The live shortcut key test. |
| `third_party/picom/` | picom 12.5's public headers (MPL-2.0) and their license. |
| `ref/`, `build/` | Local scratch, ignored by git: a picom source checkout for reference, and extracted libev headers. |
