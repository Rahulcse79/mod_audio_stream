#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "base64.h"

#include "g711.h"

static void test_base64_roundtrip() {
    const std::string input = "hello world";
    const std::string enc = base64_encode(input);
    const std::string dec = base64_decode(enc);
    assert(dec == input);
}

static void test_base64_binary_roundtrip() {
    std::vector<unsigned char> data;
    for (int i = 0; i < 256; ++i) data.push_back((unsigned char)i);

    const std::string enc = base64_encode(data.data(), data.size());
    const std::string dec = base64_decode(enc);
    assert(dec.size() == data.size());
    assert(std::memcmp(dec.data(), data.data(), data.size()) == 0);
}

static void test_g711_encoders_not_constant() {
    // Encode a simple sine and ensure output varies (not all bytes identical)
    const int samples = 160; // 20ms @ 8k
    std::vector<int16_t> pcm(samples);
    for (int i = 0; i < samples; ++i) {
        double t = (double)i / 8000.0;
        double s = std::sin(2.0 * M_PI * 440.0 * t);
        pcm[i] = (int16_t)std::lround(s * 16000.0);
    }

    std::vector<uint8_t> alaw(samples), ulaw(samples);
    for (int i = 0; i < samples; ++i) {
        alaw[i] = mod_audio_stream_linear_to_alaw(pcm[i]);
        ulaw[i] = mod_audio_stream_linear_to_ulaw(pcm[i]);
    }

    bool any_diff_a = false;
    bool any_diff_u = false;
    for (int i = 1; i < samples; ++i) {
        if (alaw[i] != alaw[0]) any_diff_a = true;
        if (ulaw[i] != ulaw[0]) any_diff_u = true;
    }
    assert(any_diff_a);
    assert(any_diff_u);
}

static void test_pushback_frame_shapes() {
    // This matches the runtime pushback contract:
    // - inbound from WS is PCM16 @ 8kHz mono 20ms => 160 samples => 320 bytes
    // - if call is PCMA/PCMU, we must output 160 bytes encoded.
    const int rate = 8000;
    const int channels = 1;
    const int samples_per_20ms = rate / 50; // 160
    const size_t pcm_bytes = (size_t)samples_per_20ms * channels * sizeof(int16_t); // 320
    const size_t g711_bytes = (size_t)samples_per_20ms * channels; // 160

    // Build a deterministic ramp waveform.
    std::vector<int16_t> pcm(samples_per_20ms);
    for (int i = 0; i < samples_per_20ms; ++i) {
        pcm[i] = (int16_t)(i * 200 - 16000);
    }
    assert(pcm.size() * sizeof(int16_t) == pcm_bytes);

    std::vector<uint8_t> pcma(g711_bytes), pcmu(g711_bytes);
    for (int i = 0; i < samples_per_20ms; ++i) {
        pcma[i] = mod_audio_stream_linear_to_alaw(pcm[i]);
        pcmu[i] = mod_audio_stream_linear_to_ulaw(pcm[i]);
    }
    assert(pcma.size() == g711_bytes);
    assert(pcmu.size() == g711_bytes);

    // Make sure not all bytes are identical (a super basic sanity check)
    bool diff_a = false;
    bool diff_u = false;
    for (size_t i = 1; i < g711_bytes; ++i) {
        if (pcma[i] != pcma[0]) diff_a = true;
        if (pcmu[i] != pcmu[0]) diff_u = true;
    }
    assert(diff_a);
    assert(diff_u);
}

int main() {
    test_base64_roundtrip();
    test_base64_binary_roundtrip();
    test_g711_encoders_not_constant();
    test_pushback_frame_shapes();

    std::cout << "standalone tests: PASS\n";
    return 0;
}
