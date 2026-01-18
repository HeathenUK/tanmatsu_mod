# IBXM (micromod) component for ESP-IDF
# MOD/XM/S3M replay library
# Using submodule at micromod/ibxm-ac/

idf_component_register(
    SRCS
        "micromod/ibxm-ac/ibxm.c"
    INCLUDE_DIRS
        "micromod/ibxm-ac"
    REQUIRES
        # No external dependencies - pure C library
)

# Compiler optimizations for audio processing
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -ffast-math
)
