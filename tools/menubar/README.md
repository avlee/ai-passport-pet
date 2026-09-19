<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Codex Pet Bridge (macOS menu bar app)

Pushes the Codex state from your Mac to the pet on the AI Passport, and shows an icon in the system menu bar.

It is **not** a second bridge implementation: the only implementation of the wire protocol lives in `tools/pet_bridge.py` (already cross-checked against the device-side parser), and this app does three things:

1. runs `pet_bridge.py` as a child process and supervises it (relaunching with backoff if it dies);
2. reads status and sends commands over the local `--control` channel;
3. renders that status in the menu bar icon and its drop-down menu.

## Build, install and run

```sh
./tools/menubar/build.sh                      # output lands in build/menubar/
./tools/menubar/build.sh --run                # build, then launch the build/menubar copy
./tools/menubar/build.sh --install            # build + install into /Applications + restart
./tools/menubar/build.sh --install-to <dir>   # same, into a different directory
```

The only requirement is `swiftc` from the Xcode Command Line Tools (`xcode-select --install`). There is no Xcode project and no third-party dependency.

`--install` is **the everyday command**: it builds, stops the running instance, replaces the installed copy, and starts it again. Stopping the instance also collects the bridge child process — `SIGTERM` skips AppKit's shutdown path, so killing only the app would leave an orphan holding the port and the new instance could not bind. Before replacing, it checks the target: if something is already there that is not this app (its `CFBundleIdentifier` does not match), the script refuses and exits rather than deleting someone else's `.app`.

`build/menubar/` and the install location are **two independent copies**: rebuilding only refreshes the former, so the installed one does not follow. After changing anything, either run `--install` again or you are still running the old build. To start it, double-click it, or add it under System Settings → General → Login Items — the app has **no login item of its own** (no LaunchAgent, no `SMAppService`).

Once launched, the icon appears on the right side of the menu bar. **Having no Dock icon is expected** (`LSUIElement=1`).

## Where the bridge script and port come from

**Decided at build time, read at runtime only from the app bundle.** The app does **not** read the source directory — a machine that installed it has no such directory, so the only things it can rely on are the ones carried in the bundle:

| What | Where it comes from |
| --- | --- |
| `pet_bridge.py` | copied into `Contents/Resources/bridge/` by `build.sh` |
| `gen_pet_package.py` | same, and it **must sit in the same directory** |
| Bridge port | extracted from `PET_BRIDGE_PORT` in `main/pet_config.h` at build time, compiled into the app |
| Version | same, taken from `PET_FIRMWARE_VERSION` |

Which leaves one rule to remember: **after editing `tools/pet_bridge.py`, or changing `PET_BRIDGE_PORT`, rebuild with `build.sh`** (use `--install` to replace the installed copy and restart in one command). That is the price of not reading the source directory at runtime.

The two scripts must share a directory because `pet_bridge.py` finds `gen_pet_package` through `sys.path[0]` and does no `sys.path` manipulation of its own.

### Why the app does not go looking in the repo

An earlier version allowed it: a menu item set the project directory, and the repo copy took priority over the bundled one, so editing the script and hitting "Restart bridge" was live without a rebuild. The price was a **runtime switch that only means anything on the author's machine** — nobody else has that checkout, so the item always read "unused"; the resolution logic had two sources to distinguish, the UI had to explain which copy was running, and it all had to tolerate the repo being moved. That development loop is now served by `build.sh --install`, which lives at build time and is far cleaner.

The bundled copy is a **build-time snapshot**, and there is no second copy in the repo: the single source of truth is still the one under `tools/`, and that one `cp` buys an app that no longer depends on the repo staying put.

**What this does not solve**: `python3` and Pillow are still required — the former is the interpreter the bridge runs on, the latter is what pet swapping needs to pack an atlas into a `.pet`. Making the `.app` self-contained down to the interpreter is a separate step (freezing the script into a single executable).

## Menu bar icon

| Icon | Meaning |
| --- | --- |
| 🐾 dimmed (`pawprint`) | device not connected |
| 🐾 solid (`pawprint.fill`) | device online, Codex idle |
| ⚙️ (`gearshape.2.fill`) | Codex working |
| ❗️ (`exclamationmark.bubble.fill`) | Codex waiting for your confirmation |
| ✅ (`checkmark.circle.fill`) | the turn finished |
| ❌ (`xmark.octagon.fill`) | failed |

Hovering shows the current state, the pack level, and the bubble text.

## Menu items

