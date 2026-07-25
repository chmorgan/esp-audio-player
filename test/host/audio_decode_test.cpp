#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "audio_mp3.h"
#include "audio_stream_io.h"
#include "audio_wav.h"

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            throw std::runtime_error(std::string("requirement failed: ") +       \
                                     #condition + " (line " +                    \
                                     std::to_string(__LINE__) + ")");            \
        }                                                                         \
    } while (0)

namespace {

constexpr size_t kMp3InputBufferSize = MAINBUF_SIZE * 3;
constexpr size_t kMp3OutputSampleCount = MAX_NCHAN * MAX_NGRAN * MAX_NSAMP;

static_assert(sizeof(wav_header_t) == 36);
static_assert(sizeof(wav_subchunk_header_t) == 8);

class Stream {
public:
    explicit Stream(audio_stream_io_handle_t handle) : handle_(handle) {
        REQUIRE(handle_ != nullptr);
    }

    ~Stream() {
        audio_stream_io_close(handle_);
    }

    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    audio_stream_io_handle_t get() const {
        return handle_;
    }

private:
    audio_stream_io_handle_t handle_;
};

class Mp3Decoder {
public:
    Mp3Decoder() : handle_(MP3InitDecoder()) {
        REQUIRE(handle_ != nullptr);
    }

    ~Mp3Decoder() {
        MP3FreeDecoder(handle_);
    }

    Mp3Decoder(const Mp3Decoder &) = delete;
    Mp3Decoder &operator=(const Mp3Decoder &) = delete;

    HMP3Decoder get() const {
        return handle_;
    }

private:
    HMP3Decoder handle_;
};

using File = std::unique_ptr<FILE, decltype(&fclose)>;

audio_stream_io_handle_t open_file_stream(const std::filesystem::path &path) {
    FILE *file = fopen(path.string().c_str(), "rb");
    if (!file) {
        throw std::runtime_error("unable to open file stream: " + path.string());
    }

    audio_stream_io_handle_t stream = audio_stream_io_from_file(file);
    if (!stream) {
        fclose(file);
        throw std::runtime_error("unable to create file stream: " + path.string());
    }
    return stream;
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
    constexpr uint16_t channels = 1;
    constexpr uint32_t sample_rate = 8000;
    constexpr uint16_t bits_per_sample = 16;
    constexpr std::array<int16_t, 4> samples = {-32768, -1, 0, 32767};
    constexpr uint32_t data_size = samples.size() * sizeof(samples[0]);
    constexpr uint32_t fmt_chunk_size = 16;
    constexpr uint32_t unknown_chunk_size = 4;

    const uint32_t riff_size =
        4 +
        8 + fmt_chunk_size +
        (include_unknown_chunk ? 8 + unknown_chunk_size : 0) +
        8 + data_size;

    std::vector<uint8_t> bytes;
    bytes.reserve(riff_size + 8);

    append_fourcc(bytes, "RIFF");
    append_u32_le(bytes, riff_size);
    append_fourcc(bytes, "WAVE");
    append_fourcc(bytes, "fmt ");
    append_u32_le(bytes, fmt_chunk_size);
    append_u16_le(bytes, 1);
    append_u16_le(bytes, channels);
    append_u32_le(bytes, sample_rate);
    append_u32_le(bytes, sample_rate * channels * bits_per_sample / 8);
    append_u16_le(bytes, channels * bits_per_sample / 8);
    append_u16_le(bytes, bits_per_sample);

    if (include_unknown_chunk) {
        append_fourcc(bytes, "JUNK");
        append_u32_le(bytes, unknown_chunk_size);
        bytes.insert(bytes.end(), {1, 2, 3, 4});
    }

    append_fourcc(bytes, "data");
    append_u32_le(bytes, data_size);
    for (int16_t sample : samples) {
        append_u16_le(bytes, static_cast<uint16_t>(sample));
    }

    REQUIRE(bytes.size() == riff_size + 8);
    return bytes;
}

std::vector<uint8_t> read_binary_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to open binary fixture: " + path.string());
    }

    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    if (bytes.empty()) {
        throw std::runtime_error("binary fixture is empty: " + path.string());
    }

    return bytes;
}

