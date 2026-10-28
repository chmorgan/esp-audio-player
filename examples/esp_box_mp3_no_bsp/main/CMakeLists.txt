idf_component_register(
    SRCS "esp_box_mp3_no_bsp.c" "box_audio.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_codec_dev esp_driver_i2c esp_driver_i2s
    EMBED_FILES "sample.mp3"
)
