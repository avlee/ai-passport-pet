<p align="right">
  <strong>English</strong> · <a href="codex-pet.zh_CN.md">简体中文</a>
</p>

# Codex Pet

Turns the AI Passport into a **second screen** for the little pet that lives in the Codex / ChatGPT desktop app: the animation, status, and text on the device track Codex on your Mac in real time, and the pet falls asleep when the link drops.

This is the application delivered on the `feature/codex-pet` branch. It does not change the public surface of `components/bsp`.

## What it looks like

- A pet stage, drawn 1:1 from the atlas pixels with no scaling. The stage has no backdrop: the pet stands directly on the page background, with the platform and a contact shadow providing the ground line.
- Top row: a status dot plus status text (offline / connecting / idle / working / waiting / ready / failed) on the left, battery on the right.
- A platform at the bottom, carrying the text Codex pushed as its front face; when there is no text it shows a placeholder for the current state. Its floor edge is pinned to the pet's feet (stage bottom row), so the pet reads as standing on it.
- Sleep (link down): the pet is dimmed to 40%, frame timing slows to 250%, backlight drops to 45%, and `Zzz` appears in the top-right of the stage.

Buttons:

| Action | Behaviour |
| --- | --- |
| UP click | Plays a local wave |
| DOWN click | Plays a local jump |
| OK click | Plays a jump and sends `poke` to the Mac; closes the info panel when it is open |
| OK long press | Opens / closes the info panel (firmware version, pet package, link, network, bridge endpoint, battery) |
| UP long press | Enters the BSP reference demo menu; long-press OK on its root menu returns to the pet |

## Module layout

Hardware stays in `components/bsp`; the application lives entirely in `main/`:

| File | Responsibility |
| --- | --- |
| `main/main.c` | Entry point: I2C and display/LVGL init, then hands over to `pet_app` |
| `main/pet_app.c` | Application wiring: buttons, battery polling, bridge events, string coverage self-check |
| `main/pet_ui.c` | Pet screen and animation driver (the single draw entry point) |
| `main/pet_state.c` | Link/Codex state → animation row mapping (pure logic) |
| `main/pet_protocol.c` | Wire-protocol parser (pure logic) |
| `main/pet_bridge.c` | Wi-Fi STA + TCP client + backoff reconnect |
| `main/pet_settings.c` | NVS persistence for connection parameters |
| `main/pet_atlas.c` | Atlas access layer wrapping frame tables into `lv_image_dsc_t` |
| `main/pet_atlas_validate.c` | Atlas self-consistency validation (pure logic) |
| `main/pet_config.h` | Compile-time defaults (SSID / bridge endpoint / timeouts / backlight) |
| `main/pet_strings.h` | Single source of truth for UI strings (X-macro list shared by self-check and tests) |
| `main/pet_fonts.c` | Application font entry points (16 px body / 20 px title + fallback) |
| `main/demo_menu.c` | The original `main.c` demo menu, moved verbatim into its own file |

`pet_state.c`, `pet_protocol.c`, `pet_atlas_validate.c`, and `pet_strings.h` deliberately do not depend on ESP-IDF or LVGL, so they run under host unit tests.

## Pet atlas (v2 format)

The source is a ChatGPT / Codex v2 pet atlas: 1536×2288 WebP, 8 columns × 11 rows, 192×208 cells. The 57 frames cover 9 state animations (idle 6, running_right 8, running_left 8, waving 4, jumping 5, failed 8, waiting 6, running 6, review 6), each frame carrying its own duration in milliseconds.

Generate the firmware assets:

```bash
python3 tools/gen_pet_assets.py \
    --atlas assets/pets/<pet-id>/spritesheet.webp \
    --pet-id <pet-id> \
    --out-bin main/assets/pet_<pet>_portrait.bin \
    --out-header main/pet_atlas_<pet>_portrait.h \
    --out-source main/pet_atlas_<pet>_portrait.c \
    --preview-dir assets/pets/<pet-id>/preview
```

