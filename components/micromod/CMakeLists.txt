# IBXM (micromod) component for ESP-IDF
# MOD/XM/S3M replay library

idf_component_register(
    SRCS
        "ibxm.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        # No external dependencies - pure C library
)

# Compiler optimizations for audio processing
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -ffast-math
)
