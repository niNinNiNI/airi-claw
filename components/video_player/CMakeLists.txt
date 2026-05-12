idf_component_register(
    SRCS "video_player.c" "app_stream_adapter.c" "app_extractor.c"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_audio_codec
    PRIV_REQUIRES esp_board_manager esp_codec_dev display_arbiter esp_mm esp_driver_jpeg
)