Notes:

- **Per-frame tight crops.** The character occupies only 54–129 px of the 192 px cell, so cropping to the non-transparent bounding box and remembering the crop offset is pixel-identical to whole cells while using roughly 3× less flash.
- **RGB565A8 pixel format** (2 colour bytes + 1 alpha byte, 3 bytes per pixel) so LVGL can blend transparency. `stride = w * 2` with the alpha plane right after the colour plane — see `img_width_to_stride()` in `lvgl/src/draw/lv_image_decoder.c`.
- Current pet `sophie-portrait`: 57 frames, 2,551,288 bytes (2.43 MiB), 129×198 draw stage.

## Animation driver

`main/pet_ui.c` has exactly one draw entry point, `render_frame()`, so frame synchronisation only has to be reasoned about in one place:

1. Every frame has a different crop origin inside its atlas cell. Subtracting the stage origin (`PET_ATLAS_STAGE_X/Y`) and setting that as the `lv_image` position is what puts it in the right place on screen — the horizontal motion inside an animation comes from the source material, matching the ChatGPT rendering, not from tweening we add.
2. The delay until the next frame is taken straight from that frame's `duration`. No interpolation, no fixed frame rate. Sleep multiplies it by `PET_STATE_SLEEP_SPEED_PCT`.
3. One-shot animations (the wave on connect, the jump celebrating completion, button emotes) fall back automatically once they finish: state-machine transitions are advanced by `pet_state_oneshot_complete()`, user-triggered ones are handled by an override flag in the UI.
4. The frame rate is entirely determined by the material (110–320 ms, roughly 3–9 fps). The 129×198 stage is about 33% of the screen, leaving several times the needed headroom at 40 MHz SPI with a 240×20 DMA buffer.

### State → animation mapping

| Link | Codex state | Looping animation | Notes |
| --- | --- | --- | --- |
| offline | — | `idle` | Asleep: dimmed 40%, 250% frame timing; Codex state is ignored meanwhile |
| connecting | — | `waiting` | Connecting Wi-Fi / TCP |
| online | idle | `idle` | |
| online | working | `running` | Working in place |
| online | waiting | `waiting` | Waiting for your confirmation |
| online | ready | `review` | Plays one `jumping` celebration first |
| online | failed | `failed` | |

On a fresh connection the pet plays one `waving` to say hello before settling into the animation for the current state.

## Wire protocol

UTF-8 text over TCP, one message per line, flat JSON. Both `\n` and `\r\n` line endings are accepted.

Mac → device:

```json
{"type":"state","state":"working","text":"refactoring the login module"}
{"type":"text","text":"update the text only, leave the state alone"}
{"type":"ping"}
```

Device → Mac:

```json
{"type":"hello","fw":"0.1.0","pet":"sophie-portrait"}
{"type":"battery","soc":95}
{"type":"pong"}
{"type":"poke"}
```

`battery` is sent whenever the level changes, with `soc` set to `null` when it cannot be read. After a reconnect the last known value is re-sent right after `hello`; otherwise the menu bar would show nothing until the next poll (up to 5 seconds).

Parsing is **bounded and forgiving**: a per-line limit of `PET_PROTOCOL_LINE_MAX` (512 bytes) and a text-field limit of `PET_PROTOCOL_TEXT_MAX` (192 bytes), overlong lines are dropped and resynchronised at the next newline, unknown fields are ignored, and UTF-8 is only truncated on character boundaries. The device is the TCP client and the Mac is the server, so the device never needs to accept inbound connections and does not depend on mDNS discovery.

## The Mac side: Pet Bridge

`tools/pet_bridge.py` is the matching server that feeds Codex state to the device:

```bash
# Follow the most recently modified Codex session log, with manual commands available
python3 tools/pet_bridge.py

# Listen only; drive the state entirely by hand
python3 tools/pet_bridge.py --no-codex

# See which session file would be followed
python3 tools/pet_bridge.py --list-sessions
```

