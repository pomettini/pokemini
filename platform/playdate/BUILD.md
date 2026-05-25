# Building PokeMini for Playdate

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

Output: `PokeMini.pdx`. Sideload to device.

## Simulator build

```bash
cmake -S . -B build
cmake --build build
```

Output: `PokeMini.pdx` with `pdex.dylib`. Open in Playdate Simulator.

## Diagnostic flags

`-DPD_OPCODE_DIAG_BUILD=ON` or `-DPD_PERF_DIAG_BUILD=ON` enable logging.
Both default OFF. Turn off before shipping.

## Notes

- `PLAYDATE_SDK_PATH` must be exported; device build fails silently otherwise.
- `build-device/` and `build/` are separate; clean each independently.
- See [NOTES.md](NOTES.md) for performance-tuning history.
