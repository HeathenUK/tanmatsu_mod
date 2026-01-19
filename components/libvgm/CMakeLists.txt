# libvgm ESP-IDF component for VGM/VGZ playback
# Builds chip emulators for various retro gaming systems

# Only build this component if VGM playback is enabled via Kconfig
if(NOT CONFIG_VGM_ENABLE)
    # Component not selected - create minimal stub
    idf_component_register(
        SRCS
            "stub.c"
        INCLUDE_DIRS
            "include"
    )

    # Create stub file if it doesn't exist
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stub.c")
        file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/stub.c"
            "// libvgm component disabled - CONFIG_VGM_ENABLE not selected\n"
        )
    endif()

    message(STATUS "libvgm: Component disabled (CONFIG_VGM_ENABLE not selected)")
    return()
endif()

# Core emulator sources (required by all chips)
set(EMU_CORE_SRCS
    "src/emu/SoundEmu.c"
    "src/emu/Resampler.c"
    "src/emu/panning.c"
    "src/emu/dac_control.c"
    "src/emu/logging.c"
)

# Genesis/MegaDrive: YM2612 (FM) + SN76496 (PSG)
set(EMU_GENESIS_SRCS
    "src/emu/cores/2612intf.c"
    "src/emu/cores/ym2612.c"
    "src/emu/cores/sn764intf.c"
    "src/emu/cores/sn76496.c"
)

# YM2151 - Arcade FM (CPS1, many arcade boards)
set(EMU_YM2151_SRCS
    "src/emu/cores/2151intf.c"
    "src/emu/cores/ym2151.c"
)

# YM2413 (OPLL) - SMS FM Unit, MSX-MUSIC
set(EMU_YM2413_SRCS
    "src/emu/cores/2413intf.c"
    "src/emu/cores/emu2413.c"
)

# AY8910 - MSX, ZX Spectrum, Amstrad CPC, Atari ST
set(EMU_AY8910_SRCS
    "src/emu/cores/ayintf.c"
    "src/emu/cores/emu2149.c"
)

# NES APU - Nintendo Entertainment System / Famicom
set(EMU_NES_SRCS
    "src/emu/cores/nesintf.c"
    "src/emu/cores/np_nes_apu.c"
    "src/emu/cores/np_nes_dmc.c"
    "src/emu/cores/np_nes_fds.c"
)

# Game Boy APU
set(EMU_GAMEBOY_SRCS
    "src/emu/cores/gb.c"
)

# HuC6280 - PC Engine / TurboGrafx-16
set(EMU_PCE_SRCS
    "src/emu/cores/c6280intf.c"
    "src/emu/cores/Ootake_PSG.c"
)

# OPL2 (YM3812) - AdLib, Sound Blaster, many DOS games
set(EMU_OPL2_SRCS
    "src/emu/cores/oplintf.c"
    "src/emu/cores/fmopl.c"
)

# OPL3 (YMF262) - Sound Blaster Pro 2, AWE32, DOS games
set(EMU_OPL3_SRCS
    "src/emu/cores/262intf.c"
    "src/emu/cores/ymf262.c"
)

# QSound - Capcom CPS1.5/CPS2 arcade
set(EMU_QSOUND_SRCS
    "src/emu/cores/qsoundintf.c"
    "src/emu/cores/qsound_ctr.c"
)

# POKEY - Atari 8-bit, Atari 5200, some arcade
set(EMU_POKEY_SRCS
    "src/emu/cores/pokey.c"
)

# K051649 (SCC) - Konami MSX cartridges
set(EMU_SCC_SRCS
    "src/emu/cores/k051649.c"
)

# SegaPCM - Sega arcade (OutRun, After Burner, etc.)
set(EMU_SEGAPCM_SRCS
    "src/emu/cores/segapcm.c"
)

# Sega CD PCM (RF5C68) - optional
if(CONFIG_VGM_ENABLE_SEGACD)
    set(EMU_SEGACD_SRCS
        "src/emu/cores/scd_pcm.c"
        "src/emu/cores/rf5cintf.c"
        "src/emu/cores/rf5c68.c"
    )
else()
    set(EMU_SEGACD_SRCS)
endif()

# Player module sources (C++ files)
set(PLAYER_SRCS
    "src/player/vgmplayer.cpp"
    "src/player/vgmplayer_cmdhandler.cpp"
    "src/player/playerbase.cpp"
    "src/player/playera.cpp"
    "src/player/helper.c"
    "src/player/dblk_compr.c"
)

# Utils module sources
# Note: Using custom MemoryLoader_simple.c instead of the original MemoryLoader.c
# to avoid zlib dependency (VGZ decompression is handled in vgm_backend.cpp)
# Using StrUtils_simple.c instead of StrUtils-CPConv_IConv.c to avoid iconv dependency
set(UTILS_SRCS
    "src/utils/DataLoader.c"
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
        "src"
        "src/emu"
        "src/player"
        "src/utils"
    PRIV_INCLUDE_DIRS
        "src"
        "src/emu"
        "src/player"
        "src/utils"
    PRIV_REQUIRES
        driver
)

# libvgm compile definitions - enable all supported chips
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    # Endianness for ESP32-P4 (RISC-V, little-endian)
    VGM_LITTLE_ENDIAN
    # Enable device selection mode (instead of building all emulators)
    SNDDEV_SELECT
    # Sega chips
    SNDDEV_YM2612      # Genesis FM
    SNDDEV_SN76496     # Genesis/SMS PSG
    # Arcade chips
    SNDDEV_YM2151      # Arcade FM (CPS1, etc.)
    SNDDEV_QSOUND      # Capcom CPS1.5/CPS2
    # Yamaha FM chips
    SNDDEV_YM2413      # OPLL (SMS FM, MSX)
    SNDDEV_YM3812      # OPL2 (AdLib)
    SNDDEV_YM3526      # OPL (predecessor to OPL2)
    SNDDEV_YMF262      # OPL3 (Sound Blaster Pro 2)
    # PSG chips
    SNDDEV_AY8910      # MSX, ZX Spectrum, Amstrad
    # Console chips
    SNDDEV_NES_APU     # NES/Famicom
    SNDDEV_GAMEBOY     # Game Boy
    SNDDEV_C6280       # PC Engine/TurboGrafx-16
    # Other chips
    SNDDEV_POKEY       # Atari
    SNDDEV_K051649     # SCC (Konami MSX)
    SNDDEV_SEGAPCM     # Sega arcade PCM
)

# Optional Sega CD support
if(CONFIG_VGM_ENABLE_SEGACD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SNDDEV_RF5C68)
endif()

# C++ standard for player module
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 11)

# Disable some warnings that appear in libvgm code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-implicit-fallthrough
    -Wno-maybe-uninitialized
    -Wno-incompatible-pointer-types  # int32_t vs INT32 on RISC-V
    -Wno-unused-function
    -Wno-unused-variable
)

message(STATUS "libvgm: Building with multi-system chip support (Genesis, SMS, NES, GB, PCE, MSX, Arcade, DOS)")
