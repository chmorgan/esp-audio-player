#include <limits.h>
#include <string.h>
#include "audio_log.h"
#include "audio_mp3.h"
#include "audio_stream_io.h"

static const char *TAG = "mp3";

uint32_t mp3_id3v2_tag_size(const mp3_id3_header_v2_t *tag) {
    return ((tag->size[0] & 0x7F) << 21) |
           ((tag->size[1] & 0x7F) << 14) |
           ((tag->size[2] & 0x7F) << 7) |
           (tag->size[3] & 0x7F);
}

static bool mp3_frame_signature_matches(const uint8_t magic[3]) {
    return (magic[0] == 0xFF) &&
           ((magic[1] == 0xFB) ||
            (magic[1] == 0xF3) ||
            (magic[1] == 0xF2));
}

static bool mp3_id3v2_header_is_valid(const mp3_id3_header_v2_t *tag) {
    if (memcmp("ID3", tag->header, sizeof(tag->header)) != 0) {
        return false;
    }

    if ((static_cast<uint8_t>(tag->ver) == UINT8_MAX) ||
        (static_cast<uint8_t>(tag->revision) == UINT8_MAX)) {
        return false;
    }

    for (char size_byte : tag->size) {
        if ((static_cast<uint8_t>(size_byte) & 0x80U) != 0) {
            return false;
        }
    }

    return true;
}

static bool mp3_rewind_after_probe(audio_stream_io_handle_t io, bool matched) {
    const bool rewound =
        audio_stream_io_seek(io, 0, AUDIO_STREAM_SEEK_SET) == ESP_OK;
    return matched && rewound;
}

static bool mp3_skip_id3_payload(audio_stream_io_handle_t io,
                                 uint32_t payload_size) {
    if (payload_size == 0) {
        return true;
    }

    /*
     * Seek to the last payload byte, then read that byte to prove the declared
     * tag exists. File streams may successfully seek beyond EOF, so seeking
     * directly to the end of the payload would not detect a truncated tag.
     *
     * Limited-seek streams reject an uncached forward seek without moving.
     * In that case, consume the payload sequentially.
     */
    const long seek_distance = static_cast<long>(payload_size - 1U);
    long position_before;
    const esp_err_t tell_result = audio_stream_io_tell(io, &position_before);
    const esp_err_t seek_result =
        audio_stream_io_seek(io, seek_distance, AUDIO_STREAM_SEEK_CUR);
    if (seek_result == ESP_OK) {
        uint8_t last_payload_byte;
        return audio_stream_io_read(io, &last_payload_byte,
                                    sizeof(last_payload_byte)) ==
               sizeof(last_payload_byte);
    }

    bool seek_did_not_move = (seek_result == ESP_ERR_NOT_SUPPORTED);
    if (!seek_did_not_move && (tell_result == ESP_OK)) {
        long position_after;
        seek_did_not_move =
            (audio_stream_io_tell(io, &position_after) == ESP_OK) &&
            (position_after == position_before);
    }
    if (!seek_did_not_move) {
        return false;
    }

    uint8_t discard[256];
    uint32_t remaining = payload_size;
    while (remaining != 0) {
        const size_t to_read =
            remaining < sizeof(discard) ? remaining : sizeof(discard);
        const size_t bytes_read = audio_stream_io_read(io, discard, to_read);
        if (bytes_read == 0) {
            return false;
        }
        remaining -= static_cast<uint32_t>(bytes_read);
    }

    return true;
}

bool is_mp3(audio_stream_io_handle_t io) {
    if (!io ||
        (audio_stream_io_seek(io, 0, AUDIO_STREAM_SEEK_SET) != ESP_OK)) {
        return false;
    }

    // see https://en.wikipedia.org/wiki/List_of_file_signatures
    uint8_t magic[3];
    if (audio_stream_io_read(io, magic, sizeof(magic)) != sizeof(magic)) {
        return mp3_rewind_after_probe(io, false);
    }

    if (mp3_frame_signature_matches(magic)) {
        return mp3_rewind_after_probe(io, true);
    }

    if (memcmp("ID3", magic, sizeof(magic)) != 0) {
        return mp3_rewind_after_probe(io, false);
    }

    if (audio_stream_io_seek(io, 0, AUDIO_STREAM_SEEK_SET) != ESP_OK) {
        return false;
    }

    mp3_id3_header_v2_t tag;
    if ((audio_stream_io_read(io, &tag, sizeof(tag)) != sizeof(tag)) ||
        !mp3_id3v2_header_is_valid(&tag)) {
        return mp3_rewind_after_probe(io, false);
    }

    /*
     * Leave the stream positioned after the ID3v2 tag so decoding starts at
     * the first audio frame. Embedded album art may contain MP3-like sync
     * words that would otherwise derail frame search.
     */
    if (!mp3_skip_id3_payload(io, mp3_id3v2_tag_size(&tag))) {
        return mp3_rewind_after_probe(io, false);
    }

    return true;
}

/**
 * @return true if data remains, false on error or end of file
 */
