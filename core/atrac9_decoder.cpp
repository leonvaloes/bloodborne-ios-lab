#include "atrac9_decoder.h"
#include "libatrac9.h"
#include <cstring>
#include <cmath>
#include <algorithm>

namespace bb {

Atrac9Decoder::Atrac9Decoder() {
    handle_ = Atrac9GetHandle();
}

Atrac9Decoder::~Atrac9Decoder() {
    if (handle_) {
        Atrac9ReleaseHandle(handle_);
        handle_ = nullptr;
    }
}

Atrac9Decoder::Atrac9Decoder(Atrac9Decoder &&other) noexcept
    : handle_(other.handle_), initialized_(other.initialized_), info_(other.info_) {
    other.handle_ = nullptr;
    other.initialized_ = false;
}

Atrac9Decoder &Atrac9Decoder::operator=(Atrac9Decoder &&other) noexcept {
    if (this != &other) {
        if (handle_) {
            Atrac9ReleaseHandle(handle_);
        }
        handle_ = other.handle_;
        initialized_ = other.initialized_;
        info_ = other.info_;
        other.handle_ = nullptr;
        other.initialized_ = false;
    }
    return *this;
}

std::array<std::uint8_t, 4> Atrac9Decoder::make_config_data(int sample_rate_index, int channel_config_index,
                                                            int frame_bytes, int superframe_index) {
    std::array<std::uint8_t, 4> cfg{};
    cfg[0] = 0xFE;
    cfg[1] = static_cast<std::uint8_t>(((sample_rate_index & 0x0F) << 4) |
                                       ((channel_config_index & 0x07) << 1) | 0);
    int fb = (frame_bytes - 1) & 0x7FF;
    cfg[2] = static_cast<std::uint8_t>((fb >> 3) & 0xFF);
    cfg[3] = static_cast<std::uint8_t>(((fb & 0x07) << 5) | ((superframe_index & 0x03) << 3));
    return cfg;
}

bool Atrac9Decoder::init(const std::uint8_t config_data[4]) {
    if (!handle_ || !config_data) return false;

    unsigned char cfg[4];
    std::memcpy(cfg, config_data, 4);

    int res = Atrac9InitDecoder(handle_, cfg);
    if (res != 0) {
        initialized_ = false;
        return false;
    }

    Atrac9CodecInfo codec_info{};
    if (Atrac9GetCodecInfo(handle_, &codec_info) != 0) {
        initialized_ = false;
        return false;
    }

    info_.channels = codec_info.channels;
    info_.channel_config_index = codec_info.channelConfigIndex;
    info_.sample_rate = codec_info.samplingRate;
    info_.superframe_size = codec_info.superframeSize;
    info_.frames_in_superframe = codec_info.framesInSuperframe;
    info_.frame_samples = codec_info.frameSamples;
    info_.wlength = codec_info.wlength;
    std::memcpy(info_.config_data.data(), config_data, 4);

    initialized_ = true;
    return true;
}

bool Atrac9Decoder::init_from_riff(const std::uint8_t *riff_data, std::size_t size) {
    if (!riff_data || size < 44) return false;

    // Verify RIFF and WAVE magic
    if (std::memcmp(riff_data, "RIFF", 4) != 0 || std::memcmp(riff_data + 8, "WAVE", 4) != 0) {
        return false;
    }

    std::size_t offset = 12;
    while (offset + 8 <= size) {
        const std::uint8_t *chunk_tag = riff_data + offset;
        std::uint32_t chunk_len = 0;
        std::memcpy(&chunk_len, riff_data + offset + 4, 4);
        offset += 8;

        if (std::memcmp(chunk_tag, "fmt ", 4) == 0) {
            // In ATRAC9 RIFF WAVE files, the 4-byte configData is located at offset 44 of the fmt chunk
            if (chunk_len >= 48 && (offset + 48) <= size) {
                return init(riff_data + offset + 44);
            }
            return false;
        }

        offset += chunk_len;
        // Chunks are 2-byte aligned in RIFF
        if (chunk_len % 2 != 0) ++offset;
    }

    return false;
}

int Atrac9Decoder::decode_frame(const std::uint8_t *atrac9_buf, std::size_t size, std::int16_t *pcm_out, int &bytes_used) {
    if (!initialized_ || !handle_ || !atrac9_buf || !pcm_out || size == 0) {
        bytes_used = 0;
        return -1;
    }

    int used = 0;
    int res = Atrac9Decode(handle_, atrac9_buf, reinterpret_cast<short*>(pcm_out), &used);
    bytes_used = used;
    return res;
}

std::size_t Atrac9Decoder::decode_stream_to_stereo_48k(const std::uint8_t *atrac9_stream, std::size_t size,
                                                       std::vector<std::int16_t> &out_pcm_stereo) {
    if (!initialized_ || !handle_ || !atrac9_stream || size == 0) return 0;

    std::size_t offset = 0;
    std::size_t frame_samples = static_cast<std::size_t>(info_.frame_samples);
    std::size_t channels = static_cast<std::size_t>(info_.channels);
    std::vector<std::int16_t> temp_pcm(frame_samples * channels);

    std::vector<std::int16_t> decoded_pcm_stereo;
    decoded_pcm_stereo.reserve((size / std::max(1, info_.superframe_size)) * frame_samples * 2);

    while (offset < size) {
        int bytes_used = 0;
        int res = decode_frame(atrac9_stream + offset, size - offset, temp_pcm.data(), bytes_used);
        if (res != 0 || bytes_used <= 0) {
            break;
        }
        offset += static_cast<std::size_t>(bytes_used);

        // Convert channels to Stereo (2 channels)
        if (channels == 1) {
            for (std::size_t s = 0; s < frame_samples; ++s) {
                std::int16_t sample = temp_pcm[s];
                decoded_pcm_stereo.push_back(sample);
                decoded_pcm_stereo.push_back(sample);
            }
        } else if (channels == 2) {
            decoded_pcm_stereo.insert(decoded_pcm_stereo.end(), temp_pcm.begin(), temp_pcm.end());
        } else {
            // Downmix surround to stereo (using standard ITU downmix coefficients)
            for (std::size_t s = 0; s < frame_samples; ++s) {
                float left = 0.0f;
                float right = 0.0f;
                float fl = static_cast<float>(temp_pcm[s * channels + 0]);
                float fr = static_cast<float>(temp_pcm[s * channels + 1]);
                float center = (channels >= 3) ? static_cast<float>(temp_pcm[s * channels + 2]) : 0.0f;
                left = fl + 0.7071f * center;
                right = fr + 0.7071f * center;
                if (channels >= 5) {
                    left += 0.7071f * static_cast<float>(temp_pcm[s * channels + 4]);
                    right += 0.7071f * static_cast<float>(temp_pcm[s * channels + 5]);
                }
                decoded_pcm_stereo.push_back(static_cast<std::int16_t>(std::clamp(left, -32768.0f, 32767.0f)));
                decoded_pcm_stereo.push_back(static_cast<std::int16_t>(std::clamp(right, -32768.0f, 32767.0f)));
            }
        }
    }

    if (decoded_pcm_stereo.empty()) return 0;

    // Resample to 48000 Hz if necessary
    if (info_.sample_rate != 48000 && info_.sample_rate > 0) {
        std::size_t in_frames = decoded_pcm_stereo.size() / 2;
        double ratio = 48000.0 / static_cast<double>(info_.sample_rate);
        std::size_t out_frames = static_cast<std::size_t>(std::round(static_cast<double>(in_frames) * ratio));
        out_pcm_stereo.resize(out_frames * 2);

        for (std::size_t i = 0; i < out_frames; ++i) {
            double src_pos = static_cast<double>(i) / ratio;
            std::size_t idx0 = static_cast<std::size_t>(src_pos);
            std::size_t idx1 = std::min(idx0 + 1, in_frames - 1);
            double frac = src_pos - static_cast<double>(idx0);

            for (std::size_t c = 0; c < 2; ++c) {
                double s0 = static_cast<double>(decoded_pcm_stereo[idx0 * 2 + c]);
                double s1 = static_cast<double>(decoded_pcm_stereo[idx1 * 2 + c]);
                double sample = s0 + frac * (s1 - s0);
                out_pcm_stereo[i * 2 + c] = static_cast<std::int16_t>(std::clamp(sample, -32768.0, 32767.0));
            }
        }
        return out_frames;
    }

    out_pcm_stereo = std::move(decoded_pcm_stereo);
    return out_pcm_stereo.size() / 2;
}

std::size_t Atrac9Decoder::decode_to_ring_buffer(const std::uint8_t *atrac9_stream, std::size_t size,
                                                 AudioRingBuffer &ring_buffer) {
    std::vector<std::int16_t> stereo_pcm;
    std::size_t frames = decode_stream_to_stereo_48k(atrac9_stream, size, stereo_pcm);
    if (frames == 0) return 0;

    return ring_buffer.write(stereo_pcm.data(), frames);
}

} // namespace bb
