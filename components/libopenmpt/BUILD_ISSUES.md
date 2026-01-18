# libopenmpt Build Issues

## Current Status

✅ **Source Added**: libopenmpt source is present and detected  
✅ **Include Paths**: Fixed - BuildSettings.hpp now found  
❌ **Compilation**: Failing due to C++ template/constexpr issues

## Compilation Errors

The build is failing with C++ template instantiation errors related to:
- `constexpr` evaluation issues with random number generation
- Incomplete type issues with `DithersWrapperOpenMPT`
- Template instantiation problems

## Root Cause

ESP-IDF uses strict C++ flags:
- `-fno-exceptions` - No exception support
- `-fno-rtti` - No runtime type information
- `-std=gnu++2b` (C++23) but with restrictions

libopenmpt is a complex C++ library that may require:
- Exception support (for error handling)
- RTTI (for some template features)
- Full C++17/20 features

## Potential Solutions

### Option 1: Enable Exceptions/RTTI (if ESP-IDF allows)
May need to override C++ flags for libopenmpt component only.

### Option 2: Use Pre-built Library
Build libopenmpt separately with a full C++ toolchain, then link the static library.

### Option 3: Alternative Library
Consider if DUMB (already in components) might be a better fit, or if we can fix libxmp issues instead.

### Option 4: Patch libopenmpt
Modify libopenmpt source to work without exceptions/RTTI (significant effort).

## Next Steps

1. Check if ESP-IDF allows enabling exceptions for a single component
2. Consider building libopenmpt as external static library
3. Evaluate if the complexity is worth it vs fixing libxmp or using DUMB