void test_file_stream_contract() {
    constexpr std::array<uint8_t, 10> data = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    File file(tmpfile(), &fclose);
    REQUIRE(file != nullptr);
    REQUIRE(fwrite(data.data(), 1, data.size(), file.get()) == data.size());
    rewind(file.get());

    {
        Stream stream(audio_stream_io_from_file_no_close(file.get()));
        long position = -1;
        REQUIRE(audio_stream_io_tell(stream.get(), &position) == ESP_OK);
        REQUIRE(position == 0);

        std::array<uint8_t, 3> bytes{};
        REQUIRE(audio_stream_io_read(stream.get(), bytes.data(), bytes.size()) ==
                bytes.size());
        REQUIRE((bytes == std::array<uint8_t, 3>{0, 1, 2}));
        REQUIRE(audio_stream_io_tell(stream.get(), &position) == ESP_OK);
        REQUIRE(position == 3);

        REQUIRE(audio_stream_io_seek(stream.get(), 5, AUDIO_STREAM_SEEK_SET) ==
                ESP_OK);
        REQUIRE(audio_stream_io_tell(stream.get(), &position) == ESP_OK);
        REQUIRE(position == 5);

        REQUIRE(audio_stream_io_seek(stream.get(), -2, AUDIO_STREAM_SEEK_CUR) ==
                ESP_OK);
        REQUIRE(audio_stream_io_tell(stream.get(), &position) == ESP_OK);
        REQUIRE(position == 3);

        REQUIRE(audio_stream_io_seek(stream.get(), -1, AUDIO_STREAM_SEEK_END) ==
                ESP_OK);
        REQUIRE(audio_stream_io_tell(stream.get(), &position) == ESP_OK);
        REQUIRE(position == 9);

        uint8_t last_byte = 0;
        REQUIRE(audio_stream_io_read(stream.get(), &last_byte, 1) == 1);
        REQUIRE(last_byte == 9);
        REQUIRE(!audio_stream_io_eof(stream.get()));
        REQUIRE(audio_stream_io_read(stream.get(), &last_byte, 1) == 0);
        REQUIRE(audio_stream_io_eof(stream.get()));
    }

    rewind(file.get());
    std::array<uint8_t, data.size()> reread{};
    REQUIRE(fread(reread.data(), 1, reread.size(), file.get()) == reread.size());
    REQUIRE(reread == data);
}

void test_owned_file_stream_closes_file() {
    File file(tmpfile(), &fclose);
    REQUIRE(file != nullptr);
    const int descriptor = fileno(file.get());
    REQUIRE(descriptor >= 0);

    audio_stream_io_handle_t handle = audio_stream_io_from_file(file.get());
    REQUIRE(handle != nullptr);
    file.release();
    audio_stream_io_close(handle);

    errno = 0;
    REQUIRE(fcntl(descriptor, F_GETFD) == -1);
    REQUIRE(errno == EBADF);
}

void test_file_stream_codec_identification(
    const std::filesystem::path &mp3_path,
    const std::filesystem::path &wav_path) {
    {
        Stream stream(open_file_stream(mp3_path));
        REQUIRE(is_mp3(stream.get()));
    }

    {
        Stream stream(open_file_stream(wav_path));
        wav_instance instance{};
        REQUIRE(is_wav(stream.get(), &instance));
        REQUIRE(instance.header.AudioFormat == 1);
        REQUIRE(instance.header.NumChannels == 1);
        REQUIRE(instance.header.SampleRate == 48000);
        REQUIRE(instance.header.BitsPerSample == 24);
    }
}

bool identifies_as_wav(const std::vector<uint8_t> &bytes) {
    Stream stream(audio_stream_io_from_memory(bytes.data(), bytes.size(), false));
    wav_instance instance{};
    return is_wav(stream.get(), &instance);
}

