# Codex 宠物应用。
#
# SRCS 里同时保留 BSP 参考示例的演示页(demo_*.c / ui_pixel.c): 它们不会被
# app_main 引用, 链接时由 --gc-sections 丢掉, 但代码留在仓库里, 通过宠物界面的
# 信息面板(长按上键)进入, 见 demo_menu.c。
idf_component_register(
    SRCS "main.c"
         "pet_app.c"
         "pet_ui.c"
         "pet_state.c"
         "pet_protocol.c"
         "pet_settings.c"
         "pet_atlas.c"
         "pet_atlas_sophie_portrait.c"
         "pet_atlas_validate.c"
         "pet_fonts.c"
         "pet_strings.c"
         "pet_bridge.c"
         "pet_provision.c"
         "pet_provision_parse.c"
         "demo_menu.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
    # 逐帧提取出来的宠物图集(RGB565A8)。由 tools/gen_pet_assets.py 生成,
    # 链接成 _binary_pet_sophie_portrait_bin_start/_end, 见 pet_atlas.c。
    EMBED_FILES "assets/pet_sophie_portrait.bin"
)

# 生成的字库源文件体积很大(~10 MB 源码), 按 assets/README.md 的约定放在仓库根的
# assets/fonts/ 下。它们不在本组件目录里, 所以用标准 target_sources 追加。
# 重新生成: python3 tools/gen_pet_fonts.py
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pet_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pet_font_20.c"
)
