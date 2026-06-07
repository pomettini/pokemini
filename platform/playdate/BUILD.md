# Building PlayMini for Playdate

## Prerequisites

- Playdate SDK
- ARM GCC toolchain (`arm-none-eabi-gcc`)
- CMake 3.16+
- `PLAYDATE_SDK_PATH` exported in your shell

## Device build (ships)

From `platform/playdate/`:

```bash
cmake -S . -B build-device \
    -DTOOLCHAIN=armgcc \
    -DCMAKE_TOOLCHAIN_FILE=$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake
cmake --build build-device --clean-first
```

Output: `PlayMini.pdx`. Sideload to device.

## Simulator build

```bash
cmake -S . -B build
cmake --build build
```

Output: `PlayMini.pdx` with `pdex.dylib`. Open in Playdate Simulator.

## Diagnostic flags

`-DPD_OPCODE_DIAG_BUILD=ON` or `-DPD_PERF_DIAG_BUILD=ON` enable logging.
Both default OFF. Turn off before shipping.

## PDLL (dev only)

`-DPOKEMINI_PDLL=ON` builds in the [PDLL](https://github.com/CrankBoyHQ/pdll)
dynamic-linking event hooks, used to hot-reload the simulator dylib during
development. Default OFF — production builds ship without it so the
`eventHandler` export matches a plain SDK build. Leave it off for release.

## Notes

- `PLAYDATE_SDK_PATH` must be exported; device build fails silently otherwise.
- `build-device/` and `build/` are separate; clean each independently.
- See [NOTES.md](NOTES.md) for performance-tuning history.
