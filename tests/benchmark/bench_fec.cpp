#include <benchmark/benchmark.h>
#include "transport/fec.h"
#include <random>

using namespace omnidesk;

static void BM_FEC_Encode_1KB(benchmark::State& state) {
    FECEncoder enc;
    enc.setFECRatio(0.2f);

    std::vector<uint8_t> payload(1024);
    std::mt19937 rng(42);
    for (auto& b : payload) b = static_cast<uint8_t>(rng());

    for (auto _ : state) {
        auto packets = enc.encode(payload.data(), payload.size(), 0);
        benchmark::DoNotOptimize(packets);
    }
    state.SetBytesProcessed(state.iterations() * 1024);
}
BENCHMARK(BM_FEC_Encode_1KB);

static void BM_FEC_Encode_64KB(benchmark::State& state) {
    FECEncoder enc;
    enc.setFECRatio(0.2f);

    std::vector<uint8_t> payload(65536);
    std::mt19937 rng(42);
    for (auto& b : payload) b = static_cast<uint8_t>(rng());

    for (auto _ : state) {
        auto packets = enc.encode(payload.data(), payload.size(), 0);
        benchmark::DoNotOptimize(packets);
    }
    state.SetBytesProcessed(state.iterations() * 65536);
}
BENCHMARK(BM_FEC_Encode_64KB);

static void BM_FEC_Decode_WithRecovery(benchmark::State& state) {
    FECEncoder enc;
    enc.setFECRatio(0.25f);

    std::vector<uint8_t> payload(4096);
    std::mt19937 rng(42);
    for (auto& b : payload) b = static_cast<uint8_t>(rng());

    auto packets = enc.encode(payload.data(), payload.size(), 0);

    FECDecoder dec;
    for (auto _ : state) {
        dec.reset();
        // Feed all but one data packet, plus the FEC packet
        for (size_t i = 1; i < packets.size(); i++) {
            dec.addPacket(packets[i]);
        }
        auto result = dec.tryRecover();
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_FEC_Decode_WithRecovery);
