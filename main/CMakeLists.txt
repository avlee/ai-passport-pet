# Codex 宠物应用。
#
# SRCS 里同时保留 BSP 参考示例的演示页(demo_*.c / ui_pixel.c): 它们不会被
# app_main 引用, 链接时由 --gc-sections 丢掉, 但代码留在仓库里, 通过宠物界面的
# 信息面板(长按上键)进入, 见 demo_menu.c。
idf_component_register(
    SRCS "main.c"
         "pet_app.c"
         "pet_ui.c"
         "pet_layout.c"
         "pet_state.c"
         "pet_screen.c"
         "pet_protocol.c"
         "pet_settings.c"
         "pet_pkg.c"
         "pet_slot.c"
         "pet_atlas.c"
         "pet_fonts.c"
         "pet_sound.c"
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
    # esp_partition / spi_flash: 宠物槽要从 pets 分区读并 mmap。
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_partition esp_timer
             esp_wifi nvs_flash spi_flash
    # 宠物不再是编译期资源: 它躺在 partitions.csv 里那个 `pets` 分区中, 由
    # tools/gen_pet_package.py 生成 .pet 包后经 WiFi 从菜单栏应用推下来。
    # 所以这里没有 EMBED_FILES —— 换宠物不需要重新编译, 更不需要重新烧录。
)

# 生成的字库源文件体积很大(~10 MB 源码), 按 assets/README.md 的约定放在仓库根的
# assets/fonts/ 下。它们不在本组件目录里, 所以用标准 target_sources 追加。
# 重新生成: python3 tools/gen_pet_fonts.py
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pet_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pet_font_20.c"
    # 提示音 PCM(约 50 KB rodata), 由 tools/gen_pet_sounds.py 从 .wav 生成。
    "${CMAKE_CURRENT_LIST_DIR}/../assets/music/pet_sound_clips.c"
)