- **Configure Wi-Fi** (⌘W): opens the Bluetooth provisioning window. It prefills the SSID and LAN address of the Mac you are on, asks for the 4-digit code shown on the device screen, and pushes the credentials over BLE once the code is accepted. Use it whenever the device is on a different network from the one it was built for, or when it has never been provisioned at all. The title carries **no ellipsis**: it is shown verbatim on the device screen, whose status area is only 200px wide, so one extra character can wrap it (`tests/test_pet_ui_text_fit.py`).
- **Pets** (submenu): lists what is available under `~/.codex/pets`, ticks the one currently on the device, and swaps on selection. The bridge builds that pet's package on demand (needs Pillow — if the interpreter lacks it the submenu states the reason at the top and disables the pets that have not been packed yet) and pushes it from a background thread, with progress shown in the first submenu row and in the status item title (`transfer 43%`). The confirmation dialog warns that the device clears the slot before writing, so an interrupted transfer leaves it with no pet — just pick it again. The list is scanned at startup; use "Refresh list" after downloading a new pet in Codex. No reflash is involved, which is the whole point of the package mechanism (see `docs/development/engineering/codex-pet.md`).
- **Bridge / Device / Battery / Codex / Bubble / Session**: read-only status rows. When the device is not connected the row reminds you to check that it is on the same 2.4 GHz network. The battery row is hidden until the device reports a level — it comes from the CW2017 gauge over the same link, and it is dropped again on disconnect so a stale reading is never shown; 20% or below adds a warning prefix.
- **Push state**: manually push idle / working / waiting / ready / failed, so demos and debugging do not need a terminal. A manual state wins over log inference until the next Codex event arrives.
- **Send bubble text…** (⌘T): changes only the one line shown on the device, not the state.
- **Follow Codex session log**: when off, the bridge stops reading `~/.codex/sessions` and states come only from manual commands.
- **Restart bridge** (⌘R): relaunches the child process by hand (it is relaunched automatically if it dies anyway). After editing `pet_bridge.py` or the port, rebuild first — see the previous section; a running child never goes back to the repo on its own.
- **Open log** (⌘L) and **Copy diagnostics**: the former gives the full output, the latter puts the key status plus the log tail on the clipboard in one go. The bridge-script line it copies is an absolute path, and says "not found" when the bundle is incomplete.
- **About Codex Pet Bridge**: version, **interpreter path**, bridge-script path, bridge port and control channel. The interpreter is the one thing that decides whether pet swapping can work at all (packing an atlas into a `.pet` needs Pillow), so it is shown here read-only — the menu deliberately has **no** way to edit it. Why, and how to override it, is in "Interpreter and Pillow" below.
- **Quit**: also stops the bridge child process, so no orphan keeps holding the port.

## Headless provisioning (`--provision`)

The same BLE client is reachable without the GUI, which is what makes the path scriptable and testable:

```bash
"Codex Pet Bridge.app/Contents/MacOS/CodexPetBridge" --provision --scan
"Codex Pet Bridge.app/Contents/MacOS/CodexPetBridge" --provision --pin 1234 --password 'hunter2'
"Codex Pet Bridge.app/Contents/MacOS/CodexPetBridge" --provision --pin 1234 --forget
```

`--scan` lists advertising devices and exits. Otherwise `--ssid`, `--host`, and `--port` default to the current Wi-Fi, this Mac's LAN address, and the bridge port compiled into the app. Exit codes: `0` success, `1` failure, `2` bad arguments. `--json` emits one JSON object per line; `--out <path>` additionally appends them to a file.

Bluetooth permission is attributed to the **responsible process**, not to the binary being run. Launched from a terminal or an automation host, that is the terminal itself — which has no `NSBluetoothAlwaysUsageDescription`, so the process dies with `SIGABRT` under the `TCC` termination namespace. Launching through LaunchServices (`open -n … --args`) makes the app its own responsible process, and `--out` exists so the result can still be collected from a file. Either way the first run prompts for permission, and someone has to accept it.

## Ports and paths

| Item | Value |
| --- | --- |
| Bridge port | extracted from `PET_BRIDGE_PORT` in `main/pet_config.h` at build time and compiled in; changing that macro requires a rebuild |
| Bridge script | **exactly one source**: `Contents/Resources/bridge/pet_bridge.py` inside the app bundle (copied from `tools/` at build time) |
| Support directory | `~/Library/Application Support/CodexPetBridge/` (socket, log, pet cache) |
| Control channel | `~/Library/Application Support/CodexPetBridge/bridge.sock` |
| Log | `~/Library/Application Support/CodexPetBridge/bridge.log` |
| python | probed in order: `/opt/homebrew/bin/python3`, `/usr/local/bin/python3`, `/usr/bin/python3`, preferring whichever can `import PIL`; **not editable from the UI** — see "Interpreter and Pillow" |

`pet_bridge.py` only uses the standard library, so the system python3 is enough and no virtual environment is needed.

Uninstalling means deleting the app bundle; the support directory above and the `local.codex-pet.bridge` preferences are left behind and have to be removed separately.

## Interpreter and Pillow

Which `python3` the bridge runs on has **no editing entry point in the menu** — it is only shown read-only in "About". The reason is that it is far too easy to break: one wrong character in the path, or simply deleting the virtual environment later, shows up only as "pet swapping fails", and once it is broken there is no way back to the previous one from the UI.

Without an override the interpreter is probed in the order shown in the table, **preferring whichever can `import PIL`**. `pet_bridge.py` itself only needs the standard library, so an interpreter without Pillow still runs the bridge — only pet swapping needs Pillow (to pack an atlas into the `.pet` the device accepts). That kind of "half working" failure is the hardest to trace, which is why the pets submenu states the reason at the top and disables the pets that have not been packed yet, before you click anything.

If the interpreter that has Pillow is not in those three locations — for example because it lives in a virtual environment, which is the case on this machine — point at it from the command line:

```bash
defaults write local.codex-pet.bridge pythonPath /path/to/python3   # set
defaults delete local.codex-pet.bridge pythonPath                   # back to auto-detection
```

**An unusable path can never stop the bridge from starting**: that override is ignored and auto-detection takes over, and both "About" and "Copy diagnostics" say so explicitly.

## Firewall

`pet_bridge.py` listens on `0.0.0.0` at the bridge port (8765 by default), so the first run may trigger the macOS prompt asking whether to allow incoming network connections. It must be allowed, otherwise the device can never connect (it will simply stay asleep offline). With the firewall currently off there is no prompt.

## A note on layering

The app writes no network traffic of its own — it hands commands to `pet_bridge.py`. So any new protocol behaviour still only has to be implemented once in `tools/pet_bridge.py`, and `tests/test_pet_bridge.py::test_control_channel_drives_the_device` covers the path from a command down to the device.

At runtime there is exactly **one** source for the script (the app bundle), so "which copy is running" is not a question: nothing is read from the support directory and the repo takes no part. The only connection between the repo and runtime is `build.sh`, which copies the scripts into the bundle and compiles the port in — both at build time.