void test_wav_decode(bool include_unknown_chunk) {
    const std::vector<uint8_t> wav = make_wav(include_unknown_chunk);
    Stream stream(audio_stream_io_from_memory(wav.data(), wav.size(), false));

    wav_instance instance{};
    REQUIRE(is_wav(stream.get(), &instance));
    REQUIRE(instance.header.AudioFormat == 1);
    REQUIRE(instance.header.NumChannels == 1);
    REQUIRE(instance.header.SampleRate == 8000);
    REQUIRE(instance.header.BitsPerSample == 16);

    long data_position = -1;
    REQUIRE(audio_stream_io_tell(stream.get(), &data_position) == ESP_OK);
    REQUIRE(data_position == (include_unknown_chunk ? 56 : 44));

    std::array<int16_t, 4> output{};
    decode_data decoded{};
    decoded.samples = reinterpret_cast<uint8_t *>(output.data());
    decoded.samples_capacity = sizeof(output);
    decoded.samples_capacity_max = sizeof(output);

    REQUIRE(decode_wav(stream.get(), &decoded, &instance) == DECODE_STATUS_CONTINUE);
    REQUIRE(decoded.fmt.channels == 1);
    REQUIRE(decoded.fmt.sample_rate == 8000);
    REQUIRE(decoded.fmt.bits_per_sample == 16);
    REQUIRE(decoded.frame_count == output.size());
    REQUIRE((output == std::array<int16_t, 4>{-32768, -1, 0, 32767}));

    REQUIRE(decode_wav(stream.get(), &decoded, &instance) == DECODE_STATUS_DONE);
    REQUIRE(decoded.frame_count == 0);
}

void test_wav_rejects_invalid_headers() {
    std::vector<uint8_t> truncated = make_wav(false);
    truncated.resize(20);
    REQUIRE(!identifies_as_wav(truncated));

    std::vector<uint8_t> cross_field_riff = make_wav(false);
    cross_field_riff[0] = 'x';
    cross_field_riff[1] = 'x';
    cross_field_riff[2] = 'x';
    cross_field_riff[3] = 'R';
    cross_field_riff[4] = 'I';
    cross_field_riff[5] = 'F';
    cross_field_riff[6] = 'F';
    cross_field_riff[7] = 0;
    REQUIRE(!identifies_as_wav(cross_field_riff));

    std::vector<uint8_t> cross_field_wave = make_wav(false);
    cross_field_wave[8] = 'x';
    cross_field_wave[9] = 'x';
    cross_field_wave[10] = 'x';
    cross_field_wave[11] = 'W';
    cross_field_wave[12] = 'A';
    cross_field_wave[13] = 'V';
    cross_field_wave[14] = 'E';
    cross_field_wave[15] = 0;
    REQUIRE(!identifies_as_wav(cross_field_wave));
}

void test_wav_metadata_decode(const std::filesystem::path &fixture_path) {
    const std::vector<uint8_t> wav = read_binary_file(fixture_path);
    Stream stream(audio_stream_io_from_memory(wav.data(), wav.size(), false));

    wav_instance instance{};
    REQUIRE(is_wav(stream.get(), &instance));
    REQUIRE(instance.header.AudioFormat == 1);
    REQUIRE(instance.header.NumChannels == 1);
    REQUIRE(instance.header.SampleRate == 48000);
    REQUIRE(instance.header.BitsPerSample == 24);

    long data_position = -1;
    REQUIRE(audio_stream_io_tell(stream.get(), &data_position) == ESP_OK);
    REQUIRE(data_position >= 0);

    constexpr size_t kExpectedDataBytes = 1440;
    constexpr size_t kExpectedFrames = 480;
    std::array<uint8_t, 2048> output{};
    decode_data decoded{};
    decoded.samples = output.data();
    decoded.samples_capacity = output.size();
    decoded.samples_capacity_max = output.size();

    REQUIRE(decode_wav(stream.get(), &decoded, &instance) == DECODE_STATUS_CONTINUE);
    REQUIRE(decoded.fmt.channels == 1);
    REQUIRE(decoded.fmt.sample_rate == 48000);
    REQUIRE(decoded.fmt.bits_per_sample == 24);
    REQUIRE(decoded.frame_count == kExpectedFrames);

    long position_after_data = -1;
    REQUIRE(audio_stream_io_tell(stream.get(), &position_after_data) == ESP_OK);
    REQUIRE(position_after_data == data_position + static_cast<long>(kExpectedDataBytes));
    REQUIRE(static_cast<size_t>(position_after_data) < wav.size());

    REQUIRE(decode_wav(stream.get(), &decoded, &instance) == DECODE_STATUS_DONE);
    REQUIRE(decoded.frame_count == 0);

    long final_position = -1;
    REQUIRE(audio_stream_io_tell(stream.get(), &final_position) == ESP_OK);
    REQUIRE(final_position == position_after_data);
}

