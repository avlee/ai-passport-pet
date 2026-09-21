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
- Screen power: idle for `PET_SCREEN_IDLE_MS` and the backlight goes off; a Bridge message, a key press, or the link coming back turns it on again. See [Screen power](#screen-power).
- With an empty slot, the stage shows "no pet yet" centred and the platform says "install one from the menu bar app" — the layout is identical to the reference pet's, so "empty" reads as a state rather than as "failed to boot".

Buttons:

| Action | Behaviour |
| --- | --- |
| UP click | Plays a local wave |
| DOWN click | Plays a local jump |
| OK click | Plays a jump and sends `poke` to the Mac; closes the info panel when it is open |
| OK long press | Opens / closes the info panel (firmware version, pet package, link, network, bridge endpoint, battery) |
| UP long press | Enters the BSP reference demo menu; long-press OK on its root menu returns to the pet |

## Screen power

Between two Codex runs nothing changes on screen, so the backlight goes off instead of burning a 520 mAh cell. The rules, and why each one is there:

- **Idle off.** `PET_SCREEN_IDLE_MS` (default 60 s) without activity takes the backlight to 0. The panel keeps its frame memory, so waking is instant and needs no repaint.
- **Wake on content.** A Bridge state/text message, any key press, or the link coming back lights the screen and restarts the clock.
- **Keepalive pings do not count.** The Bridge pings every 5 s (`PING_INTERVAL_S` in `tools/pet_bridge.py`). Counting those as activity would mean the screen never turns off at all.
- **Neither does losing the link.** The pet already dims and slows down when offline, and the reconnect loop reports `CONNECTING` on every round — waking on those would keep the screen lit throughout a network outage.
- **Never while it must be seen.** The provisioning page, the pet transfer screen, and the demo menu hold the screen on at full brightness (`pet_screen_set_hold`) — the pairing code shows up while the device is still offline, so deriving the level from the link would put it on a 45% screen. Releasing the hold also counts as activity, so the line the transfer screen leaves behind (success or failure) gets its full window rather than being switched off on the spot.
- **Off means off.** While the screen is dark, `pet_ui_set_anim_enabled(false)` stops advancing frames: each frame pushes the 129x198x2-byte sprite over SPI, and painting an invisible screen is pure waste.

The decision lives in `main/pet_screen.c` — pure logic, so `tests/test_pet_screen.c` pins the deadline, the hold semantics, and the three backlight levels (0 / sleep / active) on the host. Within the pet application `pet_app.c` is the only place that writes the backlight, and `apply_screen()` is the only function in it that does so; the BSP reference demo menu keeps its own writes, having been moved in verbatim.

## Module layout

Hardware stays in `components/bsp`; the application lives entirely in `main/`:

| File | Responsibility |
| --- | --- |
| `main/main.c` | Entry point: I2C and display/LVGL init, then hands over to `pet_app` |
| `main/pet_app.c` | Application wiring: buttons, battery polling, bridge events, string coverage self-check |
| `main/pet_ui.c` | Pet screen and animation driver (the single draw entry point), plus the transfer screen used while swapping pets |
| `main/pet_state.c` | Link/Codex state → animation row mapping (pure logic) |
| `main/pet_protocol.c` | Wire-protocol parser (pure logic) |
| `main/pet_pkg.c` | `.pet` package structural parsing and validation (pure logic) |
| `main/pet_slot.c` | The `pets` partition: erase, write, mmap, validate |
| `main/pet_layout.c` | Layout: stage centring, foot-on-platform alignment, per-frame landing (pure logic) |
| `main/pet_screen.c` | Screen power: idle auto-off and wake-on-content decisions, backlight levels (pure logic) |
| `main/pet_bridge.c` | Wi-Fi STA + TCP client + backoff reconnect + package intake |
| `main/pet_settings.c` | NVS persistence for connection parameters |
| `main/pet_atlas.c` | Atlas access layer wrapping the slot's frame tables into `lv_image_dsc_t` |
| `main/pet_config.h` | Compile-time defaults (SSID / bridge endpoint / timeouts / backlight) |
| `main/pet_strings.h` | Single source of truth for UI strings (X-macro list shared by self-check and tests) |
| `main/pet_fonts.c` | Application font entry points (16 px body / 20 px title + fallback) |
| `main/demo_menu.c` | The original `main.c` demo menu, moved verbatim into its own file |

`pet_state.c`, `pet_protocol.c`, `pet_pkg.c`, `pet_layout.c`, `pet_screen.c`, and `pet_strings.h` deliberately do not depend on ESP-IDF or LVGL, so they run under host unit tests.

## The pet package (`.pet`) and single-slot swapping

The pet is **no longer compiled into the firmware**. It is a self-describing package living in its own `pets` partition, pushed there by the Mac-side Pet Bridge over the same TCP link — so swapping a pet neither recompiles nor reflashes anything.

The source material is still a ChatGPT / Codex v2 pet atlas: 1536×2288 WebP, 8 columns × 11 rows, 192×208 cells. The 57 frames cover 9 state animations (idle 6, running_right 8, running_left 8, waving 4, jumping 5, failed 8, waiting 6, running 6, review 6), each frame carrying its own duration in milliseconds.

Building a package (byte-for-byte against `main/pet_pkg.h` on the device side):

```bash
python3 tools/gen_pet_package.py --list                    # what is under ~/.codex/pets
python3 tools/gen_pet_package.py --pet sophie-portrait     # writes build/pets/<id>.pet
python3 tools/gen_pet_package.py --pet ~/.codex/pets/sophie --out /tmp/s.pet
```

| Offset | Contents |
| --- | --- |
| `+0` | 120-byte header: magic `PET1`, version, total size, frame and state counts, stage union, `pet_id`, `display_name` |
| `+frames_offset` | Frame table, `frame_count × 16` bytes (offset + x/y/w/h + duration) |
| `+states_offset` | State table, `state_count × 20` bytes (name + first frame + frame count) |
| `+blob_offset` | Per-frame RGB565A8 pixels |

Each of the three regions has its own CRC32: header, tables (frame and state tables laid out as one contiguous run), and pixels. Keeping the tables contiguous is deliberate — one CRC then covers both, and a misplaced state table is the hardest failure to spot on screen.

Notes:

- **Per-frame tight crops.** The character occupies only 54–129 px of the 192 px cell, so cropping to the non-transparent bounding box while remembering the crop origin is pixel-identical to whole cells while using roughly 3× less space.
- **RGB565A8 pixel format** (2 colour bytes + 1 alpha byte, 3 bytes per pixel) so LVGL can blend transparency. `stride = w * 2` with the alpha plane right after the colour plane — see `img_width_to_stride()` in `lvgl/src/draw/lv_image_decoder.c`.
- **The stage is computed at runtime.** The header records the union of every frame's crop box (129×198 for the reference pet, `sophie-portrait`), and `pet_layout_for_stage()` centres it horizontally and derives its top edge by aligning the pet's feet with the platform — so a wider or taller pet stands on the same platform instead of dragging it around. A package whose stage falls outside the usable range (`main/pet_layout.h`) is refused at load time rather than drawn out of bounds.
- Current pet `sophie-portrait`: 57 frames, 2,551,288 bytes (2.43 MiB), 129×198 stage.

### Slot semantics

The `pets` partition holds exactly one pet (see [firmware-layout](firmware-layout.md)), so swapping is an **overwrite**, not a side-by-side install:

1. The device erases the header first and only then writes the package in chunks, so "erased halfway" and "empty slot" are the same state — a half-written package is never mistaken for a valid one.
2. Losing power or the link mid-transfer leaves the slot **empty**, and the device shows "no pet yet / install one from the menu bar app". There is no rollback; just send it again.
3. Before finishing, the device checks the declared length and the whole-package CRC32, then `esp_partition_mmap`s **only the bytes the package actually occupies** and unmaps immediately — mapping the full 3.94 MB would burn MMU pages it shares with the app's rodata.
4. On success the device re-sends `hello`, whose `pet` field reports the pet that is **actually** in the slot.

While the transfer runs the device shows a different screen (below), because the pet UI has to be torn down first: `lv_image` is still referencing the flash that is about to be rewritten.

### The pet transfer screen

Swapping a pet means tearing the whole pet UI down first: `lv_image` references memory mapped from the very flash being rewritten, and mapping while erasing is undefined behaviour. But the screen must not sit black either — a 1–3 MB package takes tens of seconds over 2.4 GHz Wi-Fi, and a user who cannot tell whether the device is working will pull the plug, leaving half a package behind.

So this screen goes up instead: title, pet id, progress bar, one status line. It references no atlas pixels, which is why it can outlive the unmapping. Its layout constants are the `TRANS_*` macros in `main/pet_ui.c` (integer literals like the rest, so the preview tool can read them).

- An unknown length does not fake progress: only the empty track is drawn.
- The reason for a failure (bad package, CRC mismatch, link dropped) stays on screen along with a retry hint.
- When it finishes the pet UI comes back; if the slot ended up empty, that screen says "no pet yet / install one from the menu bar app" rather than showing nothing.

## Animation driver

`main/pet_ui.c` has exactly one draw entry point, `render_frame()`, so frame synchronisation only has to be reasoned about in one place:

1. Every frame has a different crop origin inside its atlas cell. What must be subtracted is the **crop origin of the stage inside the cell** (the header's `stage_x/stage_y`, i.e. the union of every frame's crop box); the result is that frame's landing point inside the stage, and the horizontal motion inside an animation comes from the source material, matching the ChatGPT rendering, not from tweening we add. It is **not** the `stage_x/stage_y` that `pet_layout_for_stage()` computes — that is also a "stage origin", but it is the stage's position **on screen**, tens of pixels away from the cell coordinate system (for the 129 px wide reference pet: 31 in the cell, 55 on screen). Mixing the two compiles fine and never crashes; it just shifts the pet up and to the left and lets the stage clip a corner off — which is exactly what the first on-device pet swap did. The subtraction therefore lives in one place, `pet_layout_frame_rect()`, which refuses to draw a frame whose crop box is not contained in the stage's.
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
{"type":"limits","primary":98,"weekly":31}
{"type":"pet","id":"sophie-portrait","size":2551684,"crc32":2181165281}
```

Device → Mac:

```json
{"type":"hello","fw":"0.1.0","pet":"sophie-portrait"}
{"type":"battery","soc":95}
{"type":"pong"}
{"type":"poke"}
{"type":"petdone","id":"sophie-portrait","ok":true}
```

`battery` is sent whenever the level changes, with `soc` set to `null` when it cannot be read. After a reconnect the last known value is re-sent right after `hello`; otherwise the menu bar would show nothing until the next poll (up to 5 seconds).

`pet` is the one message that is **followed by raw bytes**: the `size` bytes after the announcement line are the `.pet` package itself (not base64 — that would cost another 33%), and the device is in "binary mode" for the duration, counting bytes instead of parsing lines. The bridge therefore holds its send lock for the whole transfer: any JSON slipped in would land inside the payload and misalign the package from then on. The device answers with `petdone` — **until then the package has only been sent, not installed** — and `ok` false means the slot is empty.

### Usage limits (`limits`)

Codex has no local command to query subscription usage, but every server response carries a rate-limit snapshot, and Codex writes those snapshots into its session rollouts (`event_msg/token_count` records). The bridge reads them and forwards `{"type":"limits","primary":<0-100>,"weekly":<0-100>}` — the **used** percentage of the 5-hour window (`primary`, 300 minutes) and the weekly window (`secondary`, 10080 minutes). A window without a usable snapshot is omitted from the message; a line with no window at all is dropped by the parser.

Staleness is handled on both sides:

- The bridge re-scans the most recent rollouts every 30 seconds and treats an expired `resets_at` as 0% used — the window has rolled over, and any request after that would have produced a newer snapshot.
- The device hides a window that has no value and hides both gauges while the link is offline (a stale gauge would be a lie). Values are cached, so a rebuild (demo menu, pet swap) restores them, and the bridge re-sends after a device reconnect.

On screen the two windows are vertical gauges hugging the left (`5h`) and right (`7d`) screen edges, filled bottom-up by **remaining** percentage and color-graded green/yellow/red as remaining drops below 50%/20%. `tools/preview_pet_screen.py --limits 98/31` renders them without a device.

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

How that file is found matters more than it looks: the follow loop asks again four times a second, so only the most recent dated directories are scanned (with a recursive-glob fallback when they come up empty). See `CodexWatcher.newest_session()`.

| Codex event | Pet state |
| --- | --- |
| `event_msg/task_started` | `working` |
| `event_msg/item_completed` (command / reasoning / file change / message) | `working`, with the progress shown in the bubble |
| `event_msg/task_complete` | `ready`, bubble shows `last_agent_message` |
| Any event carrying `error` | `failed` |
| Approval / confirmation events | `waiting` |
| No events for `--idle-after` (45 s by default) | `idle` |

The server pings every 5 s, comfortably faster than the device's 12 s idle check, so a genuinely silent device only happens when the network really drops.

Codex does not put message text in a plain field. An `AgentMessage` carries its body as a list of content blocks — `[{"type": "Text", "text": "..."}]` — and an error carries `{"message": ..., "codex_error_info": ...}`. Every Codex field that becomes bubble text goes through `message_text()`, which unwraps the inner text and returns nothing for a shape it does not recognise. It deliberately has **no `str()` fallback**: serialising the payload is exactly how a `[{'type': 'Text', ...}]` blob ended up on the platform in place of the progress text.

The same rule covers the item type itself. `item_completed` carries more types than the bridge names — `McpToolCall`, `WebSearch`, `Plan`, `ImageView`, `ContextCompaction` and `SubAgentActivity` among them, 2423 of the 62127 such records on this machine — and an unrecognised type falls back to the generic working text. It used to be sent through verbatim, so the bubble flipped between CamelCase English identifiers while the task ran: those mean nothing to someone reading a Chinese bubble, and Codex adds and removes them between versions. A bubble shows either text Codex actually produced or one of our own state strings; there is no third option.

### Swapping pets

At startup the bridge scans `~/.codex/pets` and broadcasts the list of available pets (the `pets` event) alongside the rest of its state. Picking one builds the package on demand — through `tools/gen_pet_package.py`'s implementation, not a second copy, because the package format is a cross-language contract with exactly one source of truth — caches it in `~/Library/Application Support/CodexPetBridge/pets/<id>.pet`, and pushes it to the device from a **background thread**.

- The cache key is the source atlas's name, size, and modification time: an untouched atlas is never re-sliced (one 3 MB pet means 57 frames converted pixel by pixel to RGB565).
- The transfer has to stay off the control channel's read thread. It takes tens of seconds, and blocking there makes the GUI look like it ignored the click.
- Progress goes out as `petxfer` events (`preparing`, `sending` with a byte count, `sent`) and the device's verdict arrives as `petdone`. **Do not treat `sent` as success** — the device is still checking the CRC at that point.
- Slicing frames needs Pillow, while `pet_bridge.py` itself uses only the standard library. That combination means **the bridge starts happily with the wrong interpreter and only pet swapping fails**. Such a failure must name the interpreter: `gen_pet_package` imports PIL lazily (so `tests/test_pet_package.py` still runs on a machine without Pillow), which means that `ImportError` never surfaces from `import gen_pet_package` — catching it there only ever produced a bare "internal error". The bridge now probes `import PIL` up front and reports a `packer` object (`available`, `python`, `pillow`, `hint`) inside the `pets` event, so the menu can explain the situation **before** anything is clicked.
- `pets.last` is the previous outcome, and a client attaching midway uses it to align the progress line. When the device returns its verdict the bridge must therefore settle it into a terminal state and **drop `petxfer` from `ControlHub`'s latest-event cache**: that event carries progress, not state, and leaving its final `sent` frame behind makes a late-attaching client replay the snapshot (the menu bar merges events sorted by name, with `petdone` ahead of `petxfer`) straight back to "waiting for the device". The same step re-scans the pet list, because the cache entry only appears once packing succeeded — otherwise the pet you just installed still reads as unpacked.

```bash
python3 tools/pet_bridge.py --pets-dir ~/.codex/pets    # a different set of sources
python3 tools/pet_bridge.py --pet-cache /tmp/pets       # a different cache directory
python3 tools/pet_bridge.py --no-pets                   # disable the whole feature (device flashed without a pets partition)
```

### The menu bar app (macOS)

`tools/menubar/` wraps that server into a native menu bar app: the icon tracks the state, and the menu shows the bridge / device / battery / Codex / bubble status plus manual state pushes, bubble text and a log-following toggle. A "pets" submenu lists the pets available in Codex, ticks the one currently on the device, and swaps on selection (the transfer percentage appears in the status item title). It has an "About" dialog that shows the interpreter the bridge runs on and whether it has Pillow, but **no way to edit it**: one wrong character in the path, or deleting that virtual environment later, shows up only as pet swapping failing, with no way back to the previous one. When no interpreter has Pillow, the pets submenu states the reason at the top and disables the pets that have not been packed yet, instead of letting you fail one pet at a time; to point at another interpreter, use `defaults write local.codex-pet.bridge pythonPath <path>` (`defaults delete` returns to auto-detection). See `tools/menubar/README.md`.

```bash
./tools/menubar/build.sh --install    # build, install into /Applications, restart
./tools/menubar/build.sh --run        # or just build and launch the build/menubar copy
```

It needs nothing but `swiftc` from the Command Line Tools — no third-party dependency, no Xcode project. The app writes **no network traffic of its own**: it runs `pet_bridge.py` as a child process (relaunching it with backoff if it dies), then reads status and sends commands over the local channel below. Quitting the app also stops the child.

The build copies `pet_bridge.py` and `gen_pet_package.py` into the bundle at `Contents/Resources/bridge/`, and compiles the bridge port in from the firmware header. The app **reads nothing from the source directory at runtime** — that bundled copy is its only script source, which is what lets it run from `/Applications`, or on a machine that never had this repo at all. Editing the script or the port therefore means rebuilding (`--install` replaces the installed copy and restarts in one step). `python3` and Pillow stay external requirements. See `tools/menubar/README.md` for the details.

### Control channel (`--control`)

A local channel for GUI front ends: AF_UNIX plus JSON lines, both directions.

```bash
python3 tools/pet_bridge.py --control "$HOME/Library/Application Support/CodexPetBridge/bridge.sock"
```

A new client first receives a `snapshot` (the whole current state) and then incremental events: `bridge`, `link`, `state`, `text`, `session`, `watch`, `device`, `battery`, `pets`, `petxfer`, `petdone`, `error`. Commands are `state`, `text`, `raw`, `watch`, `snapshot`, `ping`, `petlist`, and `pet` (with `id`).

`pets`, `petxfer` and `petdone` are three separate kinds of device fact and must **not** be folded into `device`: `ControlHub` keeps only the newest event per kind, so mixing them would evict the firmware and package reported by `hello`. The same reasoning applies to any new device fact — give it a new event name.

This is an **event stream, not request/response**: a successful command sends no acknowledgement — its result is broadcast as the matching event — and only failures add an `error`. Clients should update state from events rather than pairing a send with a receive. The authoritative definition of the wire format remains the handful of JSON messages sent to the device above; the control channel only observes and drives, and adds no downstream traffic.

## Layout preview (no device needed)

```bash
python3 tools/preview_pet_screen.py                     # every screen, one PNG each, plus a contact sheet
python3 tools/preview_pet_screen.py --pet sophie         # render a different pet
python3 tools/preview_pet_screen.py --anim idle --frame 3 --scale 3
python3 tools/preview_pet_screen.py --screen nopet,transfer,prov   # the pet-independent screens only
```

Output lands in `assets/pets/<pet-id>/preview/screen-*.png`. Besides the six state screens there are the empty slot, the pet transfer screen (`--transfer-percent` / `--transfer-failed`), and the Bluetooth provisioning screen.

The tool **copies not a single value** — everything is read at the source: pixels and frame tables come from a `.pet` package (built on the fly by default, or `--package` for an existing one), layout constants are parsed out of `main/pet_layout.{h,c}` and `main/pet_ui.c`, colours out of the `COL_*` macros in `pet_ui.c`, strings out of `main/pet_strings.h`, and line heights and baselines out of `assets/fonts/pet_font_{16,20}.c`. That also makes it a check: a wrong crop offset in the packer shows up immediately as a missing chunk of the pet.

Text is placed the way LVGL places it: **baseline = line top + `line_height` − `base_line`** (see `lv_draw_label.c`). Glyphs come from a system CJK font (the device uses subsetted Noto Sans CJK SC), so shapes differ slightly while positions and sizes are accurate.

It re-implements `pet_layout_for_stage()` in Python — there is no C compiler in an editor — and two implementations drifting apart would only mean a lying mockup, so `tests/test_pet_layout_mirror.py` compares it field by field against the same C code the device runs, with `tests/dump_pet_layout.c` as the harness.

Two formulas are compared: every layout field, and each frame's landing point (the subtraction in `pet_layout_frame_rect()`). The second one was added after the fact — the device used to subtract the wrong base (the on-screen stage position instead of the cell crop origin), so the mockup was right while the real screen was shifted 24/33 px with a corner clipped, and the comparison at the time only covered layout fields, leaving that formula with nothing to be checked against.

## Configuration, build, and flashing

Connection parameters live in `main/pet_config.h`. To keep credentials out of the tracked file, create `main/pet_config_local.h` overriding the same macros (already in `.gitignore`):

```c
#define PET_WIFI_SSID "your-2.4g-ssid"   // the ESP32-C3 has no 5 GHz radio
#define PET_WIFI_PASSWORD "your-password"
#define PET_BRIDGE_HOST "192.168.1.10"   // your Mac's LAN IP: ipconfig getifaddr en0
#define PET_BRIDGE_PORT 8765
```

These values are only compile-time defaults. NVS wins when it holds a complete explicit override, and the defaults are deliberately **not** written back to NVS — otherwise changing `pet_config.h` and reflashing would still leave the old SSID in force.

You do not have to put a real SSID here at all. Leaving the placeholder in place makes the device come up unprovisioned and offer Bluetooth pairing instead — see [Bluetooth provisioning](#bluetooth-provisioning).

```bash
source ~/esp/esp-idf-v5.5.3/export.sh
./tools/validate.sh --static      # repo checks + host tests (pet state machine / protocol / package format / layout / fonts)
PET_PYTHON=~/venv/bin/python3 ./tools/validate.sh --static   # only with Pillow does the atlas-to-.pet stage run
./tools/validate.sh --firmware    # build + verify + produce the merged image
./tools/validate.sh               # both

idf.py -B build flash             # normal flashing
esptool.py -p /dev/tty.usbmodem* write_flash 0x0 build/FoloToy-AI-Passport-full.bin
```

The merged image lands in `build/FoloToy-AI-Passport-full.bin` and can be written in one shot from `0x0` (bootloader, partition table, and application are all inside).

## Bluetooth provisioning

Values in `pet_config_local.h` are a build-time constant: change networks and you have to edit the file, rebuild, and reflash. The device can instead take its Wi-Fi parameters over BLE from the menu bar app, which reads them off the Mac it is already running on.

How the pieces fit together:

- The firmware ships with the placeholder SSID (`PET_WIFI_PLACEHOLDER_SSID`). `pet_settings_is_configured()` treats that placeholder as "never provisioned", so the device opens the provisioning window at boot and starts advertising. A real SSID in `pet_config_local.h` still connects directly, exactly as before; either way, long-press **DOWN** toggles the window — pressing it again while the window is up closes it, which is what the hint along the bottom of the provisioning screen promises. That window is opaque, covers the whole screen, and deliberately ignores every other key while it is up, so a window that cannot be closed reads as a dead device; the hint and the handler have to change together (`tests/test_pet_button_semantics.py` pins them together).
- The screen shows a 4-digit pairing code. It **rotates per window**: opening the provisioning screen generates a fresh code and stores it in NVS (`rotate_pin()` in `pet_provision.c`); reconnects within the same window reuse it, and it dies with the window — so stale codes from serial logs or NVS never pair again. Exhausting `PET_PROVISION_MAX_ATTEMPTS` also rotates immediately and drops the link — that is no longer a typo, it is someone trying. Rotation costs the user nothing: provisioning always involves reading the code off the screen.
- The attempt counter does **not** reset per connection. A fresh code at window open resets the counter with it, but reconnects within the same window get no free tries — "same code plus three free tries per connection" is a code that can be ground down.
- Only once the code is accepted will the device store anything. Commands are single-key JSON lines — `{"pin":"1234"}`, `{"ssid":"…"}`, `{"pass":"…"}`, `{"host":"…"}`, `{"port":8765}`, `{"commit":true}`, `{"forget":true}` — sent one field at a time so each can be acknowledged separately and a failure points at one field.
- `{"commit":true}` saves to NVS and rebuilds the bridge link, then reports the IP the device obtained. The password is never echoed back in any status message. On success the device shows that address for `PROV_CLOSE_AFTER_DONE_MS` (3 s) and then **leaves the provisioning screen on its own**; on failure the error stays on screen so it can be read or retried. The teardown must go through `pet_provision_stop()` — just deleting the task leaves `s_running` true and never emits `PET_PROV_OFF`, so the provisioning screen would stay on top of the pet forever.
- `{"forget":true}` clears the **connection parameters** in NVS and returns the device to the unprovisioned state. The pairing code is not part of that: it decides who may talk to the device, which is a different question from which network it joins.

The pairing code is an application-layer check, not BLE-layer encryption: it keeps a nearby bystander from pushing credentials, and nothing more. Treat provisioning as a convenience on a trusted LAN, not as an authorization scheme.

`pet_provision_parse.c` holds the parsing and status-building logic and deliberately does not depend on NimBLE, so `tests/test_pet_provision.c` exercises it on the host — command framing, field length limits, and the rejection paths. Two bugs have already been caught there: the status writer escaping its own JSON structural quotes, and UUID parsing that ran before `nimble_port_init()` (see below).

The Mac side talks to the device with CoreBluetooth (`tools/menubar/Sources/ProvisionClient.swift`), so the app keeps its zero-third-party-dependency property. The first use triggers the usual macOS Bluetooth permission prompt.

Two ordering constraints are easy to get wrong:

- UUID parsing must happen **after** `nimble_port_init()`. `ble_uuid_from_str()` reaches `ble_uuid_base_init()`, which allocates 16 bytes through the NimBLE allocator; call it earlier and it returns `BLE_HS_ENOMEM`, so every UUID "fails to parse" — intermittently, which makes it look like a bad UUID string rather than a call-order problem.
- `pet_settings_load()` must run before the provisioning window opens, because the window decides from the loaded SSID whether the device is unconfigured.

## Chinese fonts

The LVGL baseline ships only Montserrat, which has no CJK glyphs at all, so a Chinese UI has to bring its own font:

- `main/pet_strings.h` is the single source of truth for UI strings, listed as an X-macro manifest.
- `tools/gen_pet_fonts.py` generates `assets/fonts/pet_font_16.c` (GB2312 levels 1+2, 7029 codepoints), `pet_font_20.c` (level 1, 4021 codepoints), and the coverage manifest `pet_font_charset.json`.
- Two coverage gates: at boot `pet_app.c` looks every manifest entry up in the font and logs misses; on the host `tests/test_pet_font_coverage.py` checks the manifest. Adding a string only means editing `pet_strings.h`; both gates follow automatically.

See [lvgl-chinese-fonts.zh_CN.md](lvgl-chinese-fonts.zh_CN.md) for details.

## Resource usage

| Item | Size |
| --- | --- |
| `factory` app partition | 4,194,304 bytes (`partitions.csv`) |
| App image (excluding any pet) | 3,244,544 bytes (~3.1 MiB), 77% of the partition |
| Pet package `.pet` (57 frames, RGB565A8) | 2.43 MiB (in the `pets` partition, **not** in the app) |
| `pets` data partition | 4,128,768 bytes (0x3f0000) |
| `pet_font_16` glyph data (GB2312 levels 1+2) | ~0.5 MiB |
| `pet_font_20` glyph data (GB2312 level 1) | ~0.4 MiB |

The ESP32-C3 has no PSRAM, so fonts stay in flash and are read directly by LVGL (`lv_image` references the external `lv_image_dsc_t` without copying). Pet pixels are `esp_partition_mmap`ed at load time and unmapped right after use. The LVGL memory pool is still `CONFIG_LV_MEM_SIZE_KILOBYTES=24`; this UI uses roughly 35 objects, which fits.

The `pets` partition holds one uncompressed pet, so "just carry a few" does not fit in 8 MB. That would take per-frame deflate (down to 0.86–1.04 MB per pet, at a peak cost of 75–90 KB of RAM) or giving up the OTA slot pair.

## Assets and licensing

Recorded as `assets/README.md` requires:

| Path | Contents | Source / licence | Integration |
| --- | --- | --- | --- |
| `assets/pets/sophie-portrait/spritesheet.webp`, `pet.json` | v2 pet source atlas | Copied from `~/.codex/pets/sophie-portrait` on this machine. **The upstream licence is not stated**, so confirm it before redistributing publicly | Input to `tools/gen_pet_package.py`; not compiled |
| `assets/pets/sophie-portrait/preview/screen-*.png` | One preview per screen | Same as above | Human review only; not compiled |
| `assets/fonts/pet_font_16.c`, `pet_font_20.c` | Generated CJK glyph data | Subset-generated by `tools/gen_pet_fonts.py` from Noto Sans CJK SC (SIL OFL 1.1) | `target_sources` in `main/CMakeLists.txt` |
| `assets/fonts/pet_font_charset.json` | Font codepoint coverage manifest | Same as above | Read by `tests/test_pet_font_coverage.py` |

Two things worth knowing:

- The firmware contains **no pet assets at all** any more. `main/assets/pet_*.bin` and the generated `pet_atlas_*.{h,c}` are gone, so `main/CMakeLists.txt` has no `EMBED_FILES`.
- A **fresh clone therefore builds without the source atlas**, and the firmware it produces has no pet in it: the device boots with an empty slot and waits for you to install one from the menu bar app. The atlas is only needed to build packages and previews.
- Swapping pets requires no source change, no rebuild, and no reflash — pick one from the "pets" submenu in the menu bar.
