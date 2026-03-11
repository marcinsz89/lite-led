# lite-led

`lite-led` is a Nintendo Switch sysmodule and Tesla overlay for controlling the notification LED.

It packages as:

- an Atmosphere sysmodule at title ID `0100000000000895`
- a Tesla overlay at `switch/.overlays/lite-led.ovl`

## Features

- Boot-time LED handling
- Controller notification LED updates
- Tesla overlay for changing modes on-device
- One-step packaging script for Atmosphere + Tesla layout

## Modes

| Mode | Behavior |
| --- | --- |
| `Solid` | Full brightness |
| `Dim` | Lower brightness |
| `Fade` | Breathing / fade effect |
| `Off` | LED disabled |
| `Charge` | Dim while charging |
| `Battery` | Dim below 15%, blink below 5% |

## Requirements

- `bash`
- `make`
- [devkitPro](https://devkitpro.org/) with `libnx`
- `DEVKITPRO` exported in your shell

Example:

```bash
export DEVKITPRO=/opt/devkitpro
```

## Quick Start

Build both the sysmodule and the overlay, then package them into the correct folder structure:

```bash
./build-and-package.sh
```

The script will:

1. Build `products/sysmodule/lite-led.nsp`
2. Build `products/overlay/lite-led.ovl`
3. Ask for an output parent directory
4. Create a `lite-led/` package folder inside that parent directory
5. Ask before clearing an existing `lite-led/` package folder

You can also pass the output parent directory as an argument:

```bash
./build-and-package.sh /path/to/output-parent
```

That will create:

```text
/path/to/output-parent/lite-led
```

## Packaged Output

The generated package directory looks like this:

```text
lite-led/
├── atmosphere/
│   └── contents/
│       └── 0100000000000895/
│           ├── exefs.nsp
│           ├── flags/
│           └── toolbox.json
└── switch/
    └── .overlays/
        └── lite-led.ovl
```

## Manual Build

If you want to build the targets separately:

```bash
make -C sysmodule
make -C overlay
```

Artifacts:

- `products/sysmodule/lite-led.nsp`
- `products/overlay/lite-led.ovl`

Clean build outputs:

```bash
make -C sysmodule clean
make -C overlay clean
```

## Installation

Copy the contents of the packaged `lite-led/` folder to the root of your SD card.

That will place:

- `atmosphere/contents/0100000000000895/exefs.nsp`
- `atmosphere/contents/0100000000000895/toolbox.json`
- `switch/.overlays/lite-led.ovl`

Reboot the console after installing the sysmodule.

## Project Layout

| Path | Purpose |
| --- | --- |
| `sysmodule/` | Atmosphere sysmodule source and config |
| `overlay/` | Tesla overlay source |
| `build-and-package.sh` | Builds both targets and assembles the release folder |