void test_wav_rejects_fixture(const std::filesystem::path &fixture_path) {
    REQUIRE(!identifies_as_wav(read_binary_file(fixture_path)));
}

void test_wav_truncated_payload_fails_safely(
    const std::filesystem::path &fixture_path) {
    const std::vector<uint8_t> wav = read_binary_file(fixture_path);
    Stream stream(audio_stream_io_from_memory(wav.data(), wav.size(), false));

    wav_instance instance{};
    REQUIRE(is_wav(stream.get(), &instance));
    REQUIRE(instance.header.AudioFormat == 1);
    REQUIRE(instance.header.NumChannels == 1);
    REQUIRE(instance.header.SampleRate == 11025);
    REQUIRE(instance.header.BitsPerSample == 8);

    std::array<uint8_t, 4096> output{};
    decode_data decoded{};
    decoded.samples = output.data();
    decoded.samples_capacity = output.size();
    decoded.samples_capacity_max = output.size();

    constexpr size_t kDecodeIterationLimit = 32;
    for (size_t iteration = 0; iteration < kDecodeIterationLimit; ++iteration) {
        decoded.frame_count = 0;
        const DECODE_STATUS status = decode_wav(stream.get(), &decoded, &instance);
        if (status == DECODE_STATUS_ERROR) {
            return;
        }

        REQUIRE(status != DECODE_STATUS_DONE);
    }

    throw std::runtime_error("truncated WAV decoding did not report an error");
}

void test_mp3_id3_tag_size() {
    mp3_id3_header_v2_t tag{};
    tag.size[0] = static_cast<char>(0xff);
    tag.size[1] = static_cast<char>(0xff);
    tag.size[2] = static_cast<char>(0xff);
    tag.size[3] = static_cast<char>(0xff);
    REQUIRE(mp3_id3v2_tag_size(&tag) == 0x0fffffff);

    tag.size[0] = 0;
    tag.size[1] = 0;
    tag.size[2] = 2;
    tag.size[3] = 1;
    REQUIRE(mp3_id3v2_tag_size(&tag) == 257);
}

void test_mp3_identification() {
    for (uint8_t second_byte : {uint8_t{0xfb}, uint8_t{0xf3}, uint8_t{0xf2}}) {
        const std::array<uint8_t, 3> signature = {0xff, second_byte, 0};
        Stream stream(audio_stream_io_from_memory(signature.data(), signature.size(), false));
        REQUIRE(is_mp3(stream.get()));

        long position = -1;
        REQUIRE(audio_stream_io_tell(stream.get(), &position) == ESP_OK);
        REQUIRE(position == 0);
    }

    std::vector<uint8_t> id3(14, 0);
    std::memcpy(id3.data(), "ID3", 3);
    id3[3] = 4;
    id3[9] = 4;
    Stream id3_stream(audio_stream_io_from_memory(id3.data(), id3.size(), false));
    REQUIRE(is_mp3(id3_stream.get()));

    long id3_position = -1;
    REQUIRE(audio_stream_io_tell(id3_stream.get(), &id3_position) == ESP_OK);
    REQUIRE(id3_position == 14);

    const std::array<uint8_t, 3> truncated_id3 = {'I', 'D', '3'};
    Stream truncated_stream(
        audio_stream_io_from_memory(truncated_id3.data(), truncated_id3.size(), false));
    REQUIRE(!is_mp3(truncated_stream.get()));

    const std::array<uint8_t, 4> junk = {1, 2, 3, 4};
    Stream junk_stream(audio_stream_io_from_memory(junk.data(), junk.size(), false));
    REQUIRE(!is_mp3(junk_stream.get()));
}