It follows whichever of `~/.codex/sessions/**/rollout-*.jsonl` was modified most recently and maps events to states:

| Codex event | Pet state |
| --- | --- |
| `event_msg/task_started` | `working` |
| `event_msg/item_completed` (command / reasoning / file change / message) | `working`, with the progress shown in the bubble |
| `event_msg/task_complete` | `ready`, bubble shows `last_agent_message` |
| Any event carrying `error` | `failed` |
| Approval / confirmation events | `waiting` |
| No events for `--idle-after` (45 s by default) | `idle` |

The server pings every 5 s, comfortably faster than the device's 12 s idle check, so a genuinely silent device only happens when the network really drops.

### The menu bar app (macOS)

`tools/menubar/` wraps that server into a native menu bar app: the icon tracks the state, and the menu shows the bridge / device / battery / Codex / bubble status plus manual state pushes, bubble text and a log-following toggle.

```bash
./tools/menubar/build.sh --run
```

It needs nothing but `swiftc` from the Command Line Tools — no third-party dependency, no Xcode project. The app writes **no network traffic of its own**: it runs `pet_bridge.py` as a child process (relaunching it with backoff if it dies), then reads status and sends commands over the local channel below. Quitting the app also stops the child. See `tools/menubar/README.md` for the details.

### Control channel (`--control`)

A local channel for GUI front ends: AF_UNIX plus JSON lines, both directions.

```bash
python3 tools/pet_bridge.py --control "$HOME/Library/Application Support/CodexPetBridge/bridge.sock"
```

A new client first receives a `snapshot` (the whole current state) and then incremental events: `bridge`, `link`, `state`, `text`, `session`, `watch`, `device`, `battery`, `error`. Commands are `state`, `text`, `raw`, `watch`, `snapshot` and `ping`.

This is an **event stream, not request/response**: a successful command sends no acknowledgement — its result is broadcast as the matching event — and only failures add an `error`. Clients should update state from events rather than pairing a send with a receive. The authoritative definition of the wire format remains the handful of JSON messages sent to the device above; the control channel only observes and drives, and adds no downstream traffic.

## Layout preview (no device needed)

```bash
python3 tools/preview_pet_screen.py          # one PNG per state plus a combined contact sheet
python3 tools/preview_pet_screen.py --anim idle --frame 3 --scale 3
```

Output lands in `assets/pets/<pet-id>/preview/screen-*.png`.

The tool renders **from the generated artefacts**, not from the source atlas: frame tables are
parsed out of `main/pet_atlas_<pet>_portrait.c`, pixels are read from
`main/assets/pet_<pet>_portrait.bin` — the exact bytes embedded in the firmware — and the layout
constants are parsed out of `main/pet_ui.c`. That makes it a check as well as a mockup: a wrong
crop offset or blob layout shows up immediately as a missing or misplaced chunk of the pet. Text uses
a system CJK font as a stand-in (the device uses subsetted Noto Sans CJK SC), so positions and sizes
are accurate while glyph shapes differ slightly.

## Configuration, build, and flashing

Connection parameters live in `main/pet_config.h`. To keep credentials out of the tracked file, create `main/pet_config_local.h` overriding the same macros (already in `.gitignore`):

```c
#define PET_WIFI_SSID "your-2.4g-ssid"   // the ESP32-C3 has no 5 GHz radio
#define PET_WIFI_PASSWORD "your-password"
#define PET_BRIDGE_HOST "192.168.1.10"   // your Mac's LAN IP: ipconfig getifaddr en0
#define PET_BRIDGE_PORT 8765
```

These values are only compile-time defaults. NVS wins when it holds a complete explicit override, and the defaults are deliberately **not** written back to NVS — otherwise changing `pet_config.h` and reflashing would still leave the old SSID in force.