DECODE_STATUS decode_mp3(HMP3Decoder mp3_decoder, audio_stream_io_handle_t io, decode_data *pData, mp3_instance *pInstance) {
    MP3FrameInfo frame_info;

    size_t unread_bytes = pInstance->bytes_in_data_buf - (pInstance->read_ptr - pInstance->data_buf);

    /* somewhat arbitrary trigger to refill buffer - should always be enough for a full frame */
    if (unread_bytes < 1.25 * MAINBUF_SIZE && !pInstance->eof_reached) {
        uint8_t *write_ptr = pInstance->data_buf + unread_bytes;
        size_t free_space = pInstance->data_buf_size - unread_bytes;

    	/* move last, small chunk from end of buffer to start,
           then fill with new data */
        memmove(pInstance->data_buf, pInstance->read_ptr, unread_bytes);

        size_t nRead = audio_stream_io_read(io, write_ptr, free_space);

        pInstance->bytes_in_data_buf = unread_bytes + nRead;
        pInstance->read_ptr = pInstance->data_buf;

        if ((nRead == 0) || audio_stream_io_eof(io)) {
            pInstance->eof_reached = true;
        }

        long pos = 0;
        audio_stream_io_tell(io, &pos);
        LOGI_2("pos %ld, nRead %d, eof %d", pos, (int)nRead, pInstance->eof_reached);

        unread_bytes = pInstance->bytes_in_data_buf;
    }

    LOGI_3("data_buf 0x%p, read 0x%p", pInstance->data_buf, pInstance->read_ptr);

    if(unread_bytes == 0) {
        LOGI_1("unread_bytes == 0, status done");
        return DECODE_STATUS_DONE;
    }

    if (unread_bytes > static_cast<size_t>(INT_MAX)) {
        ESP_LOGE(TAG, "MP3 input buffer too large: %zu bytes", unread_bytes);
        return DECODE_STATUS_ERROR;
    }
    int bytes_left = static_cast<int>(unread_bytes);

    /* Find MP3 sync word from read buffer */
    int offset = MP3FindSyncWord(pInstance->read_ptr, bytes_left);

    LOGI_2("unread %d, total %d, offset 0x%x(%d)",
            (int)unread_bytes, (int)pInstance->bytes_in_data_buf, offset, offset);

    if (offset >= 0) {
        COMPILE_3(int starting_unread_bytes = bytes_left);
        uint8_t *read_ptr = pInstance->read_ptr + offset; /*!< Data start point */
        uint8_t *decode_start = read_ptr;
        bytes_left -= offset;
        LOGI_3("read 0x%p, unread %d", read_ptr, bytes_left);
        int mp3_dec_err = MP3Decode(mp3_decoder, &read_ptr, &bytes_left, reinterpret_cast<int16_t *>(pData->samples), 0);

        pInstance->read_ptr = read_ptr;

        if(mp3_dec_err == ERR_MP3_NONE) {
            /* Get MP3 frame info */
            MP3GetLastFrameInfo(mp3_decoder, &frame_info);

            pData->fmt.sample_rate = frame_info.samprate;
            pData->fmt.bits_per_sample = frame_info.bitsPerSample;
            pData->fmt.channels = frame_info.nChans;

            pData->frame_count = (frame_info.outputSamps / frame_info.nChans);

            LOGI_2("MP3 decode OK: ch=%d, sr=%d, frames=%d",
                pData->fmt.channels, pData->fmt.sample_rate, pData->frame_count);
            LOGI_3("mp3: channels %d, sr %d, bps %d, frame_count %d, processed %d",
                pData->fmt.channels,
                pData->fmt.sample_rate,
                pData->fmt.bits_per_sample,
                frame_info.outputSamps,
                starting_unread_bytes - bytes_left);
        } else {
            if (pInstance->eof_reached) {
                ESP_LOGE(TAG, "status error %d, but EOF", mp3_dec_err);
                return DECODE_STATUS_DONE;
            } else if (mp3_dec_err == ERR_MP3_MAINDATA_UNDERFLOW) {
                // underflow indicates MP3Decode should be called again
                LOGI_1("underflow read ptr is 0x%p", read_ptr);
                return DECODE_STATUS_NO_DATA_CONTINUE;
            } else {
                // Invalid frame header - skip ahead to find next sync word
                // Don't stay at the same position forever
                ESP_LOGW(TAG, "invalid frame header %d, advancing to find next sync", mp3_dec_err);
                pData->frame_count = 0;

                uint8_t *data_end = pInstance->data_buf + pInstance->bytes_in_data_buf;
                uint8_t *next_read_ptr = pInstance->read_ptr;
                if (next_read_ptr <= decode_start) {
                    next_read_ptr = decode_start + 1;
                } else if (next_read_ptr < data_end) {
                    next_read_ptr += 1;
                }
                if (next_read_ptr > data_end) {
                    next_read_ptr = data_end;
                }
                pInstance->read_ptr = next_read_ptr;

                return DECODE_STATUS_NO_DATA_CONTINUE;
            }
        }
    } else {
        // if we are dropping data there were no frames decoded
        pData->frame_count = 0;

        // drop an even count of words
        size_t words_to_drop = unread_bytes / BYTES_IN_WORD;
        size_t bytes_to_drop = words_to_drop * BYTES_IN_WORD;

        // if the unread bytes is less than BYTES_IN_WORD, we should drop any unread bytes
        // to avoid the situation where the file could have a few extra bytes at the end
        // of the file that isn't at least BYTES_IN_WORD and decoding would get stuck
        if(unread_bytes < BYTES_IN_WORD) {
            bytes_to_drop = unread_bytes;
        }

        // shift the read_ptr to drop the bytes in the buffer
        pInstance->read_ptr += bytes_to_drop;

        /* Sync word not found in frame. Drop data that was read until a word boundary */
        ESP_LOGE(TAG, "MP3 sync word not found, dropping %d bytes", (int)bytes_to_drop);
    }

    return DECODE_STATUS_CONTINUE;
}
