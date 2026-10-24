# HachimoDock pet runtime: Mosaico glue (hd_*), the ported shared modules and
# hardware adapters (pet/pet_p4_*). Host-only shims under pet/host_only are
# excluded here; the simulator picks them via game.sim.json instead.
set(PET_SOURCES
    "pet/pet_p4_protocol.c"
    "pet/pet_p4_miniapp.c"
    "pet/pet_p4_game.c"
    "pet/pet_p4_input_core.c"
    "pet/pet_p4_touch_core.c"
    "pet/pet_p4_behavior.c"
    "pet/pet_p4_stats.c"
    "pet/pet_p4_view.c"
    "pet/pet_p4_widget_data.c"
    "pet/pet_p4_media.c"
    "pet/pet_p4_media_codec.c"
    "pet/pet_p4_instrument_core.c"
    "pet/pet_p4_system_cues.c"
    "pet/pet_p4_conversation.c"
    "pet/pet_p4_assets.c"
    "pet/pet_p4_builtin_components_data.c"
    "pet/pet_p4_font_cn.c"
    "pet/pet_p4_input.c"
    "pet/pet_p4_touch.c"
    "pet/pet_p4_audio.c"
    "pet/pet_p4_diagnostics.c"
    "pet/pet_p4_renderer.c"
    "pet/pet_p4_ota_stub.c"
    "pet/pet_p4_audio_frontend_stub.c")

idf_component_register(
    SRCS "main.c" "hd_pet.c" "hd_cdc.c" "hd_storage.c" ${PET_SOURCES}
    INCLUDE_DIRS "." "pet"
    REQUIRES mosaico_game_app mosaico_raylib_fast mosaico_game_2d esp-gsp
    PRIV_REQUIRES esp-mosaico-bsp esp_tinyusb esp_codec_dev nvs_flash
        esp_littlefs spi_nand_flash esp_blockdev esp_h264 lvgl cjson esp-mosaico-bsp)

# GSP needs an image to bind the Raylib framebuffer. This generated placeholder
# is embedded in the application; no external game resource partition is used.
set(canvas_png "${CMAKE_CURRENT_BINARY_DIR}/canvas.png")
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/make_canvas_png.py"
    "${canvas_png}" RESULT_VARIABLE canvas_result)
if(NOT canvas_result EQUAL 0)
    message(FATAL_ERROR "Could not generate the game canvas placeholder")
endif()
set(CANVAS_IMAGE "${canvas_png}")
configure_file("scene.json.in" "${CMAKE_CURRENT_BINARY_DIR}/scene.json" @ONLY)
# Upstream HachimoDock compact one-line style trips the misleading-indentation
# diagnostic on this GCC; it is stylistic, not semantic.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-misleading-indentation)

# Embedded pet assets: wooden-fish instrument layers and the shared status
# cue WAVs (OTA-updatable, independent of the appearance slots).
foreach(layer body mallet)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/assets/wooden_fish_${layer}.bin" BINARY)
endforeach()
foreach(cue done error waiting_user)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/assets/${cue}.wav" BINARY)
endforeach()

gsp_add_bundle(${COMPONENT_LIB}
    SCENES "${CMAKE_CURRENT_BINARY_DIR}/scene.json"
    PIXEL_FORMAT rgb565)
