# 正常使用 - 智能语音助手
idf_component_register(SRCS "main.cc" "bsp_board.cc" "audio_manager.cc" "wifi_manager.cc" "websocket_client.cc"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_wifi nvs_flash esp_timer esp_driver_i2s esp_driver_gpio esp-sr)

# 【测试用】音频硬件测试程序
# 如果需要测试功放硬件，临时改成：
# idf_component_register(SRCS "audio_test.cc"
#                        INCLUDE_DIRS "."
#                        REQUIRES driver nvs_flash esp_timer)
