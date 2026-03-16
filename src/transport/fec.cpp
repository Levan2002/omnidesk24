#include "transport/fec.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace omnidesk {

// ---- FecEncoder ----

FecEncoder::FecEncoder() = default;
FecEncoder::~FecEncoder() = default;

void FecEncoder::setProtectionRatio(float ratio) {
    if (ratio <= 0.0f) {
        ratio_ = 0.0f;
    } else {
        ratio_ = std::clamp(ratio, minRatio_, maxRatio_);
    }
}

void FecEncoder::adaptToLossRate(float lossRate) {
    // Scale protection ratio based on observed loss.
    // At 0% loss: minimal protection (minRatio_)
    // At 5% loss: ~20% protection
    // At 10%+ loss: max protection
    if (lossRate <= 0.001f) {
        ratio_ = minRatio_;
    } else if (lossRate < 0.10f) {
        ratio_ = std::clamp(lossRate * 2.0f + 0.05f, minRatio_, maxRatio_);
    } else {
        ratio_ = maxRatio_;
    }
}

std::vector<FecPacket> FecEncoder::encode(const std::vector<FecPacket>& dataPackets,
                                           uint16_t groupId) {
    std::vector<FecPacket> parityPackets;
    size_t k = dataPackets.size();
    if (k == 0) return parityPackets;

    // Calculate number of parity packets
    size_t numParity = static_cast<size_t>(std::ceil(static_cast<float>(k) * ratio_));
    if (numParity == 0 && ratio_ > 0.0f && k > 0) {
        numParity = 1;  // At least 1 parity if ratio > 0
    }
    if (numParity == 0) return parityPackets;

    // Find max packet length for padding
    size_t maxLen = 0;
    for (const auto& pkt : dataPackets) {
        maxLen = std::max(maxLen, pkt.data.size());
    }
    if (maxLen == 0) return parityPackets;

    // Generate parity packets using interleaved XOR groups.
    // Each parity packet XORs a subset of the data packets.
    // For simple case (1 parity), XOR all packets together.
    // For multiple parity packets, use interleaved grouping.
    for (size_t p = 0; p < numParity; ++p) {
        std::vector<const uint8_t*> group;
        std::vector<std::vector<uint8_t>> padded;

        // Each parity packet p covers data packets where (index % numParity == p)
        for (size_t i = p; i < k; i += numParity) {
            // Pad packet to maxLen
            padded.emplace_back(maxLen, 0);
            std::memcpy(padded.back().data(), dataPackets[i].data.data(),
                        dataPackets[i].data.size());
            group.push_back(padded.back().data());
        }

        if (group.empty()) continue;

        FecPacket parity;
        parity.data = xorPackets(group, maxLen);
        parity.index = static_cast<uint16_t>(k + p);
        parity.groupId = groupId;
        parity.isParity = true;
        parityPackets.push_back(std::move(parity));
    }

    return parityPackets;
}

void FecEncoder::reset() {
    ratio_ = 0.1f;
}

std::vector<uint8_t> FecEncoder::xorPackets(const std::vector<const uint8_t*>& packets,
                                             size_t packetLen) const {
    std::vector<uint8_t> result(packetLen, 0);

    for (const auto* pkt : packets) {
        // XOR each byte
        for (size_t i = 0; i < packetLen; ++i) {
            result[i] ^= pkt[i];
        }
    }

    return result;
}

// ---- FecDecoder ----

FecDecoder::FecDecoder() = default;
FecDecoder::~FecDecoder() = default;

void FecDecoder::setExpectedCount(uint16_t dataCount, uint16_t parityCount) {
    group_.expectedDataCount = dataCount;
    group_.expectedParityCount = parityCount;
    group_.dataPackets.resize(dataCount);
    group_.parityPackets.resize(parityCount);
    group_.dataReceived.resize(dataCount, false);
    group_.parityReceived.resize(parityCount, false);
    group_.recovered = false;
}

