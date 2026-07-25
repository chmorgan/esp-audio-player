#include <limits.h>
#include <string.h>
#include "audio_wav.h"
#include "audio_stream_io.h"

static const char *TAG = "wav";

static uint16_t read_u16_le(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

static uint32_t read_u32_le(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

static bool read_exact(audio_stream_io_handle_t io, void *buffer, size_t size) {
    uint8_t *write_ptr = static_cast<uint8_t *>(buffer);
    size_t total_read = 0;

    while(total_read < size) {
        size_t bytes_read = audio_stream_io_read(io, write_ptr + total_read,
                                                 size - total_read);
        if(bytes_read == 0) {
            return false;
        }
        total_read += bytes_read;
    }

    return true;
}

static bool discard_exact(audio_stream_io_handle_t io, uint64_t byte_count) {
    uint8_t discard_buffer[256];

    while(byte_count != 0) {
        size_t bytes_to_read =
            byte_count < sizeof(discard_buffer)
                ? static_cast<size_t>(byte_count)
                : sizeof(discard_buffer);
        if(!read_exact(io, discard_buffer, bytes_to_read)) {
            return false;
        }
        byte_count -= bytes_to_read;
    }

    return true;
}

static bool skip_bytes(audio_stream_io_handle_t io, uint64_t byte_count) {
    while(byte_count != 0) {
        long offset = byte_count > static_cast<uint64_t>(LONG_MAX)
                          ? LONG_MAX
                          : static_cast<long>(byte_count);
        long position_before;
        esp_err_t tell_result = audio_stream_io_tell(io, &position_before);
        esp_err_t seek_result =
            audio_stream_io_seek(io, offset, AUDIO_STREAM_SEEK_CUR);

        if(seek_result != ESP_OK) {
            bool seek_did_not_move = (seek_result == ESP_ERR_NOT_SUPPORTED);
            if(!seek_did_not_move && (tell_result == ESP_OK)) {
                long position_after;
                seek_did_not_move =
                    (audio_stream_io_tell(io, &position_after) == ESP_OK) &&
                    (position_after == position_before);
            }

            if(!seek_did_not_move ||
               !discard_exact(io, static_cast<uint64_t>(offset))) {
                return false;
            }
        }
        byte_count -= static_cast<uint64_t>(offset);
    }

    return true;
}

static bool valid_pcm_format(const wav_header_t *header) {
    if((header->AudioFormat != 1) ||
       (header->NumChannels == 0) ||
       (header->SampleRate == 0) ||
       (header->SampleRate > INT_MAX) ||
       (header->BitsPerSample == 0) ||
       ((header->BitsPerSample % BITS_PER_BYTE) != 0)) {
        return false;
    }

    uint64_t bytes_per_frame =
        static_cast<uint64_t>(header->NumChannels) *
        (header->BitsPerSample / BITS_PER_BYTE);
    uint64_t byte_rate =
        static_cast<uint64_t>(header->SampleRate) * bytes_per_frame;

    return (bytes_per_frame <= UINT16_MAX) &&
           (header->BlockAlign == bytes_per_frame) &&
           (byte_rate <= UINT32_MAX) &&
           (header->ByteRate == byte_rate);
}

/**
 * @param io
 * @param pInstance - Values can be considered valid if true is returned
 * @return true if stream is a wav file
 */
bool is_wav(audio_stream_io_handle_t io, wav_instance *pInstance) {
    if((io == NULL) || (pInstance == NULL)) {
        return false;
    }

    memset(pInstance, 0, sizeof(*pInstance));
    if(audio_stream_io_seek(io, 0, AUDIO_STREAM_SEEK_SET) != ESP_OK) {
        return false;
    }

    uint8_t riff_header[12];
    if(!read_exact(io, riff_header, sizeof(riff_header))) {
        return false;
    }

    wav_header_t *wav_head = &pInstance->header;
    memcpy(wav_head->ChunkID, riff_header, sizeof(wav_head->ChunkID));
    wav_head->ChunkSize = read_u32_le(riff_header + 4);
    memcpy(wav_head->Format, riff_header + 8, sizeof(wav_head->Format));

    if((memcmp(wav_head->ChunkID, "RIFF", sizeof(wav_head->ChunkID)) != 0) ||
       (memcmp(wav_head->Format, "WAVE", sizeof(wav_head->Format)) != 0) ||
       (wav_head->ChunkSize < sizeof(wav_head->Format))) {
        return false;
    }

    uint64_t riff_remaining =
        static_cast<uint64_t>(wav_head->ChunkSize) - sizeof(wav_head->Format);
    bool found_format = false;

    while(riff_remaining >= sizeof(wav_subchunk_header_t)) {
        uint8_t chunk_header[8];
        if(!read_exact(io, chunk_header, sizeof(chunk_header))) {
            return false;
        }

        wav_subchunk_header_t subchunk;
        memcpy(subchunk.SubchunkID, chunk_header, sizeof(subchunk.SubchunkID));
        subchunk.SubchunkSize = read_u32_le(chunk_header + 4);
        riff_remaining -= sizeof(chunk_header);

        uint64_t padded_chunk_size =
            static_cast<uint64_t>(subchunk.SubchunkSize) +
            (subchunk.SubchunkSize & 1U);
        if(padded_chunk_size > riff_remaining) {
            return false;
        }

        if(memcmp(subchunk.SubchunkID, "fmt ", 4) == 0) {
            if(found_format || (subchunk.SubchunkSize < 16)) {
                return false;
            }

            uint8_t format_data[16];
            if(!read_exact(io, format_data, sizeof(format_data))) {
                return false;
            }

            memcpy(wav_head->Subchunk1ID, subchunk.SubchunkID,
                   sizeof(wav_head->Subchunk1ID));
            wav_head->Subchunk1Size = subchunk.SubchunkSize;
            wav_head->AudioFormat = read_u16_le(format_data);
            wav_head->NumChannels = read_u16_le(format_data + 2);
            wav_head->SampleRate = read_u32_le(format_data + 4);
            wav_head->ByteRate = read_u32_le(format_data + 8);
            wav_head->BlockAlign = read_u16_le(format_data + 12);
            wav_head->BitsPerSample = read_u16_le(format_data + 14);

            if(!valid_pcm_format(wav_head) ||
               !skip_bytes(io, padded_chunk_size - sizeof(format_data))) {
                return false;
            }
            found_format = true;
        } else if(memcmp(subchunk.SubchunkID, "data", 4) == 0) {
            if(!found_format ||
               ((subchunk.SubchunkSize % wav_head->BlockAlign) != 0)) {
                return false;
            }

            pInstance->data_size = subchunk.SubchunkSize;
            pInstance->data_remaining = subchunk.SubchunkSize;

            LOGI_2("sample_rate=%d, channels=%d, bps=%d",
                    static_cast<int>(wav_head->SampleRate),
                    static_cast<int>(wav_head->NumChannels),
                    static_cast<int>(wav_head->BitsPerSample));
            return true;
        } else if(!skip_bytes(io, padded_chunk_size)) {
            return false;
        }

        riff_remaining -= padded_chunk_size;
    }

    return false;
}

/**
 * @return true if data remains, false on error or end of file
 */
DECODE_STATUS decode_wav(audio_stream_io_handle_t io, decode_data *pData, wav_instance *pInstance) {
    if((io == NULL) || (pData == NULL) || (pInstance == NULL)) {
        return DECODE_STATUS_ERROR;
    }

    pData->frame_count = 0;
    pData->fmt.channels = pInstance->header.NumChannels;
    pData->fmt.bits_per_sample = pInstance->header.BitsPerSample;
    pData->fmt.sample_rate = static_cast<int>(pInstance->header.SampleRate);

    if(pInstance->data_remaining == 0) {
        return DECODE_STATUS_DONE;
    }

    // read an even multiple of frames that can fit into output_samples buffer, otherwise
    // we would have to manage what happens with partial frames in the output buffer
    size_t bytes_per_frame = pInstance->header.BlockAlign;
    if((pData->samples == NULL) ||
       (bytes_per_frame == 0) ||
       (pData->samples_capacity < bytes_per_frame)) {
        return DECODE_STATUS_ERROR;
    }

    size_t frames_to_read = pData->samples_capacity / bytes_per_frame;
    size_t bytes_to_read = frames_to_read * bytes_per_frame;
    if(bytes_to_read > pInstance->data_remaining) {
        bytes_to_read = pInstance->data_remaining;
    }

    size_t bytes_read = audio_stream_io_read(io, pData->samples, bytes_to_read);
    if((bytes_read > bytes_to_read) || ((bytes_read % bytes_per_frame) != 0)) {
        return DECODE_STATUS_ERROR;
    }

    pInstance->data_remaining -= static_cast<uint32_t>(bytes_read);
    pData->frame_count = bytes_read / bytes_per_frame;

    LOGI_2("bytes_per_frame %d, bytes_to_read %d, bytes_read %d, frame_count %d",
            (int)bytes_per_frame, (int)bytes_to_read, (int)bytes_read,
            (int)pData->frame_count);

    if(bytes_read == 0) {
        // A zero-byte read means EOF or an I/O error. Either is premature
        // while the data chunk still has declared bytes remaining.
        return DECODE_STATUS_ERROR;
    }

    if((bytes_read < bytes_to_read) && audio_stream_io_eof(io)) {
        pData->frame_count = 0;
        return DECODE_STATUS_ERROR;
    }

    return DECODE_STATUS_CONTINUE;
}
