# libopenmpt Component for ESP-IDF
# Note: libopenmpt is a C++ library requiring C++17

# Only build this component if MOD_BACKEND_OPENMPT is selected via Kconfig
if(NOT CONFIG_MOD_BACKEND_OPENMPT)
    # Component not selected - create minimal stub to prevent build errors
    idf_component_register(
        SRCS
            "stub.cpp"
        INCLUDE_DIRS
            "."
    )
    
    # Create stub file if it doesn't exist
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stub.cpp")
        file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/stub.cpp"
            "// libopenmpt component disabled - MOD_BACKEND_OPENMPT not selected\n"
        )
    endif()
    
    message(STATUS "libopenmpt: Component disabled (MOD_BACKEND_OPENMPT not selected)")
    return()
endif()

# Minimum C++ standard required
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Check if libopenmpt source exists
# libopenmpt source structure (when added as submodule or extracted):
# - libopenmpt/libopenmpt/ (C++ implementation)
# - libopenmpt/libopenmpt_c/ (C API wrapper)
# - include/libopenmpt/ (headers)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/libopenmpt/libopenmpt_c.cpp")
    # libopenmpt source is present - configure full build
    
    # Collect source files from required directories
    # libopenmpt needs: libopenmpt/, soundlib/, common/, sounddsp/, unarchiver/, tracklib/
    file(GLOB_RECURSE LIBOPENMPT_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/libopenmpt/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/libopenmpt/libopenmpt/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/soundlib/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/common/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/sounddsp/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/unarchiver/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/tracklib/*.cpp"
    )
    
    # Filter out test files, examples, plugin code, and optional dependencies
    list(FILTER LIBOPENMPT_SOURCES EXCLUDE REGEX ".*test.*")
    list(FILTER LIBOPENMPT_SOURCES EXCLUDE REGEX ".*example.*")
    # Filter out plugin files but keep PlugInterface.cpp which is needed even without plugins
    list(FILTER LIBOPENMPT_SOURCES EXCLUDE REGEX ".*plugins/Plugin.*\\.cpp$")
    list(FILTER LIBOPENMPT_SOURCES EXCLUDE REGEX ".*plugins/PluginManager.*")
    list(FILTER LIBOPENMPT_SOURCES EXCLUDE REGEX ".*plugin.*gui.*")
    list(FILTER LIBOPENMPT_SOURCES EXCLUDE REGEX ".*plugin-common.*")
    # Keep PlugInterface.cpp - it's needed for SNDMIXPLUGIN::Destroy() even when NO_PLUGINS is defined
    list(FILTER LIBOPENMPT_SOURCES EXCLUDE REGEX ".*in_openmpt.*")
    list(FILTER LIBOPENMPT_SOURCES EXCLUDE REGEX ".*xmp-openmpt.*")
    list(FILTER LIBOPENMPT_SOURCES EXCLUDE REGEX ".*TimeStretchPitchShift.*")
    
    idf_component_register(
        SRCS
            ${LIBOPENMPT_SOURCES}
        INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt"
            "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/libopenmpt"
            "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/common"
            "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/soundlib"
            "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/sounddsp"
            "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/unarchiver"
            "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/tracklib"
            "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/libopenmpt/src"
        PRIV_REQUIRES
            # No external dependencies for minimal build
    )
    
    # Compile definitions for minimal embedded build
    # Disable optional features to reduce size
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        # Disable optional dependencies
        OPENMPT_BUILD_NO_ZLIB=1
        OPENMPT_BUILD_NO_VORBIS=1
        OPENMPT_BUILD_NO_OGG=1
        OPENMPT_BUILD_NO_MPG123=1
        OPENMPT_BUILD_NO_FLAC=1
        # Disable GUI/tracker features (we only need playback)
        OPENMPT_BUILD_NO_GUI=1
        # Disable plugins (but keep PlugInterface.cpp for structure definitions)
        NO_PLUGINS=1
        # Use C API (simpler for embedded)
        OPENMPT_USE_C_API=1
    )
    
    # Override ESP-IDF's -fno-exceptions and -fno-rtti for libopenmpt
    # libopenmpt requires exceptions and RTTI to compile
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -fexceptions
        -frtti
        -Os
        -ffast-math
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-sign-compare
        -Wno-return-type
        -Wno-error=return-type
        -Wno-switch
        -Wno-error=switch
    )
    
    message(STATUS "libopenmpt: Full build configured")
else()
    # libopenmpt source not present - create stub component
    message(WARNING "libopenmpt source not found. Please add libopenmpt source to components/libopenmpt/")
    message(WARNING "See components/libopenmpt/README.md for setup instructions")
    
    # Create a stub to prevent build errors
    idf_component_register(
        SRCS
            "stub.cpp"
        INCLUDE_DIRS
            "."
    )
    
    # Create stub file if it doesn't exist
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stub.cpp")
        file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/stub.cpp"
            "// libopenmpt stub - add libopenmpt source to enable\n"
            "// See README.md for setup instructions\n"
        )
    endif()
endif()

# Note: libopenmpt uses C++, so we need to ensure C++ runtime is linked
# ESP-IDF should handle this automatically, but verify if issues arise
