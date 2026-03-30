# speedometer

Small LVGL-on-Linux display test app for a Raspberry Pi style setup using:

- LVGL
- DRM/KMS page flipping
- raw evdev touch input

## Requirements

- a local LVGL checkout
- `libdrm`
- a C/C++ toolchain

This repo does not vendor LVGL. The build compiles LVGL directly from a local checkout.

## LVGL checkout

By default, the `Makefile` expects LVGL at:

```sh
../src/lvgl
```

So if this repo is checked out at:

```sh
/home/user/speedometer
```

the default LVGL path would be:

```sh
/home/user/src/lvgl
```

## Build

If your LVGL checkout matches the default layout:

```sh
make
```

If LVGL is somewhere else, point the build at it explicitly:

```sh
make LVGL_DIR=/path/to/lvgl
```

You can also generate a compile database for editor tooling:

```sh
make compdb LVGL_DIR=/path/to/lvgl
```

## Notes

- The build checks that `$(LVGL_DIR)/lvgl.h` and `$(LVGL_DIR)/src` exist before compiling.