```bash
source ~/esp/esp-idf-v5.5.3/export.sh
./tools/validate.sh --static      # repo checks + host tests (pet state machine / protocol / atlas / font coverage)
./tools/validate.sh --firmware    # build + verify + produce the merged image
./tools/validate.sh               # both

idf.py -B build flash             # normal flashing
esptool.py -p /dev/tty.usbmodem* write_flash 0x0 build/FoloToy-AI-Passport-full.bin
```

The merged image lands in `build/FoloToy-AI-Passport-full.bin` and can be written in one shot from `0x0` (bootloader, partition table, and application are all inside).

## Chinese fonts

The LVGL baseline ships only Montserrat, which has no CJK glyphs at all, so a Chinese UI has to bring its own font:

- `main/pet_strings.h` is the single source of truth for UI strings, listed as an X-macro manifest.
- `tools/gen_pet_fonts.py` generates `assets/fonts/pet_font_16.c` (GB2312 levels 1+2, 7029 codepoints), `pet_font_20.c` (level 1, 4021 codepoints), and the coverage manifest `pet_font_charset.json`.
- Two coverage gates: at boot `pet_app.c` looks every manifest entry up in the font and logs misses; on the host `tests/test_pet_font_coverage.py` checks the manifest. Adding a string only means editing `pet_strings.h`; both gates follow automatically.

See [lvgl-chinese-fonts.zh_CN.md](lvgl-chinese-fonts.zh_CN.md) for details.

## Resource usage

| Item | Size |
| --- | --- |
| Pet atlas blob (RGB565A8, 57 frames) | 2.43 MiB |
| `pet_font_16` glyph data (GB2312 levels 1+2) | ~0.5 MiB |
| `pet_font_20` glyph data (GB2312 level 1) | ~0.4 MiB |
| `factory` app partition | 8,323,072 bytes (`partitions.csv`) |

The ESP32-C3 has no PSRAM, so the atlas and fonts stay in flash and are read directly by LVGL (`lv_image` references the external `lv_image_dsc_t` without copying). The LVGL memory pool is still `CONFIG_LV_MEM_SIZE_KILOBYTES=24`; this UI uses roughly 35 objects, which fits.

## Assets and licensing

Recorded as `assets/README.md` requires:

| Path | Contents | Source / licence | Integration |
| --- | --- | --- | --- |
| `assets/pets/sophie-portrait/spritesheet.webp`, `pet.json` | v2 pet source atlas | Copied from `~/.codex/pets/sophie-portrait` on this machine. **The upstream licence is not stated**, so confirm it before redistributing publicly | Input to the generator; not compiled |
| `assets/pets/sophie-portrait/preview/*.png` | One preview frame per animation | Same as above | Human review only; not compiled |
| `assets/fonts/pet_font_16.c`, `pet_font_20.c` | Generated CJK glyph data | Subset-generated by `tools/gen_pet_fonts.py` from Noto Sans CJK SC (SIL OFL 1.1) | `target_sources` in `main/CMakeLists.txt` |
| `assets/fonts/pet_font_charset.json` | Font codepoint coverage manifest | Same as above | Read by `tests/test_pet_font_coverage.py` |
| `main/assets/pet_sophie_portrait.bin` | Per-frame cropped RGB565A8 frame data | Derived from the source atlas in `assets/pets/` | `EMBED_FILES` in `main/CMakeLists.txt` |
| `main/pet_atlas_sophie_portrait.{h,c}` | Generated frame tables | Same as above | Compiled into the firmware |

Two things worth knowing:

- The generated artefacts (`.bin`, generated `.c`/`.h`, manifest) are all committed, so a **fresh clone builds without the source atlas**; the atlas is only needed to regenerate or to swap pets.
- Swapping pets means updating three places: the generated header included by `main/pet_atlas.h`, the `extern` symbol name in `main/pet_atlas.c`, and the `EMBED_FILES` path in `main/CMakeLists.txt`.
