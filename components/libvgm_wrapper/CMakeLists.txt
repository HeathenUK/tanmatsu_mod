# libvgm ESP-IDF component for VGM/VGZ playback
# Builds chip emulators for various retro gaming systems

# libvgm source root (submodule)
set(LIBVGM_DIR "${CMAKE_CURRENT_LIST_DIR}/../../external/libvgm")

if(NOT EXISTS "${LIBVGM_DIR}/emu")
    message(FATAL_ERROR "libvgm submodule not found at ${LIBVGM_DIR}. Run: git submodule update --init --recursive")
endif()

# Core emulator sources (required by all chips)
set(EMU_CORE_SRCS
    "${LIBVGM_DIR}/emu/SoundEmu.c"
    "${LIBVGM_DIR}/emu/Resampler.c"
    "${LIBVGM_DIR}/emu/panning.c"
    "${LIBVGM_DIR}/emu/dac_control.c"
    "${LIBVGM_DIR}/emu/logging.c"
)

# Genesis/MegaDrive: YM2612 (FM) + SN76496 (PSG)
# Note: fmopn.c is for GPGX/MAME core (EC_YM2612_GPGX), ym2612.c is for Gens core (EC_YM2612_GENS)
set(EMU_GENESIS_SRCS
    "${LIBVGM_DIR}/emu/cores/2612intf.c"
    "${LIBVGM_DIR}/emu/cores/fmopn.c"
    "${LIBVGM_DIR}/emu/cores/ym2612.c"
    "${LIBVGM_DIR}/emu/cores/sn764intf.c"
    "${LIBVGM_DIR}/emu/cores/sn76496.c"
    "${LIBVGM_DIR}/emu/cores/sn76489.c"
)

# YM2151 - Arcade FM (CPS1, many arcade boards)
set(EMU_YM2151_SRCS
    "${LIBVGM_DIR}/emu/cores/2151intf.c"
    "${LIBVGM_DIR}/emu/cores/ym2151.c"
)

# YM2413 (OPLL) - SMS FM Unit, MSX-MUSIC
set(EMU_YM2413_SRCS
    "${LIBVGM_DIR}/emu/cores/2413intf.c"
    "${LIBVGM_DIR}/emu/cores/emu2413.c"
)

# AY8910 - MSX, ZX Spectrum, Amstrad CPC, Atari ST
set(EMU_AY8910_SRCS
    "${LIBVGM_DIR}/emu/cores/ayintf.c"
    "${LIBVGM_DIR}/emu/cores/emu2149.c"
)

# NES APU - Nintendo Entertainment System / Famicom
set(EMU_NES_SRCS
    "${LIBVGM_DIR}/emu/cores/nesintf.c"
    "${LIBVGM_DIR}/emu/cores/np_nes_apu.c"
    "${LIBVGM_DIR}/emu/cores/np_nes_dmc.c"
    "${LIBVGM_DIR}/emu/cores/np_nes_fds.c"
)

# Game Boy APU
set(EMU_GAMEBOY_SRCS
    "${LIBVGM_DIR}/emu/cores/gb.c"
)

# HuC6280 - PC Engine / TurboGrafx-16
set(EMU_PCE_SRCS
    "${LIBVGM_DIR}/emu/cores/c6280intf.c"
)

# OPL2 (YM3812) - AdLib, Sound Blaster, many DOS games
set(EMU_OPL2_SRCS
    "${LIBVGM_DIR}/emu/cores/oplintf.c"
    "${LIBVGM_DIR}/emu/cores/fmopl.c"
)

# OPL3 (YMF262) - Sound Blaster Pro 2, AWE32, DOS games
set(EMU_OPL3_SRCS
    "${LIBVGM_DIR}/emu/cores/262intf.c"
    "${LIBVGM_DIR}/emu/cores/ymf262.c"
)

# QSound - Capcom CPS1.5/CPS2 arcade
set(EMU_QSOUND_SRCS
    "${LIBVGM_DIR}/emu/cores/qsoundintf.c"
    "${LIBVGM_DIR}/emu/cores/qsound_ctr.c"
)

# POKEY - Atari 8-bit, Atari 5200, some arcade
set(EMU_POKEY_SRCS
    "${LIBVGM_DIR}/emu/cores/pokey.c"
)

# K051649 (SCC) - Konami MSX cartridges
set(EMU_SCC_SRCS
    "${LIBVGM_DIR}/emu/cores/k051649.c"
)

# SegaPCM - Sega arcade (OutRun, After Burner, etc.)
set(EMU_SEGAPCM_SRCS
    "${LIBVGM_DIR}/emu/cores/segapcm.c"
)

# Sega CD PCM (RF5C68) - always enabled
set(EMU_SEGACD_SRCS
    "${LIBVGM_DIR}/emu/cores/scd_pcm.c"
    "${LIBVGM_DIR}/emu/cores/rf5cintf.c"
    "${LIBVGM_DIR}/emu/cores/rf5c68.c"
)

