idf_component_register(SRCS "audio_player_test.c" "audio_mixer_test.c"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES unity test_utils audio_player
                       EMBED_TXTFILES gs-16b-1c-44100hz.mp3)
