# MOD backend selection (libxmp only)
set(MOD_BACKEND_SRC "mod_backend_xmp.c")
set(MOD_BACKEND_REQUIRES libxmp)
set(MOD_BACKEND_DEFINE "MOD_BACKEND_XMP=1")
message(STATUS "MOD backend: libxmp")

set(VGM_SRCS "vgm_player.c" "vgm_backend.cpp")
set(VGM_REQUIRES libvgm_wrapper)
message(STATUS "VGM playback: enabled")

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
		libvgm_wrapper
		esp_driver_sdmmc
		esp_driver_ppa
		esp-dsp
		esp_rom
	INCLUDE_DIRS
		"."
)

# Pass the backend selection to the preprocessor
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${MOD_BACKEND_DEFINE})