# Player module sources (C++ files)
set(PLAYER_SRCS
    "${LIBVGM_DIR}/player/vgmplayer.cpp"
    "${LIBVGM_DIR}/player/vgmplayer_cmdhandler.cpp"
    "${LIBVGM_DIR}/player/playerbase.cpp"
    "${LIBVGM_DIR}/player/playera.cpp"
    "${LIBVGM_DIR}/player/helper.c"
    "${LIBVGM_DIR}/player/dblk_compr.c"
)

# Utils module sources
# Note: Using custom MemoryLoader_simple.c instead of the original MemoryLoader.c
# to avoid zlib dependency (VGZ decompression is handled in vgm_backend.cpp)
# Using StrUtils_simple.c instead of StrUtils-CPConv_IConv.c to avoid iconv dependency
set(UTILS_SRCS
    "${LIBVGM_DIR}/utils/DataLoader.c"
    "MemoryLoader_simple.c"
    "StrUtils_simple.c"
)

idf_component_register(
    SRCS
        ${EMU_CORE_SRCS}
        ${EMU_GENESIS_SRCS}
        ${EMU_YM2151_SRCS}
        ${EMU_YM2413_SRCS}
        ${EMU_AY8910_SRCS}
        ${EMU_NES_SRCS}
        ${EMU_GAMEBOY_SRCS}
        ${EMU_PCE_SRCS}
        ${EMU_OPL2_SRCS}
        ${EMU_OPL3_SRCS}
        ${EMU_QSOUND_SRCS}
        ${EMU_POKEY_SRCS}
        ${EMU_SCC_SRCS}
        ${EMU_SEGAPCM_SRCS}
        ${EMU_SEGACD_SRCS}
        ${PLAYER_SRCS}
        ${UTILS_SRCS}
    INCLUDE_DIRS
        "${LIBVGM_DIR}"
        "${LIBVGM_DIR}/emu"
        "${LIBVGM_DIR}/player"
        "${LIBVGM_DIR}/utils"
    PRIV_INCLUDE_DIRS
        "${LIBVGM_DIR}"
        "${LIBVGM_DIR}/emu"
        "${LIBVGM_DIR}/player"
        "${LIBVGM_DIR}/utils"
    PRIV_REQUIRES
        driver
    LDFRAGMENTS
        "linker.lf"
)

# libvgm compile definitions - enable supported chips
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    # Endianness for ESP32-P4 (RISC-V, little-endian)
    VGM_LITTLE_ENDIAN
    # Enable device selection mode (instead of building all emulators)
    SNDDEV_SELECT

    # ========== Sound device enables ==========
    # Genesis/Mega Drive (most common VGMs)
    SNDDEV_YM2612      # Genesis FM
    SNDDEV_SN76496     # Genesis/SMS PSG

    # Master System / Game Gear
    SNDDEV_YM2413      # OPLL FM

    # MSX / ZX Spectrum / Amstrad
    SNDDEV_AY8910      # PSG

    # NES / GB / Arcade / OPL / Sega CD
    SNDDEV_NES_APU     # NES APU
    SNDDEV_GAMEBOY     # Game Boy
    SNDDEV_SEGAPCM     # Sega arcade PCM
    SNDDEV_YM3812      # OPL2
    SNDDEV_YMF262      # OPL3
    SNDDEV_RF5C68      # Sega CD PCM

    # ========== Emulator core enables ==========
    # When SNDDEV_SELECT is defined, we must explicitly enable cores.

    # Genesis FM - Gens core (faster than GPGX)
    EC_YM2612_GENS

    # Genesis/SMS PSG - Maxim core (faster than MAME)
    EC_SN76496_MAXIM

    # OPLL (SMS FM, MSX) - EMU2413 is lightweight
    EC_YM2413_EMU2413

    # AY8910 PSG - EMU2149 is lightweight
    EC_AY8910_EMU2149

    # OPL2/OPL3 - MAME cores (compatible with current build flags)
    EC_YM3812_MAME
    EC_YMF262_MAME

    # NES APU - NSFPlay core (matches current source set)
    EC_NES_NSFPLAY
)

# C++ standard for player module
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 11)

# Optimization flags for performance-critical emulation code
# -O3 for maximum optimization
# -ffast-math for faster floating point (chip emulators are not precision-critical)
# -funroll-loops to unroll tight loops in emulators
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -ffast-math
    -funroll-loops
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-implicit-fallthrough
    -Wno-maybe-uninitialized
    -Wno-incompatible-pointer-types  # int32_t vs INT32 on RISC-V
    -Wno-unused-function
    -Wno-unused-variable
)

message(STATUS "libvgm: Building with multi-system chip support (Genesis, SMS, NES, GB, PCE, MSX, Arcade, DOS)")
