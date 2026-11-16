# Panel UI

The UI lives in `ui/`, apart from the firmware, so it runs both on the P4 and in a browser.

- `ui/src`: the UI (`ui.c`) and its draw layer (`canvas.c`), plain C with no hardware or libc. The UI takes a `ui_state_t` (channel, frequency, decoding, locked, signal level, USB), draws the 170x480 strip right of the picture, and turns touches into actions such as "tune R5".
- `ui/fonts`: Inter SemiBold baked into 4-bit alpha bitmaps at 20, 36 and 80 px (`inter.c`, from `rake ui:fonts`), with its SIL OFL licence. Digits use Inter's tabular forms. No font engine runs on the device.
- `ui/sim`: the simulator page, its WebAssembly glue (`sim.c`), a six-second clip of p4vrx's own decoded video, and the dev server. It never opens a board or a serial port.
- `ui/CMakeLists.txt` makes `ui/` an ESP-IDF component, which `firmware/p4` adds through `EXTRA_COMPONENT_DIRS`. On the P4, `screen.c` fills the state ten times a second, and `lcd.c` has the PPA turn the strip into each frame buffer after it changes.

## Simulator

`rake ui:dev` builds `ui/build/ui.wasm` and serves http://127.0.0.1:8790/ (`PORT=` to change it). It rebuilds in under a second when a file under `ui/` changes, and the page reloads itself. The page has controls for the channel, decoding, lock, signal level, sync noise and USB state, and a mode: analog, or HDZero, which is theoretical until the receiver decodes it. In HDZero mode the picture is 16:9 letterboxed in the 630x480 area (the analog clip cropped, as no HDZero footage is decoded yet), the strip shows the format (720p60, 540p90 or 1080p30) and a link-quality bar, and a tap steps through HDZero's channels (R1-R8, F2, F4). The P4 always reports analog. Mouse presses on the panel are touches.

The build uses Xcode's clang (`/usr/bin/clang --target=wasm32`) and, as the linker, `rust-lld -flavor wasm` from the active rustup toolchain. The README says how to install both.

## On the P4

The strip costs nothing per frame: the picture's PPA pass writes only its own 630 columns, so each buffer keeps the strip until it changes. A change costs one 3.0-3.5 ms PPA pass per buffer. Drawing the strip with the CPU instead took 12.5 ms a copy from PSRAM and dropped the panel to 41 fps. `tools/lcdgrab.py OUT.png` saves what the panel shows ([experiment](../experiments/2026-11-16-lcd-ui-strip.md)).

Touch is not wired up yet. The tap that steps the channel works only in the simulator.
