#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "freertos/FreeRTOS.h"

#include "audio_http_stream.h"
#include "audio_mp3.h"
#include "audio_stream_io.h"
#include "audio_wav.h"
#include "http_stream_test_runtime.h"

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            throw std::runtime_error(std::string("requirement failed: ") +       \
                                     #condition + " (line " +                    \
                                     std::to_string(__LINE__) + ")");            \
        }                                                                         \
    } while (0)

namespace {

constexpr size_t kInitialBufferSize = 8 * 1024;

class HttpStream {
public:
    HttpStream() {
        http_stream_test_runtime::reset();
        open();
    }

    HttpStream(const std::vector<uint8_t> &response, size_t chunk_size) {
        http_stream_test_runtime::reset();
        http_stream_test_runtime::configure_http_response(
            response.data(), response.size(), chunk_size);
        open();
    }

    ~HttpStream() {
        audio_stream_io_close(io_);
        audio_http_stream_close(stream_);
        http_stream_test_runtime::reset();
    }

    HttpStream(const HttpStream &) = delete;
    HttpStream &operator=(const HttpStream &) = delete;

    audio_stream_io_handle_t io() const {
        return io_;
    }

    // These methods require the runtime owned by a live HttpStream instance.
    // cppcheck-suppress-begin functionStatic
    void feed(const std::vector<uint8_t> &bytes) {
        REQUIRE(http_stream_test_runtime::feed_ring_buffer(
            bytes.data(), bytes.size()));
    }

    void run_producer() {
        http_stream_test_runtime::run_captured_task();
    }
    // cppcheck-suppress-end functionStatic

private:
    void open() {
        audio_http_stream_config_t config =
            DEFAULT_AUDIO_HTTP_STREAM_CONFIG("http://host.test/audio");
        stream_ = audio_http_stream_open(&config);
        REQUIRE(stream_ != nullptr);
        REQUIRE(audio_http_stream_get_io(stream_, &io_) == ESP_OK);
        REQUIRE(io_ != nullptr);
    }

    audio_http_stream_handle_t stream_ = nullptr;
    audio_stream_io_handle_t io_ = nullptr;
};

class SeekFailureStream {
public:
    SeekFailureStream(std::vector<uint8_t> bytes, size_t failed_seek_call)
        : bytes_(std::move(bytes)) {
        context_.bytes = &bytes_;
        context_.failed_seek_call = failed_seek_call;
        io_ = audio_stream_io_create(&ops_, &context_);
        REQUIRE(io_ != nullptr);
    }

    ~SeekFailureStream() {
        audio_stream_io_close(io_);
    }

    SeekFailureStream(const SeekFailureStream &) = delete;
    SeekFailureStream &operator=(const SeekFailureStream &) = delete;

    audio_stream_io_handle_t io() const {
        return io_;
    }

    size_t position() const {
        return context_.position;
    }

    size_t read_calls() const {
        return context_.read_calls;
    }

    size_t seek_calls() const {
        return context_.seek_calls;
    }

private:
    struct Context {
        const std::vector<uint8_t> *bytes = nullptr;
        size_t position = 0;
        size_t read_calls = 0;
        size_t seek_calls = 0;
        size_t failed_seek_call = 0;
    };

    static size_t read(void *ctx, void *buf, size_t size) {
        auto *context = static_cast<Context *>(ctx);
        ++context->read_calls;

        const size_t available = context->bytes->size() - context->position;
        const size_t to_copy = size < available ? size : available;
        if (to_copy != 0) {
            memcpy(buf, context->bytes->data() + context->position, to_copy);
            context->position += to_copy;
        }
        return to_copy;
    }

    static int seek(void *ctx, long offset, int whence) {
        auto *context = static_cast<Context *>(ctx);
        ++context->seek_calls;
        if (context->seek_calls == context->failed_seek_call) {
            return -1;
        }
        if ((whence != AUDIO_STREAM_SEEK_SET) || (offset < 0) ||
            (static_cast<size_t>(offset) > context->bytes->size())) {
            return -1;
        }

        context->position = static_cast<size_t>(offset);
        return 0;
    }

    // audio_stream_io_ops_t defines callbacks with a mutable context pointer.
    // cppcheck-suppress constParameterCallback
    static long tell(void *ctx) {
        const auto *context = static_cast<const Context *>(ctx);
        return static_cast<long>(context->position);
    }

    inline static const audio_stream_io_ops_t ops_ = {
        .read = read,
        .seek = seek,
        .tell = tell,
        .eof = nullptr,
        .close = nullptr,
    };

