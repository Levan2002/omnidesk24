#include "transport/framer.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace omnidesk {

// ---- FramePacketizer ----

FramePacketizer::FramePacketizer() = default;
FramePacketizer::~FramePacketizer() = default;

void FramePacketizer::setFecRatio(float ratio) {
    fecEncoder_.setProtectionRatio(ratio);
}

std::vector<FramePacketizer::Fragment> FramePacketizer::packetize(
    const EncodedPacket& packet, uint16_t fecGroupId) {

    std::vector<Fragment> fragments;

    const uint8_t* srcData = packet.data.data();
    size_t srcLen = packet.data.size();

    // Calculate number of fragments needed
    uint16_t fragCount = static_cast<uint16_t>(
        (srcLen + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD);
    if (fragCount == 0) fragCount = 1;

    // Build video header flags
    uint8_t flags = 0;
    if (packet.isKeyFrame) flags |= FLAG_KEYFRAME;
    flags = setTemporalLayer(flags, packet.temporalLayer);
    if (!packet.dirtyRects.empty()) flags |= FLAG_HAS_DIRTY_RECTS;

    uint8_t rectCount = static_cast<uint8_t>(
        std::min(packet.dirtyRects.size(), static_cast<size_t>(255)));

    // Prepare FEC data packets
    std::vector<FecPacket> fecDataPackets;

    // Split data into fragments
    size_t offset = 0;
    for (uint16_t i = 0; i < fragCount; ++i) {
        size_t chunkLen = std::min(MAX_FRAGMENT_PAYLOAD, srcLen - offset);

        VideoHeader vh;
        vh.frameId = static_cast<uint32_t>(packet.frameId);
        vh.fragId = i;
        vh.fragCount = fragCount;
        vh.fecGroup = fecGroupId;
        vh.flags = flags;
        vh.rectCount = (i == 0) ? rectCount : 0;  // Only first fragment carries rect count

        Fragment frag;
        frag.fragId = i;
        frag.isParity = false;
        frag.data.resize(VideoHeader::SIZE + chunkLen);

        vh.serialize(frag.data.data());
        std::memcpy(frag.data.data() + VideoHeader::SIZE, srcData + offset, chunkLen);

        fragments.push_back(std::move(frag));

        // Also build FEC input
        FecPacket fp;
        fp.data.assign(srcData + offset, srcData + offset + chunkLen);
        fp.index = i;
        fp.groupId = fecGroupId;
        fp.isParity = false;
        fecDataPackets.push_back(std::move(fp));

        offset += chunkLen;
    }

    // Generate FEC parity fragments
    auto parityPackets = fecEncoder_.encode(fecDataPackets, fecGroupId);
    for (const auto& pp : parityPackets) {
        VideoHeader vh;
        vh.frameId = static_cast<uint32_t>(packet.frameId);
        vh.fragId = pp.index;  // Index beyond data fragment range
        vh.fragCount = fragCount;
        vh.fecGroup = fecGroupId;
        vh.flags = flags;
        vh.rectCount = 0;

        Fragment frag;
        frag.fragId = pp.index;
        frag.isParity = true;
        frag.data.resize(VideoHeader::SIZE + pp.data.size());

        vh.serialize(frag.data.data());
        std::memcpy(frag.data.data() + VideoHeader::SIZE, pp.data.data(), pp.data.size());

        fragments.push_back(std::move(frag));
    }

    return fragments;
}

// ---- FrameAssembler ----

FrameAssembler::FrameAssembler() = default;
FrameAssembler::~FrameAssembler() = default;

void FrameAssembler::setFrameCallback(FrameCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    frameCallback_ = std::move(callback);
}

bool FrameAssembler::addFragment(const uint8_t* data, size_t length) {
    if (length < VideoHeader::SIZE) return false;

    VideoHeader vh = VideoHeader::deserialize(data);

    // Discard fragments for already-completed old frames
    if (vh.frameId <= lastCompletedFrameId_ && lastCompletedFrameId_ > 0) {
        return false;
    }

    const uint8_t* payload = data + VideoHeader::SIZE;
    size_t payloadLen = length - VideoHeader::SIZE;

    std::lock_guard<std::mutex> lock(mutex_);

    auto& frame = pendingFrames_[vh.frameId];
    if (frame.frameId == 0) {
        // New frame entry
        frame.frameId = vh.frameId;
        frame.fragCount = vh.fragCount;
        frame.fecGroup = vh.fecGroup;
        frame.flags = vh.flags;
        frame.rectCount = vh.rectCount;
    }

    // Store fragment payload (only data fragments, not parity for now)
    if (vh.fragId < vh.fragCount) {
        frame.fragments[vh.fragId] = std::vector<uint8_t>(payload, payload + payloadLen);
    }

    // Update rect count from first fragment
    if (vh.fragId == 0 && vh.rectCount > 0) {
        frame.rectCount = vh.rectCount;
    }

    // Try to assemble
    if (tryAssemble(frame)) {
        return true;
    }

    return true;
}

bool FrameAssembler::tryAssemble(PartialFrame& frame) {
    if (frame.complete) return true;

    // Check if all data fragments are present
    if (frame.fragments.size() < frame.fragCount) return false;

    for (uint16_t i = 0; i < frame.fragCount; ++i) {
        if (frame.fragments.find(i) == frame.fragments.end()) return false;
    }

    // All fragments present - reassemble
    EncodedPacket packet;
    packet.frameId = frame.frameId;
    packet.isKeyFrame = (frame.flags & FLAG_KEYFRAME) != 0;
    packet.temporalLayer = getTemporalLayer(frame.flags);

    // Concatenate fragment payloads
    size_t totalSize = 0;
    for (uint16_t i = 0; i < frame.fragCount; ++i) {
        totalSize += frame.fragments[i].size();
    }
    packet.data.resize(totalSize);

    size_t offset = 0;
    for (uint16_t i = 0; i < frame.fragCount; ++i) {
        const auto& frag = frame.fragments[i];
        std::memcpy(packet.data.data() + offset, frag.data(), frag.size());
        offset += frag.size();
    }

    frame.complete = true;
    if (frame.frameId > lastCompletedFrameId_) {
        lastCompletedFrameId_ = frame.frameId;
    }

    // Invoke callback
    if (frameCallback_) {
        frameCallback_(packet);
    }

    return true;
}

SelectiveAck FrameAssembler::getSelectiveAck(uint32_t frameId) const {
    std::lock_guard<std::mutex> lock(mutex_);

    SelectiveAck sack;
    auto it = pendingFrames_.find(frameId);
    if (it == pendingFrames_.end()) return sack;

    const auto& frame = it->second;
    sack.frameId = frameId;
    sack.fragCount = frame.fragCount;
    sack.bitmap.resize((frame.fragCount + 7) / 8, 0);

    for (const auto& [fragId, _] : frame.fragments) {
        sack.setReceived(fragId);
    }

    return sack;
}

std::vector<uint16_t> FrameAssembler::getMissingFragments(uint32_t frameId) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<uint16_t> missing;
    auto it = pendingFrames_.find(frameId);
    if (it == pendingFrames_.end()) return missing;

    const auto& frame = it->second;
    for (uint16_t i = 0; i < frame.fragCount; ++i) {
        if (frame.fragments.find(i) == frame.fragments.end()) {
            missing.push_back(i);
        }
    }
    return missing;
}

bool FrameAssembler::isFrameComplete(uint32_t frameId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pendingFrames_.find(frameId);
    if (it == pendingFrames_.end()) return false;
    return it->second.complete;
}

void FrameAssembler::purgeOldFrames(uint32_t olderThan) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pendingFrames_.begin();
    while (it != pendingFrames_.end()) {
        if (it->first < olderThan) {
            it = pendingFrames_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace omnidesk
