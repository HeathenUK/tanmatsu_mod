# libopenmpt Component for ESP-IDF

This component integrates libopenmpt (OpenMPT library) for MOD file playback.

## Status

🚧 **IN PROGRESS** - Component integration phase

## Setup Instructions

libopenmpt is a C++ library. This component needs to be added as a git submodule or downloaded source.

### Option 1: Git Submodule (Recommended)

```bash
cd components/libopenmpt
git submodule add https://github.com/OpenMPT/openmpt.git libopenmpt
cd libopenmpt
git checkout libopenmpt-0.7.0  # Use latest stable release
cd ../..
```

**Note:** The libopenmpt source will be in `components/libopenmpt/libopenmpt/` after adding the submodule.

### Option 2: Download Release Tarball

1. Download a release tarball from https://lib.openmpt.org/libopenmpt/download/
2. Extract to `components/libopenmpt/libopenmpt/`
3. Ensure the structure is: `components/libopenmpt/libopenmpt/libopenmpt/` (source files)

### Option 3: Manual Clone

```bash
cd components/libopenmpt
git clone https://github.com/OpenMPT/openmpt.git libopenmpt
cd libopenmpt
git checkout libopenmpt-0.7.0
cd ../..
```

## Source Structure

After adding libopenmpt source, you should have:
```
components/libopenmpt/
├── CMakeLists.txt          (this component's build file)
├── idf_component.yml
├── README.md               (this file)
└── libopenmpt/             (libopenmpt source - added via submodule/clone)
    ├── libopenmpt/         (C++ implementation)
    ├── libopenmpt_c/       (C API wrapper)
    └── include/            (headers)
```

## Build Configuration

The component is configured for minimal embedded build:
- **C++17 standard** - Required by libopenmpt
- **Static linking** - No dynamic libraries
- **Minimal format support** - MOD/XM/S3M/IT enabled
- **Disabled optional dependencies:**
  - No zlib (compression)
  - No vorbis/ogg (sample formats)
  - No mpg123 (MP3 samples)
  - No FLAC support
  - No GUI/tracker features (playback only)

## Dependencies

- **C++ runtime** - Provided by ESP-IDF
- **Standard library** - Standard C++ library
- **No external libraries** - Minimal build avoids optional deps

## Verification

After adding the source, verify the build:

```bash
cd /path/to/tanmatsu_mod
make build
```

You should see libopenmpt compiling. If you see warnings about missing source, check that the libopenmpt source is in the correct location.

## Troubleshooting

### "libopenmpt source not found" warning
- Ensure libopenmpt source is in `components/libopenmpt/libopenmpt/`
- Check that `libopenmpt/libopenmpt_c.cpp` exists

### C++ compilation errors
- Verify ESP-IDF supports C++17 for your target
- Check that `CMAKE_CXX_STANDARD` is set correctly

### Link errors
- Ensure C++ runtime is linked (ESP-IDF should handle this)
- Check for missing symbols (may need to add more source files)

## Next Steps

Once the component builds successfully:
1. See `LIBOPENMPT_MIGRATION_PLAN.md` for migration details
2. Create wrapper layer (`mod_backend_openmpt.c/h`)
3. Replace libxmp usage in `mod_player.c`

## References

- libopenmpt Documentation: https://lib.openmpt.org/doc/
- libopenmpt GitHub: https://github.com/OpenMPT/openmpt
- libopenmpt C API: https://lib.openmpt.org/doc/group__libopenmpt__c.html
