# libopenmpt Setup Guide

Quick reference for setting up libopenmpt in this project.

## Quick Start

```bash
# From project root
cd components/libopenmpt

# Add libopenmpt as submodule
git submodule add https://github.com/OpenMPT/openmpt.git libopenmpt

# Checkout stable release
cd libopenmpt
git checkout libopenmpt-0.7.0
cd ../..

# Return to project root
cd ../..

# Test build
make build
```

## Verify Setup

After setup, verify these files exist:

```bash
ls components/libopenmpt/libopenmpt/libopenmpt_c/libopenmpt_c.cpp
ls components/libopenmpt/libopenmpt/include/libopenmpt/libopenmpt.h
```

If these files exist, the setup is correct.

## Build Test

Try building the project:

```bash
make build 2>&1 | grep -i libopenmpt
```

You should see libopenmpt source files being compiled. If you see warnings about missing source, check the file paths.

## Common Issues

### Issue: "stub.cpp" being compiled
**Solution:** libopenmpt source not found. Verify the submodule is added correctly:
```bash
cd components/libopenmpt
git submodule status
```

### Issue: C++ compilation errors
**Solution:** Ensure ESP-IDF version supports C++17. Check `idf.py --version` and update if needed.

### Issue: Missing source files
**Solution:** The CMakeLists.txt uses glob to find sources. If files are missing, you may need to manually list them. Check libopenmpt's build system for required files.

## Next: Create Wrapper

Once libopenmpt builds successfully, proceed to create the wrapper layer:
- See `LIBOPENMPT_MIGRATION_PLAN.md`
- Create `main/mod_backend_openmpt.h` and `main/mod_backend_openmpt.c`
