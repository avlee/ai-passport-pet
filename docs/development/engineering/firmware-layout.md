<p align="right">
  <a href="firmware-layout.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Firmware Layout

This repository is a minimal base for user-defined firmware targeting an
ESP32-C3 with 8 MB Flash. Its default does not reserve product-specific
identity, OTA, or unused data partitions.

## Default layout

The default partition table contains exactly:

| Partition | Type/subtype | Offset | Size | Purpose |
| --- | --- | ---: | ---: | --- |
| `nvs` | data/NVS | `0x9000` | `0x6000` | ESP-IDF and application key-value storage |
| `phy_init` | data/PHY | `0xF000` | `0x1000` | PHY initialization data |
| `factory` | app/factory | `0x10000` | `0x7F0000` | The single application image; all remaining Flash |

The default has no OTA slots. This is a starting point, not a restriction on
user firmware.

> Note: this branch (`feature/codex-pet`) has already changed its
> `partitions.csv` as described under "Custom layouts" below — the app region
> shrinks to 4 MB and a `pets` data partition appears. The three rows above are
> the template defaults, not what the branch currently ships.

## Custom layouts

Users may edit `partitions.csv` to resize, move, add, or remove partitions for
their application. A custom table may use OTA slots, filesystem/resource
partitions, or other application-specific data. Keep the 8 MB device boundary,
avoid overlaps, and make sure the application image is flashed at the start of
an app partition large enough to contain it. When a derivative changes its
layout, update that project's documentation and flashing instructions.

### This branch's layout (Codex pet)

Pet assets are no longer compiled into the application image. They travel as a
single `.pet` data package in a dedicated `pets` partition, pushed over Wi-Fi by
the Mac-side Pet Bridge — which is why swapping a pet needs neither a rebuild nor
a reflash. Fitting a 4 MB-scale application, CJK fonts, and a 2.43 MiB pet into
8 MB means giving up the OTA slot pair:

| Partition | Type/subtype | Offset | Size | Purpose |
| --- | --- | ---: | ---: | --- |
| `pets` | data/`0x40` | `0x410000` | `0x3F0000` | The single pet package (`.pet`), swappable at runtime |

- The app region shrinks from `0x7F0000` to **`0x400000`**, with `pets` right
  after it. `tools/verify_firmware.py` validates the table by the same rules as
  always (boundaries, overlaps, unique labels, app capacity).
- ESP-IDF reserves `data` subtypes `0x00`–`0x3F`, so application-defined data
  starts at `0x40`. The firmware looks the partition up as `(data, 0x40)` — see
  `main/pet_slot.c`.
- The slot is **single**: swapping overwrites rather than storing side by side.
  Carrying more than one pet on this device would need per-frame compression
  (0.86–1.04 MB each, at the cost of a 75–90 KB RAM decode buffer) — see
  [codex-pet](codex-pet.md#the-pet-package-pet-and-single-slot-swapping).
- No OTA slot: 8 MB cannot hold "two slots + CJK fonts + a pet". Firmware
  upgrades still go through the merged image written once from `0x0`.

## Enforced validation

Run:

```bash
./tools/validate.sh --firmware
```

The check builds in an isolated directory, creates the merged image, reads the
configured image offsets from `flash_args`, validates the partition-table MD5,
partition bounds, unique labels, and non-overlap, then ensures the application
offset matches an app partition large enough to contain it. It intentionally
does not require the default partition list. CI runs the same gate.

Every image listed in `flash_args`, including user-defined resources and OTA
data, must exist, be nonempty and match the merged bytes at its configured
offset. Image ranges must stay within 8 MB and must not overlap. Additional
images must fit entirely inside a configured partition; an offset inside that
partition is allowed. Merely declaring a resource partition does not require a
preloaded image, but listing an image in `flash_args` makes it mandatory.

Upload only `build/FoloToy-AI-Passport-full.bin`; the similarly named app-only
`build/FoloToy-AI-Passport.bin` does not contain the bootloader or partition
table.

## Flashing and stored data

> **No backup of the firmware already installed on the device is required
> before downloading (flashing) new firmware.** Do not make reading out the
> original firmware or saving a full-Flash dump a prerequisite for this
> workflow. The new firmware replaces the original firmware; this workflow
> does not retain an automatic rollback copy or promise that the original
> firmware can be restored.

Firmware and user data are different. If existing NVS settings, application
records, or files must be kept, export or otherwise save them before flashing
using a method supported by that application. Not requiring an original-firmware
backup does not guarantee data preservation or authorize a full-chip erase.

The verified merged image is written from `0x0`. Because the merged file pads
the gaps between images, flashing it can reset the NVS and PHY data regions.
Use the merged image for blank-device provisioning or an intentional complete
refresh. During normal development, use segmented `idf.py flash` when existing
NVS state should be preserved; this also requires a compatible partition layout
and flash targets that do not overwrite those data regions. `idf.py erase-flash`
erases all user data. Do not add it as a routine prerequisite: use it only when
a complete erase is explicitly intended and any data that must be kept has
been saved.