    std::vector<uint8_t> bytes_;
    Context context_;
    audio_stream_io_handle_t io_ = nullptr;
};

long tell(audio_stream_io_handle_t io) {
    long position = -1;
    REQUIRE(audio_stream_io_tell(io, &position) == ESP_OK);
    return position;
}

std::vector<uint8_t> read_bytes(audio_stream_io_handle_t io, size_t size) {
    std::vector<uint8_t> bytes(size);
    REQUIRE(audio_stream_io_read(io, bytes.data(), bytes.size()) == bytes.size());
    return bytes;
}

std::vector<uint8_t> slice(const std::vector<uint8_t> &bytes,
                           size_t begin,
                           size_t end) {
    return {bytes.begin() + static_cast<std::ptrdiff_t>(begin),
            bytes.begin() + static_cast<std::ptrdiff_t>(end)};
}

void append_fourcc(std::vector<uint8_t> &bytes, const char value[5]) {
    bytes.insert(bytes.end(), value, value + 4);
}

void append_u16_le(std::vector<uint8_t> &bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void append_u32_le(std::vector<uint8_t> &bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void append_synchsafe_u32(std::vector<uint8_t> &bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>((value >> 21) & 0x7f));
    bytes.push_back(static_cast<uint8_t>((value >> 14) & 0x7f));
    bytes.push_back(static_cast<uint8_t>((value >> 7) & 0x7f));
    bytes.push_back(static_cast<uint8_t>(value & 0x7f));
}

std::vector<uint8_t> make_id3_mp3(size_t tag_payload_size) {
    REQUIRE(tag_payload_size <= UINT32_C(0x0fffffff));

    std::vector<uint8_t> bytes = {'I', 'D', '3', 4, 0, 0};
    append_synchsafe_u32(bytes, static_cast<uint32_t>(tag_payload_size));
    for (size_t i = 0; i < tag_payload_size; ++i) {
        bytes.push_back(static_cast<uint8_t>(i % 251));
    }
    bytes.insert(bytes.end(), {0xff, 0xfb, 0x90, 0});
    return bytes;
}

std::vector<uint8_t> make_wav(size_t unknown_chunk_size) {
    constexpr uint32_t fmt_size = 16;
    constexpr std::array<int16_t, 2> samples = {-1, 1};
    constexpr uint32_t data_size = samples.size() * sizeof(samples[0]);
    const uint32_t padded_unknown_chunk_size =
        static_cast<uint32_t>(unknown_chunk_size + (unknown_chunk_size & 1U));
    const uint32_t riff_size =
        4 + 8 + fmt_size +
        (unknown_chunk_size != 0 ? 8 + padded_unknown_chunk_size : 0) +
        8 + data_size;

    std::vector<uint8_t> bytes;
    append_fourcc(bytes, "RIFF");
    append_u32_le(bytes, riff_size);
    append_fourcc(bytes, "WAVE");
    append_fourcc(bytes, "fmt ");
    append_u32_le(bytes, fmt_size);
    append_u16_le(bytes, 1);
    append_u16_le(bytes, 1);
    append_u32_le(bytes, 8000);
    append_u32_le(bytes, 16000);
    append_u16_le(bytes, 2);
    append_u16_le(bytes, 16);

    if (unknown_chunk_size != 0) {
        append_fourcc(bytes, "JUNK");
        append_u32_le(bytes, static_cast<uint32_t>(unknown_chunk_size));
        for (size_t i = 0; i < unknown_chunk_size; ++i) {
            bytes.push_back(static_cast<uint8_t>(i % 251));
        }
        if ((unknown_chunk_size & 1U) != 0) {
            bytes.push_back(0);
        }
    }

    append_fourcc(bytes, "data");
    append_u32_le(bytes, data_size);
    for (int16_t sample : samples) {
        append_u16_le(bytes, static_cast<uint16_t>(sample));
    }
    return bytes;
}

void test_sequential_reads_do_not_duplicate_data() {
    const std::vector<uint8_t> input = {
        'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    HttpStream stream;
    stream.feed(input);

    REQUIRE(read_bytes(stream.io(), 3) == slice(input, 0, 3));
    REQUIRE(read_bytes(stream.io(), 3) == slice(input, 3, 6));
    REQUIRE(read_bytes(stream.io(), 2) == slice(input, 6, 8));
}

void test_tell_tracks_consumer_position() {
    const std::vector<uint8_t> input = {0, 1, 2, 3, 4, 5, 6, 7};
    HttpStream stream;
    stream.feed(input);

    REQUIRE(tell(stream.io()) == 0);
    REQUIRE(read_bytes(stream.io(), 3) == slice(input, 0, 3));
    REQUIRE(tell(stream.io()) == 3);
    REQUIRE(read_bytes(stream.io(), 2) == slice(input, 3, 5));
    REQUIRE(tell(stream.io()) == 5);
}

void test_set_rewind_replays_cached_data_then_continues() {
    const std::vector<uint8_t> input = {
        10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21};
    HttpStream stream;
    stream.feed(input);

    REQUIRE(read_bytes(stream.io(), 6) == slice(input, 0, 6));
    REQUIRE(audio_stream_io_seek(stream.io(), 0, AUDIO_STREAM_SEEK_SET) ==
            ESP_OK);
    REQUIRE(tell(stream.io()) == 0);
    REQUIRE(read_bytes(stream.io(), 8) == slice(input, 0, 8));
    REQUIRE(tell(stream.io()) == 8);
}

void test_cur_and_invalid_seeks_preserve_position() {
    const std::vector<uint8_t> input = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    HttpStream stream;
    stream.feed(input);

    REQUIRE(read_bytes(stream.io(), 8) == slice(input, 0, 8));
    REQUIRE(audio_stream_io_seek(stream.io(), 3, AUDIO_STREAM_SEEK_SET) ==
            ESP_OK);
    REQUIRE(audio_stream_io_seek(stream.io(), 4, AUDIO_STREAM_SEEK_CUR) ==
            ESP_OK);
    REQUIRE(tell(stream.io()) == 7);
    REQUIRE(read_bytes(stream.io(), 1) == slice(input, 7, 8));

    const long position = tell(stream.io());
    REQUIRE(audio_stream_io_seek(stream.io(), -1, AUDIO_STREAM_SEEK_SET) !=
            ESP_OK);
    REQUIRE(tell(stream.io()) == position);
    REQUIRE(audio_stream_io_seek(stream.io(), -9, AUDIO_STREAM_SEEK_CUR) !=
            ESP_OK);
    REQUIRE(tell(stream.io()) == position);
    REQUIRE(audio_stream_io_seek(stream.io(), 0, AUDIO_STREAM_SEEK_END) !=
            ESP_OK);
    REQUIRE(tell(stream.io()) == position);
}

void test_uncached_forward_seek_fails_without_moving() {
    const std::vector<uint8_t> input = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    HttpStream stream;
    stream.feed(input);

    REQUIRE(read_bytes(stream.io(), 3) == slice(input, 0, 3));
    const long position = tell(stream.io());
    REQUIRE(audio_stream_io_seek(stream.io(), 4, AUDIO_STREAM_SEEK_CUR) !=
            ESP_OK);
    REQUIRE(tell(stream.io()) == position);
    REQUIRE(read_bytes(stream.io(), 3) == slice(input, 3, 6));
}

std::vector<uint8_t> make_pattern(size_t size) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>(i % 251);
    }
    return bytes;
}

void test_rewind_at_initial_buffer_boundary() {
    const std::vector<uint8_t> input = make_pattern(kInitialBufferSize + 1);
    HttpStream stream;
    stream.feed(input);

    REQUIRE(read_bytes(stream.io(), kInitialBufferSize) ==
            slice(input, 0, kInitialBufferSize));
    REQUIRE(audio_stream_io_seek(stream.io(), 0, AUDIO_STREAM_SEEK_SET) ==
            ESP_OK);
    REQUIRE(read_bytes(stream.io(), kInitialBufferSize) ==
            slice(input, 0, kInitialBufferSize));
    REQUIRE(read_bytes(stream.io(), 1) ==
            slice(input, kInitialBufferSize, kInitialBufferSize + 1));
}

void test_rewind_rejected_after_initial_buffer_boundary() {
    const std::vector<uint8_t> input = make_pattern(kInitialBufferSize + 1);
    HttpStream stream;
    stream.feed(input);

    REQUIRE(read_bytes(stream.io(), input.size()) == input);
    REQUIRE(tell(stream.io()) == static_cast<long>(input.size()));
    REQUIRE(audio_stream_io_seek(stream.io(), 0, AUDIO_STREAM_SEEK_SET) !=
            ESP_OK);
    REQUIRE(tell(stream.io()) == static_cast<long>(input.size()));
}

void test_buffered_bytes_are_readable_after_producer_eof() {
    const std::vector<uint8_t> input = {
        'b', 'u', 'f', 'f', 'e', 'r', 'e', 'd'};
    HttpStream stream(input, 3);
    stream.run_producer();

    REQUIRE(!audio_stream_io_eof(stream.io()));
    REQUIRE(read_bytes(stream.io(), input.size()) == input);
}

void test_eof_after_buffer_is_drained() {
    const std::vector<uint8_t> input = {'e', 'o', 'f'};
    HttpStream stream(input, 1);
    stream.run_producer();

    REQUIRE(!audio_stream_io_eof(stream.io()));
    REQUIRE(read_bytes(stream.io(), input.size()) == input);
    REQUIRE(audio_stream_io_eof(stream.io()));
}

void test_rewind_replays_data_after_producer_eof() {
    const std::vector<uint8_t> input = {
        'r', 'e', 'w', 'i', 'n', 'd', '-', 'e', 'o', 'f'};
    HttpStream stream(input, 3);
    stream.run_producer();

    REQUIRE(read_bytes(stream.io(), 4) == slice(input, 0, 4));
    REQUIRE(audio_stream_io_seek(stream.io(), 0, AUDIO_STREAM_SEEK_SET) ==
            ESP_OK);
    REQUIRE(!audio_stream_io_eof(stream.io()));
    REQUIRE(read_bytes(stream.io(), input.size()) == input);
    REQUIRE(audio_stream_io_eof(stream.io()));
}

void test_plain_mp3_probe_rewinds_to_start() {
    const std::vector<uint8_t> input = {0xff, 0xfb, 0x90, 1, 2, 3};
    HttpStream stream;
    stream.feed(input);

    REQUIRE(is_mp3(stream.io()));
    REQUIRE(tell(stream.io()) == 0);
    REQUIRE(read_bytes(stream.io(), 3) == slice(input, 0, 3));
}

void test_id3_mp3_probe_lands_at_first_frame() {
    const std::vector<uint8_t> input = make_id3_mp3(4);
    HttpStream stream;
    stream.feed(input);

    REQUIRE(is_mp3(stream.io()));
    REQUIRE(tell(stream.io()) == 14);
    REQUIRE(read_bytes(stream.io(), 3) == slice(input, 14, 17));
}

void test_large_id3_mp3_probe_lands_at_first_frame() {
    constexpr size_t tag_payload_size = kInitialBufferSize + 1;
    const std::vector<uint8_t> input = make_id3_mp3(tag_payload_size);
    const size_t frame_offset = 10 + tag_payload_size;
    HttpStream stream;
    stream.feed(input);

    REQUIRE(is_mp3(stream.io()));
    REQUIRE(tell(stream.io()) == static_cast<long>(frame_offset));
    REQUIRE(read_bytes(stream.io(), 3) ==
            slice(input, frame_offset, frame_offset + 3));
}

void test_truncated_declared_id3_tag_is_rejected() {
    std::vector<uint8_t> input = {'I', 'D', '3', 4, 0, 0};
    append_synchsafe_u32(input, 32);
    input.insert(input.end(), {1, 2, 3, 4});
    HttpStream stream;
    stream.feed(input);

    REQUIRE(!is_mp3(stream.io()));
}

void test_failed_initial_mp3_seek_does_not_consume_data() {
    SeekFailureStream stream({0xff, 0xfb, 0x90, 1, 2, 3}, 1);

    REQUIRE(!is_mp3(stream.io()));
    REQUIRE(stream.position() == 0);
    REQUIRE(stream.read_calls() == 0);
    REQUIRE(stream.seek_calls() == 1);
}

void test_failed_final_mp3_rewind_does_not_report_success() {
    SeekFailureStream stream({0xff, 0xfb, 0x90, 1, 2, 3}, 2);

    REQUIRE(!is_mp3(stream.io()));
    REQUIRE(stream.position() == 3);
    REQUIRE(stream.read_calls() == 1);
    REQUIRE(stream.seek_calls() == 2);
}

void test_minimal_wav_probe_lands_at_audio_data() {
    const std::vector<uint8_t> input = make_wav(0);
    HttpStream stream;
    stream.feed(input);

    wav_instance instance{};
    REQUIRE(is_wav(stream.io(), &instance));
    REQUIRE(tell(stream.io()) == 44);
    REQUIRE(read_bytes(stream.io(), 4) == slice(input, 44, 48));
}

void test_wav_probe_skips_unknown_chunk() {
    const std::vector<uint8_t> input = make_wav(4);
    HttpStream stream;
    stream.feed(input);

    wav_instance instance{};
    REQUIRE(is_wav(stream.io(), &instance));
    REQUIRE(tell(stream.io()) == 56);
    REQUIRE(read_bytes(stream.io(), 4) == slice(input, 56, 60));
}

void test_wav_probe_skips_large_unknown_chunk() {
    constexpr size_t unknown_chunk_size = kInitialBufferSize + 2;
    constexpr size_t data_offset = 52 + unknown_chunk_size;
    const std::vector<uint8_t> input = make_wav(unknown_chunk_size);
    HttpStream stream;
    stream.feed(input);

    wav_instance instance{};
    REQUIRE(is_wav(stream.io(), &instance));
    REQUIRE(tell(stream.io()) == static_cast<long>(data_offset));
    REQUIRE(read_bytes(stream.io(), 4) ==
            slice(input, data_offset, data_offset + 4));
}

void test_wav_probe_skips_odd_unknown_chunk_and_padding() {
    constexpr size_t unknown_chunk_size = 3;
    constexpr size_t data_offset = 52 + unknown_chunk_size + 1;
    const std::vector<uint8_t> input = make_wav(unknown_chunk_size);
    HttpStream stream;
    stream.feed(input);

    wav_instance instance{};
    REQUIRE(is_wav(stream.io(), &instance));
    REQUIRE(tell(stream.io()) == static_cast<long>(data_offset));
    REQUIRE(read_bytes(stream.io(), 4) ==
            slice(input, data_offset, data_offset + 4));
}

void test_wav_probe_succeeds_after_mp3_rejection() {
    const std::vector<uint8_t> input = make_wav(0);
    HttpStream stream;
    stream.feed(input);

    REQUIRE(!is_mp3(stream.io()));
    REQUIRE(tell(stream.io()) == 0);

    wav_instance instance{};
    REQUIRE(is_wav(stream.io(), &instance));
    REQUIRE(tell(stream.io()) == 44);
    REQUIRE(read_bytes(stream.io(), 4) == slice(input, 44, 48));
}

template <typename Function>
void run_test(const char *name, Function function, int &failures) {
    try {
        function();
        std::cout << "PASS: " << name << '\n';
    } catch (const std::exception &error) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
    }
}

}  // namespace

