<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Codex Pet Bridge (macOS menu bar app)

Pushes the Codex state from your Mac to the pet on the AI Passport, and shows an icon in the system menu bar.

It is **not** a second bridge implementation: the only implementation of the wire protocol lives in `tools/pet_bridge.py` (already cross-checked against the device-side parser), and this app does three things:

1. runs `pet_bridge.py` as a child process and supervises it (relaunching with backoff if it dies);
2. reads status and sends commands over the local `--control` channel;
3. renders that status in the menu bar icon and its drop-down menu.

## Build and run

```sh
./tools/menubar/build.sh          # output lands in build/menubar/
./tools/menubar/build.sh --run    # build, then launch
```

The only requirement is `swiftc` from the Xcode Command Line Tools (`xcode-select --install`). There is no Xcode project and no third-party dependency. The build bakes the current project directory into the app as its default, so an app built inside the repo works out of the box.

Once launched, the icon appears on the right side of the menu bar. **Having no Dock icon is expected** (`LSUIElement=1`).

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
- **Bridge / Device / Battery / Codex / Bubble / Session**: read-only status rows. When the device is not connected the row reminds you to check that it is on the same 2.4 GHz network. The battery row is hidden until the device reports a level — it comes from the CW2017 gauge over the same link, and it is dropped again on disconnect so a stale reading is never shown; 20% or below adds a warning prefix.
- **Push state**: manually push idle / working / waiting / ready / failed, so demos and debugging do not need a terminal. A manual state wins over log inference until the next Codex event arrives.
- **Send bubble text…** (⌘T): changes only the one line shown on the device, not the state.
- **Follow Codex session log**: when off, the bridge stops reading `~/.codex/sessions` and states come only from manual commands.
- **Restart bridge** (⌘R): relaunches the child process. Use it after changing the port in `main/pet_config.h`.
- **Open log** (⌘L) and **Copy diagnostics**: the former gives the full output, the latter puts the key status plus the log tail on the clipboard in one go.
- **Project directory**: point the app at another checkout if it was not built inside this repo.
- **Quit**: also stops the bridge child process, so no orphan keeps holding the port.

## Headless provisioning (`--provision`)

The same BLE client is reachable without the GUI, which is what makes the path scriptable and testable:

```bash
"Codex Pet Bridge.app/Contents/MacOS/CodexPetBridge" --provision --scan
"Codex Pet Bridge.app/Contents/MacOS/CodexPetBridge" --provision --pin 1234 --password 'hunter2'
"Codex Pet Bridge.app/Contents/MacOS/CodexPetBridge" --provision --pin 1234 --forget
```

`--scan` lists advertising devices and exits. Otherwise `--ssid`, `--host`, and `--port` default to the current Wi-Fi, this Mac's LAN address, and the configured bridge port. Exit codes: `0` success, `1` failure, `2` bad arguments. `--json` emits one JSON object per line; `--out <path>` additionally appends them to a file.

Bluetooth permission is attributed to the **responsible process**, not to the binary being run. Launched from a terminal or an automation host, that is the terminal itself — which has no `NSBluetoothAlwaysUsageDescription`, so the process dies with `SIGABRT` under the `TCC` termination namespace. Launching through LaunchServices (`open -n … --args`) makes the app its own responsible process, and `--out` exists so the result can still be collected from a file. Either way the first run prompts for permission, and someone has to accept it.

## Ports and paths

| Item | Value |
| --- | --- |
| Bridge port | read from `PET_BRIDGE_PORT` in `main/pet_config.h`, so changing the config changes the port |
| Control channel | `~/Library/Application Support/CodexPetBridge/bridge.sock` |
| Log | `~/Library/Application Support/CodexPetBridge/bridge.log` |
| python | probed in order: `/usr/bin/python3`, `/opt/homebrew/bin/python3`, `/usr/local/bin/python3` |

`pet_bridge.py` only uses the standard library, so the system python3 is enough and no virtual environment is needed.

## Firewall

`pet_bridge.py` listens on `0.0.0.0:8765`, so the first run may trigger the macOS prompt asking whether to allow incoming network connections. It must be allowed, otherwise the device can never connect (it will simply stay asleep offline). With the firewall currently off there is no prompt.

## A note on layering

The app writes no network traffic of its own — it hands commands to `pet_bridge.py`. So any new protocol behaviour still only has to be implemented once in `tools/pet_bridge.py`, and `tests/test_pet_bridge.py::test_control_channel_drives_the_device` covers the path from a command down to the device.
