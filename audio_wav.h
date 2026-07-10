#pragma once

#include "audio_log.h"
#include "audio_decode_types.h"
#include "audio_stream_io.h"

typedef struct {
    // The "RIFF" chunk descriptor
    uint8_t ChunkID[4];
    int32_t ChunkSize;
    uint8_t Format[4];
    // The "fmt" sub-chunk
    uint8_t Subchunk1ID[4];
    int32_t Subchunk1Size;
    int16_t AudioFormat;
    int16_t NumChannels;
    int32_t SampleRate;
    int32_t ByteRate;
    int16_t BlockAlign;
    int16_t BitsPerSample;
} wav_header_t;

typedef struct {
    // The "data" sub-chunk
    uint8_t SubchunkID[4];
    int32_t SubchunkSize;
} wav_subchunk_header_t;

typedef struct {
    wav_header_t header;
} wav_instance;

bool is_wav(audio_stream_io_handle_t io, wav_instance *pInstance);
DECODE_STATUS decode_wav(audio_stream_io_handle_t io, decode_data *pData, wav_instance *pInstance);
