idf_component_register(
    SRCS "video_player.c" "app_stream_adapter.c" "app_extractor.c" "emotion_video_controller.c"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_audio_codec cap_lua claw_cap json
    PRIV_REQUIRES esp_board_manager esp_codec_dev esp_lcd_touch display_arbiter esp_mm esp_driver_jpeg
)