int main() {
    int failures = 0;
    run_test("sequential reads do not duplicate data",
             test_sequential_reads_do_not_duplicate_data, failures);
    run_test("tell tracks consumer position",
             test_tell_tracks_consumer_position, failures);
    run_test("SEEK_SET rewind preserves data",
             test_set_rewind_replays_cached_data_then_continues, failures);
    run_test("SEEK_CUR and invalid seeks",
             test_cur_and_invalid_seeks_preserve_position, failures);
    run_test("uncached forward seek fails without moving",
             test_uncached_forward_seek_fails_without_moving, failures);
    run_test("rewind at 8 KiB boundary",
             test_rewind_at_initial_buffer_boundary, failures);
    run_test("rewind rejected past 8 KiB boundary",
             test_rewind_rejected_after_initial_buffer_boundary, failures);
    run_test("buffered bytes readable after producer EOF",
             test_buffered_bytes_are_readable_after_producer_eof, failures);
    run_test("EOF after buffered bytes are drained",
             test_eof_after_buffer_is_drained, failures);
    run_test("rewind replays data after producer EOF",
             test_rewind_replays_data_after_producer_eof, failures);
    run_test("plain MP3 probe rewinds",
             test_plain_mp3_probe_rewinds_to_start, failures);
    run_test("ID3 MP3 probe lands at frame",
             test_id3_mp3_probe_lands_at_first_frame, failures);
    run_test("large ID3 MP3 probe lands at frame",
             test_large_id3_mp3_probe_lands_at_first_frame, failures);
    run_test("truncated declared ID3 tag is rejected",
             test_truncated_declared_id3_tag_is_rejected, failures);
    run_test("failed initial MP3 seek does not consume data",
             test_failed_initial_mp3_seek_does_not_consume_data, failures);
    run_test("failed final MP3 rewind does not report success",
             test_failed_final_mp3_rewind_does_not_report_success, failures);
    run_test("minimal WAV probe lands at data",
             test_minimal_wav_probe_lands_at_audio_data, failures);
    run_test("WAV probe skips unknown chunk",
             test_wav_probe_skips_unknown_chunk, failures);
    run_test("WAV probe skips large unknown chunk",
             test_wav_probe_skips_large_unknown_chunk, failures);
    run_test("WAV probe skips odd unknown chunk and padding",
             test_wav_probe_skips_odd_unknown_chunk_and_padding, failures);
    run_test("WAV probe succeeds after MP3 rejection",
             test_wav_probe_succeeds_after_mp3_rejection, failures);
    return failures == 0 ? 0 : 1;
}