void test_mp3_decode(const std::string &fixture_path) {
    const std::vector<uint8_t> mp3 = read_binary_file(fixture_path);
    REQUIRE(!mp3.empty());

    Stream stream(audio_stream_io_from_memory(mp3.data(), mp3.size(), false));
    REQUIRE(is_mp3(stream.get()));

    Mp3Decoder decoder;
    std::vector<uint8_t> input_buffer(kMp3InputBufferSize);
    mp3_instance instance{};
    instance.data_buf = input_buffer.data();
    instance.data_buf_size = input_buffer.size();
    instance.read_ptr = input_buffer.data();

    std::array<int16_t, kMp3OutputSampleCount> pcm{};
    decode_data decoded{};
    decoded.samples = reinterpret_cast<uint8_t *>(pcm.data());
    decoded.samples_capacity = sizeof(pcm);
    decoded.samples_capacity_max = sizeof(pcm);

    constexpr size_t iteration_limit = 10000;
    size_t decoded_calls = 0;
    size_t total_frames = 0;
    bool reached_end = false;

    for (size_t iteration = 0; iteration < iteration_limit; ++iteration) {
        decoded.frame_count = 0;
        const DECODE_STATUS status =
            decode_mp3(decoder.get(), stream.get(), &decoded, &instance);
        REQUIRE(status != DECODE_STATUS_ERROR);

        if (decoded.frame_count > 0) {
            ++decoded_calls;
            total_frames += decoded.frame_count;
            REQUIRE(decoded.fmt.sample_rate == 44100);
            REQUIRE(decoded.fmt.bits_per_sample == 16);
            REQUIRE(decoded.fmt.channels == 1);
        }

        if (status == DECODE_STATUS_DONE) {
            reached_end = true;
            break;
        }
    }

    REQUIRE(reached_end);
    REQUIRE(decoded_calls > 0);
    REQUIRE(total_frames > 0);
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

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0]
                  << " <sample.mp3> <wav-fixture-directory>\n";
        return 2;
    }

    try {
        const std::filesystem::path wav_fixture_directory(argv[2]);
        const auto wav_fixture = [&](const char *name) {
            return wav_fixture_directory / name;
        };

        int failures = 0;
        run_test("FILE stream read, seek, tell, and EOF",
                 test_file_stream_contract, failures);
        run_test("owned FILE stream closes its file",
                 test_owned_file_stream_closes_file, failures);
        run_test("FILE streams identify real MP3 and WAV fixtures", [&] {
            test_file_stream_codec_identification(
                argv[1],
                wav_fixture("1khz_sine_48k_mono_region_marker.wav"));
        }, failures);
        run_test("canonical WAV decode", [] { test_wav_decode(false); }, failures);
        run_test("WAV unknown chunk", [] { test_wav_decode(true); }, failures);
        run_test("WAV invalid headers", test_wav_rejects_invalid_headers, failures);
        run_test("WAV metadata is not decoded as audio", [&] {
            test_wav_metadata_decode(
                wav_fixture("1khz_sine_48k_mono_region_marker.wav"));
        }, failures);
        run_test("WAV oversized data is rejected", [&] {
            test_wav_rejects_fixture(wav_fixture("bug1301226.wav"));
        }, failures);
        run_test("WAV truncated payload reports an error", [&] {
            test_wav_truncated_payload_fails_safely(
                wav_fixture("r11025_u8_c1_trunc.wav"));
        }, failures);
        run_test("WAV inconsistent header is rejected", [&] {
            test_wav_rejects_fixture(
                wav_fixture("test-8000Hz-le-3ch-5S-24bit-inconsistent.wav"));
        }, failures);
        run_test("MP3 ID3 tag size", test_mp3_id3_tag_size, failures);
        run_test("MP3 identification", test_mp3_identification, failures);
        run_test("MP3 decode", [&] { test_mp3_decode(argv[1]); }, failures);
        return failures == 0 ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "fatal test error: " << error.what() << '\n';
        return 1;
    }
}
