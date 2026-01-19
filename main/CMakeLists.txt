# MOD backend selection via Kconfig (menuconfig)
# Only the selected backend source file and library will be compiled/linked

# Determine which backend is selected from Kconfig
if(CONFIG_MOD_BACKEND_XMP)
    set(MOD_BACKEND_SRC "mod_backend_xmp.c")
    set(MOD_BACKEND_REQUIRES libxmp)
    set(MOD_BACKEND_DEFINE "MOD_BACKEND_XMP=1")
    message(STATUS "MOD backend: libxmp (via Kconfig)")
elseif(CONFIG_MOD_BACKEND_OPENMPT)
    set(MOD_BACKEND_SRC "mod_backend_openmpt.c")
    set(MOD_BACKEND_REQUIRES libopenmpt)
    set(MOD_BACKEND_DEFINE "MOD_BACKEND_OPENMPT=1")
    message(STATUS "MOD backend: libopenmpt (via Kconfig)")
elseif(CONFIG_MOD_BACKEND_MICROMOD)
    set(MOD_BACKEND_SRC "mod_backend_micromod.c")
    set(MOD_BACKEND_REQUIRES micromod)
    set(MOD_BACKEND_DEFINE "MOD_BACKEND_MICROMOD=1")
    message(STATUS "MOD backend: micromod/IBXM (via Kconfig)")
else()
    # Default fallback (shouldn't happen if Kconfig is properly set up)
    set(MOD_BACKEND_SRC "mod_backend_xmp.c")
    set(MOD_BACKEND_REQUIRES libxmp)
    set(MOD_BACKEND_DEFINE "MOD_BACKEND_XMP=1")
    message(WARNING "No MOD backend selected in Kconfig, defaulting to libxmp")
endif()

# VGM playback support (conditionally compiled)
if(CONFIG_VGM_ENABLE)
    set(VGM_SRCS "vgm_player.c" "vgm_backend.cpp")
    set(VGM_REQUIRES libvgm)
    message(STATUS "VGM playback: enabled")
else()
    set(VGM_SRCS)
    set(VGM_REQUIRES)
    message(STATUS "VGM playback: disabled")
endif()

idf_component_register(
	SRCS
		"main.c"
		"audio.c"
		"mod_player.c"
		${MOD_BACKEND_SRC}
		${VGM_SRCS}
		"sdcard.c"
		"file_browser.c"
		"simple_font.c"
		"profiling.c"
		"ui_primitives.c"
		"spectrum_analyzer.c"
		"stb_image_impl.c"
		"fkey_icons.c"
		"app_state.c"
		"graphics/hw_accel.c"
		"ui/ui_file_browser.c"
		"ui/ui_input_handler.c"
	PRIV_REQUIRES
		esp_lcd
		fatfs
		nvs_flash
		badge-bsp
		tanmatsu-wifi
		libxmp
		libopenmpt
		micromod
		libvgm
		esp_driver_sdmmc
		esp_driver_ppa
		esp-dsp
		esp_rom
	INCLUDE_DIRS
		"."
)

# Pass the backend selection to the preprocessor
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${MOD_BACKEND_DEFINE})