void FecDecoder::addPacket(const FecPacket& packet) {
    if (packet.isParity) {
        uint16_t idx = packet.index - group_.expectedDataCount;
        if (idx < group_.parityPackets.size()) {
            group_.parityPackets[idx] = packet;
            group_.parityReceived[idx] = true;
            maxPacketLen_ = std::max(maxPacketLen_, packet.data.size());
        }
    } else {
        if (packet.index < group_.dataPackets.size()) {
            group_.dataPackets[packet.index] = packet;
            group_.dataReceived[packet.index] = true;
            maxPacketLen_ = std::max(maxPacketLen_, packet.data.size());
        }
    }
}

bool FecDecoder::tryRecover() {
    if (group_.recovered) return true;

    // Check if all data packets are already received
    bool allReceived = true;
    for (size_t i = 0; i < group_.expectedDataCount; ++i) {
        if (!group_.dataReceived[i]) {
            allReceived = false;
            break;
        }
    }
    if (allReceived) {
        group_.recovered = true;
        return true;
    }

    // XOR-based recovery: for each parity group, if exactly one data packet
    // is missing, we can recover it by XORing the parity with all other
    // data packets in the group.
    uint16_t numParity = group_.expectedParityCount;
    if (numParity == 0) return false;

    bool anyRecovered = true;
    while (anyRecovered) {
        anyRecovered = false;

        for (uint16_t p = 0; p < numParity; ++p) {
            if (!group_.parityReceived[p]) continue;

            // Find which data packets belong to this parity group
            std::vector<uint16_t> groupIndices;
            for (uint16_t i = p; i < group_.expectedDataCount; i += numParity) {
                groupIndices.push_back(i);
            }

            // Count missing packets in this group
            uint16_t missingIdx = 0;
            int missingCount = 0;
            for (uint16_t idx : groupIndices) {
                if (!group_.dataReceived[idx]) {
                    missingIdx = idx;
                    missingCount++;
                }
            }

            // Can only recover if exactly one is missing
            if (missingCount != 1) continue;

            // Recover: XOR parity with all received data packets
            std::vector<uint8_t> recovered(maxPacketLen_, 0);

            // Start with parity data
            const auto& parityData = group_.parityPackets[p].data;
            for (size_t i = 0; i < parityData.size() && i < maxPacketLen_; ++i) {
                recovered[i] = parityData[i];
            }

            // XOR with all received packets in this group
            for (uint16_t idx : groupIndices) {
                if (idx == missingIdx) continue;
                const auto& d = group_.dataPackets[idx].data;
                for (size_t i = 0; i < d.size() && i < maxPacketLen_; ++i) {
                    recovered[i] ^= d[i];
                }
            }

            // Store recovered packet
            FecPacket pkt;
            pkt.data = std::move(recovered);
            pkt.index = missingIdx;
            pkt.groupId = group_.parityPackets[p].groupId;
            pkt.isParity = false;
            group_.dataPackets[missingIdx] = std::move(pkt);
            group_.dataReceived[missingIdx] = true;
            anyRecovered = true;
        }
    }

    // Check if all data is now available
    for (size_t i = 0; i < group_.expectedDataCount; ++i) {
        if (!group_.dataReceived[i]) return false;
    }

    group_.recovered = true;
    return true;
}

std::vector<FecPacket> FecDecoder::getDataPackets() const {
    std::vector<FecPacket> result;
    for (size_t i = 0; i < group_.expectedDataCount; ++i) {
        if (group_.dataReceived[i]) {
            result.push_back(group_.dataPackets[i]);
        }
    }
    return result;
}

bool FecDecoder::hasPacket(uint16_t index) const {
    if (index >= group_.dataReceived.size()) return false;
    return group_.dataReceived[index];
}

uint16_t FecDecoder::receivedDataCount() const {
    uint16_t count = 0;
    for (bool b : group_.dataReceived) {
        if (b) ++count;
    }
    return count;
}

uint16_t FecDecoder::receivedParityCount() const {
    uint16_t count = 0;
    for (bool b : group_.parityReceived) {
        if (b) ++count;
    }
    return count;
}

bool FecDecoder::isComplete() const {
    if (group_.recovered) return true;

    // Also complete if all data packets have been received
    if (group_.expectedDataCount == 0) return false;
    for (size_t i = 0; i < group_.expectedDataCount; ++i) {
        if (!group_.dataReceived[i]) return false;
    }
    return true;
}

void FecDecoder::reset() {
    group_ = {};
    maxPacketLen_ = 0;
}

} // namespace omnidesk
