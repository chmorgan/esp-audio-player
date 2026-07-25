#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
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

    void feed(const std::vector<uint8_t> &bytes) {
        REQUIRE(http_stream_test_runtime::feed_ring_buffer(
            bytes.data(), bytes.size()));
    }

    void run_producer() {
        http_stream_test_runtime::run_captured_task();
    }

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

std::vector<uint8_t> make_wav(bool include_unknown_chunk) {
    constexpr uint32_t fmt_size = 16;
    constexpr uint32_t junk_size = 4;
    constexpr std::array<int16_t, 2> samples = {-1, 1};
    constexpr uint32_t data_size = samples.size() * sizeof(samples[0]);
    const uint32_t riff_size =
        4 + 8 + fmt_size +
        (include_unknown_chunk ? 8 + junk_size : 0) +
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

    if (include_unknown_chunk) {
        append_fourcc(bytes, "JUNK");
        append_u32_le(bytes, junk_size);
        bytes.insert(bytes.end(), {1, 2, 3, 4});
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

void test_cur_seek_discards_uncached_bytes() {
    const std::vector<uint8_t> input = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    HttpStream stream;
    stream.feed(input);

    REQUIRE(read_bytes(stream.io(), 3) == slice(input, 0, 3));
    REQUIRE(audio_stream_io_seek(stream.io(), 4, AUDIO_STREAM_SEEK_CUR) ==
            ESP_OK);
    REQUIRE(tell(stream.io()) == 7);
    REQUIRE(read_bytes(stream.io(), 3) == slice(input, 7, 10));
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

void test_plain_mp3_probe_rewinds_to_start() {
    const std::vector<uint8_t> input = {0xff, 0xfb, 0x90, 1, 2, 3};
    HttpStream stream;
    stream.feed(input);

    REQUIRE(is_mp3(stream.io()));
    REQUIRE(tell(stream.io()) == 0);
    REQUIRE(read_bytes(stream.io(), 3) == slice(input, 0, 3));
}

void test_id3_mp3_probe_lands_at_first_frame() {
    const std::vector<uint8_t> input = {
        'I', 'D', '3', 4, 0, 0, 0, 0, 0, 4,
        1,   2,   3,   4,
        0xff, 0xfb, 0x90, 0};
    HttpStream stream;
    stream.feed(input);

    REQUIRE(is_mp3(stream.io()));
    REQUIRE(tell(stream.io()) == 14);
    REQUIRE(read_bytes(stream.io(), 3) == slice(input, 14, 17));
}

void test_minimal_wav_probe_lands_at_audio_data() {
    const std::vector<uint8_t> input = make_wav(false);
    HttpStream stream;
    stream.feed(input);

    wav_instance instance{};
    REQUIRE(is_wav(stream.io(), &instance));
    REQUIRE(tell(stream.io()) == 44);
    REQUIRE(read_bytes(stream.io(), 4) == slice(input, 44, 48));
}

void test_wav_probe_skips_unknown_chunk() {
    const std::vector<uint8_t> input = make_wav(true);
    HttpStream stream;
    stream.feed(input);

    wav_instance instance{};
    REQUIRE(is_wav(stream.io(), &instance));
    REQUIRE(tell(stream.io()) == 56);
    REQUIRE(read_bytes(stream.io(), 4) == slice(input, 56, 60));
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
    run_test("SEEK_CUR discards uncached bytes",
             test_cur_seek_discards_uncached_bytes, failures);
    run_test("rewind at 8 KiB boundary",
             test_rewind_at_initial_buffer_boundary, failures);
    run_test("rewind rejected past 8 KiB boundary",
             test_rewind_rejected_after_initial_buffer_boundary, failures);
    run_test("buffered bytes readable after producer EOF",
             test_buffered_bytes_are_readable_after_producer_eof, failures);
    run_test("EOF after buffered bytes are drained",
             test_eof_after_buffer_is_drained, failures);
    run_test("plain MP3 probe rewinds",
             test_plain_mp3_probe_rewinds_to_start, failures);
    run_test("ID3 MP3 probe lands at frame",
             test_id3_mp3_probe_lands_at_first_frame, failures);
    run_test("minimal WAV probe lands at data",
             test_minimal_wav_probe_lands_at_audio_data, failures);
    run_test("WAV probe skips unknown chunk",
             test_wav_probe_skips_unknown_chunk, failures);
    return failures == 0 ? 0 : 1;
}
