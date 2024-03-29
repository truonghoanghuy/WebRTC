/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "modules/video_coding/packet_buffer.h"

#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "common_video/h264/h264_common.h"
#include "modules/video_coding/frame_object.h"
#include "rtc_base/random.h"
#include "system_wrappers/include/clock.h"
#include "test/field_trial.h"
#include "test/gtest.h"

namespace webrtc {
namespace video_coding {

class PacketBufferTest : public ::testing::Test,
                         public OnAssembledFrameCallback {
 protected:
  explicit PacketBufferTest(std::string field_trials = "")
      : scoped_field_trials_(field_trials),
        rand_(0x7732213),
        clock_(new SimulatedClock(0)),
        packet_buffer_(clock_.get(), kStartSize, kMaxSize, this) {}

  uint16_t Rand() { return rand_.Rand<uint16_t>(); }

  void OnAssembledFrame(std::unique_ptr<RtpFrameObject> frame) override {
    uint16_t first_seq_num = frame->first_seq_num();
    if (frames_from_callback_.find(first_seq_num) !=
        frames_from_callback_.end()) {
      ADD_FAILURE() << "Already received frame with first sequence number "
                    << first_seq_num << ".";
      return;
    }

    frames_from_callback_.insert(
        std::make_pair(frame->first_seq_num(), std::move(frame)));
  }

  enum IsKeyFrame { kKeyFrame, kDeltaFrame };
  enum IsFirst { kFirst, kNotFirst };
  enum IsLast { kLast, kNotLast };

  bool Insert(uint16_t seq_num,             // packet sequence number
              IsKeyFrame keyframe,          // is keyframe
              IsFirst first,                // is first packet of frame
              IsLast last,                  // is last packet of frame
              int data_size = 0,            // size of data
              uint8_t* data = nullptr,      // data pointer
              uint32_t timestamp = 123u) {  // rtp timestamp
    VCMPacket packet;
    packet.video_header.codec = kVideoCodecGeneric;
    packet.timestamp = timestamp;
    packet.seqNum = seq_num;
    packet.video_header.frame_type = keyframe == kKeyFrame
                                         ? VideoFrameType::kVideoFrameKey
                                         : VideoFrameType::kVideoFrameDelta;
    packet.video_header.is_first_packet_in_frame = first == kFirst;
    packet.video_header.is_last_packet_in_frame = last == kLast;
    packet.sizeBytes = data_size;
    packet.dataPtr = data;

    return packet_buffer_.InsertPacket(&packet);
  }

  void CheckFrame(uint16_t first_seq_num) {
    auto frame_it = frames_from_callback_.find(first_seq_num);
    ASSERT_FALSE(frame_it == frames_from_callback_.end())
        << "Could not find frame with first sequence number " << first_seq_num
        << ".";
  }

  void DeleteFrame(uint16_t first_seq_num) {
    auto frame_it = frames_from_callback_.find(first_seq_num);
    ASSERT_FALSE(frame_it == frames_from_callback_.end())
        << "Could not find frame with first sequence number " << first_seq_num
        << ".";
    frames_from_callback_.erase(frame_it);
  }

  static constexpr int kStartSize = 16;
  static constexpr int kMaxSize = 64;

  const test::ScopedFieldTrials scoped_field_trials_;

  Random rand_;
  std::unique_ptr<SimulatedClock> clock_;
  PacketBuffer packet_buffer_;
  std::map<uint16_t, std::unique_ptr<RtpFrameObject>> frames_from_callback_;
};

TEST_F(PacketBufferTest, InsertOnePacket) {
  const uint16_t seq_num = Rand();
  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kLast));
}

TEST_F(PacketBufferTest, InsertMultiplePackets) {
  const uint16_t seq_num = Rand();
  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(seq_num + 1, kKeyFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(seq_num + 2, kKeyFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(seq_num + 3, kKeyFrame, kFirst, kLast));
}

TEST_F(PacketBufferTest, InsertDuplicatePacket) {
  const uint16_t seq_num = Rand();
  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + 1, kKeyFrame, kNotFirst, kLast));
}

TEST_F(PacketBufferTest, SeqNumWrapOneFrame) {
  EXPECT_TRUE(Insert(0xFFFF, kKeyFrame, kFirst, kNotLast));
  EXPECT_TRUE(Insert(0x0, kKeyFrame, kNotFirst, kLast));

  CheckFrame(0xFFFF);
}

TEST_F(PacketBufferTest, SeqNumWrapTwoFrames) {
  EXPECT_TRUE(Insert(0xFFFF, kKeyFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(0x0, kKeyFrame, kFirst, kLast));

  CheckFrame(0xFFFF);
  CheckFrame(0x0);
}

TEST_F(PacketBufferTest, InsertOldPackets) {
  const uint16_t seq_num = Rand();

  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + 2, kDeltaFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(seq_num + 1, kKeyFrame, kNotFirst, kLast));
  ASSERT_EQ(2UL, frames_from_callback_.size());

  frames_from_callback_.erase(seq_num + 2);
  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast));
  ASSERT_EQ(1UL, frames_from_callback_.size());

  frames_from_callback_.erase(frames_from_callback_.find(seq_num));
  ASSERT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + 2, kDeltaFrame, kFirst, kLast));

  packet_buffer_.ClearTo(seq_num + 2);
  EXPECT_TRUE(Insert(seq_num + 2, kDeltaFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(seq_num + 3, kDeltaFrame, kFirst, kLast));
  ASSERT_EQ(2UL, frames_from_callback_.size());
}

TEST_F(PacketBufferTest, NackCount) {
  const uint16_t seq_num = Rand();

  VCMPacket packet;
  packet.video_header.codec = kVideoCodecGeneric;
  packet.seqNum = seq_num;
  packet.video_header.frame_type = VideoFrameType::kVideoFrameKey;
  packet.video_header.is_first_packet_in_frame = true;
  packet.video_header.is_last_packet_in_frame = false;
  packet.timesNacked = 0;

  packet_buffer_.InsertPacket(&packet);

  packet.seqNum++;
  packet.video_header.is_first_packet_in_frame = false;
  packet.timesNacked = 1;
  packet_buffer_.InsertPacket(&packet);

  packet.seqNum++;
  packet.timesNacked = 3;
  packet_buffer_.InsertPacket(&packet);

  packet.seqNum++;
  packet.video_header.is_last_packet_in_frame = true;
  packet.timesNacked = 1;
  packet_buffer_.InsertPacket(&packet);

  ASSERT_EQ(1UL, frames_from_callback_.size());
  RtpFrameObject* frame = frames_from_callback_.begin()->second.get();
  EXPECT_EQ(3, frame->times_nacked());
}

TEST_F(PacketBufferTest, FrameSize) {
  const uint16_t seq_num = Rand();
  uint8_t* data1 = new uint8_t[5]();
  uint8_t* data2 = new uint8_t[5]();
  uint8_t* data3 = new uint8_t[5]();
  uint8_t* data4 = new uint8_t[5]();

  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast, 5, data1));
  EXPECT_TRUE(Insert(seq_num + 1, kKeyFrame, kNotFirst, kNotLast, 5, data2));
  EXPECT_TRUE(Insert(seq_num + 2, kKeyFrame, kNotFirst, kNotLast, 5, data3));
  EXPECT_TRUE(Insert(seq_num + 3, kKeyFrame, kNotFirst, kLast, 5, data4));

  ASSERT_EQ(1UL, frames_from_callback_.size());
  EXPECT_EQ(20UL, frames_from_callback_.begin()->second->size());
}

TEST_F(PacketBufferTest, CountsUniqueFrames) {
  const uint16_t seq_num = Rand();

  ASSERT_EQ(0, packet_buffer_.GetUniqueFramesSeen());

  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast, 0, nullptr, 100));
  ASSERT_EQ(1, packet_buffer_.GetUniqueFramesSeen());
  // Still the same frame.
  EXPECT_TRUE(
      Insert(seq_num + 1, kKeyFrame, kNotFirst, kLast, 0, nullptr, 100));
  ASSERT_EQ(1, packet_buffer_.GetUniqueFramesSeen());

  // Second frame.
  EXPECT_TRUE(
      Insert(seq_num + 2, kKeyFrame, kFirst, kNotLast, 0, nullptr, 200));
  ASSERT_EQ(2, packet_buffer_.GetUniqueFramesSeen());
  EXPECT_TRUE(
      Insert(seq_num + 3, kKeyFrame, kNotFirst, kLast, 0, nullptr, 200));
  ASSERT_EQ(2, packet_buffer_.GetUniqueFramesSeen());

  // Old packet.
  EXPECT_TRUE(
      Insert(seq_num + 1, kKeyFrame, kNotFirst, kLast, 0, nullptr, 100));
  ASSERT_EQ(2, packet_buffer_.GetUniqueFramesSeen());

  // Missing middle packet.
  EXPECT_TRUE(
      Insert(seq_num + 4, kKeyFrame, kFirst, kNotLast, 0, nullptr, 300));
  EXPECT_TRUE(
      Insert(seq_num + 6, kKeyFrame, kNotFirst, kLast, 0, nullptr, 300));
  ASSERT_EQ(3, packet_buffer_.GetUniqueFramesSeen());
}

TEST_F(PacketBufferTest, HasHistoryOfUniqueFrames) {
  const int kNumFrames = 1500;
  const int kRequiredHistoryLength = 1000;
  const uint16_t seq_num = Rand();
  const uint32_t timestamp = 0xFFFFFFF0;  // Large enough to cause wrap-around.

  for (int i = 0; i < kNumFrames; ++i) {
    Insert(seq_num + i, kKeyFrame, kFirst, kNotLast, 0, nullptr,
           timestamp + 10 * i);
  }
  ASSERT_EQ(kNumFrames, packet_buffer_.GetUniqueFramesSeen());

  // Old packets within history should not affect number of seen unique frames.
  for (int i = kNumFrames - kRequiredHistoryLength; i < kNumFrames; ++i) {
    Insert(seq_num + i, kKeyFrame, kFirst, kNotLast, 0, nullptr,
           timestamp + 10 * i);
  }
  ASSERT_EQ(kNumFrames, packet_buffer_.GetUniqueFramesSeen());

  // Very old packets should be treated as unique.
  Insert(seq_num, kKeyFrame, kFirst, kNotLast, 0, nullptr, timestamp);
  ASSERT_EQ(kNumFrames + 1, packet_buffer_.GetUniqueFramesSeen());
}

TEST_F(PacketBufferTest, ExpandBuffer) {
  const uint16_t seq_num = Rand();

  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast));
  for (int i = 1; i < kStartSize; ++i)
    EXPECT_TRUE(Insert(seq_num + i, kKeyFrame, kNotFirst, kNotLast));

  // Already inserted kStartSize number of packets, inserting the last packet
  // should increase the buffer size and also result in an assembled frame.
  EXPECT_TRUE(Insert(seq_num + kStartSize, kKeyFrame, kNotFirst, kLast));
}

TEST_F(PacketBufferTest, SingleFrameExpandsBuffer) {
  const uint16_t seq_num = Rand();

  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast));
  for (int i = 1; i < kStartSize; ++i)
    EXPECT_TRUE(Insert(seq_num + i, kKeyFrame, kNotFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + kStartSize, kKeyFrame, kNotFirst, kLast));

  ASSERT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(seq_num);
}

TEST_F(PacketBufferTest, ExpandBufferOverflow) {
  const uint16_t seq_num = Rand();

  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast));
  for (int i = 1; i < kMaxSize; ++i)
    EXPECT_TRUE(Insert(seq_num + i, kKeyFrame, kNotFirst, kNotLast));

  // Already inserted kMaxSize number of packets, inserting the last packet
  // should overflow the buffer and result in false being returned.
  EXPECT_FALSE(Insert(seq_num + kMaxSize, kKeyFrame, kNotFirst, kLast));
}

TEST_F(PacketBufferTest, OnePacketOneFrame) {
  const uint16_t seq_num = Rand();
  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kLast));
  ASSERT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(seq_num);
}

TEST_F(PacketBufferTest, TwoPacketsTwoFrames) {
  const uint16_t seq_num = Rand();

  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(seq_num + 1, kKeyFrame, kFirst, kLast));

  EXPECT_EQ(2UL, frames_from_callback_.size());
  CheckFrame(seq_num);
  CheckFrame(seq_num + 1);
}

TEST_F(PacketBufferTest, TwoPacketsOneFrames) {
  const uint16_t seq_num = Rand();

  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + 1, kKeyFrame, kNotFirst, kLast));

  EXPECT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(seq_num);
}

TEST_F(PacketBufferTest, ThreePacketReorderingOneFrame) {
  const uint16_t seq_num = Rand();

  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + 2, kKeyFrame, kNotFirst, kLast));
  EXPECT_TRUE(Insert(seq_num + 1, kKeyFrame, kNotFirst, kNotLast));

  EXPECT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(seq_num);
}

TEST_F(PacketBufferTest, Frames) {
  const uint16_t seq_num = Rand();

  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(seq_num + 1, kDeltaFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(seq_num + 2, kDeltaFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(seq_num + 3, kDeltaFrame, kFirst, kLast));

  ASSERT_EQ(4UL, frames_from_callback_.size());
  CheckFrame(seq_num);
  CheckFrame(seq_num + 1);
  CheckFrame(seq_num + 2);
  CheckFrame(seq_num + 3);
}

TEST_F(PacketBufferTest, ClearSinglePacket) {
  const uint16_t seq_num = Rand();

  for (int i = 0; i < kMaxSize; ++i)
    EXPECT_TRUE(Insert(seq_num + i, kDeltaFrame, kFirst, kLast));

  packet_buffer_.ClearTo(seq_num);
  EXPECT_TRUE(Insert(seq_num + kMaxSize, kDeltaFrame, kFirst, kLast));
}

TEST_F(PacketBufferTest, ClearFullBuffer) {
  for (int i = 0; i < kMaxSize; ++i)
    EXPECT_TRUE(Insert(i, kDeltaFrame, kFirst, kLast));

  packet_buffer_.ClearTo(kMaxSize - 1);

  for (int i = kMaxSize; i < 2 * kMaxSize; ++i)
    EXPECT_TRUE(Insert(i, kDeltaFrame, kFirst, kLast));
}

TEST_F(PacketBufferTest, DontClearNewerPacket) {
  EXPECT_TRUE(Insert(0, kKeyFrame, kFirst, kLast));
  packet_buffer_.ClearTo(0);
  EXPECT_TRUE(Insert(2 * kStartSize, kKeyFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(3 * kStartSize + 1, kKeyFrame, kFirst, kNotLast));
  packet_buffer_.ClearTo(2 * kStartSize);
  EXPECT_TRUE(Insert(3 * kStartSize + 2, kKeyFrame, kNotFirst, kLast));

  ASSERT_EQ(3UL, frames_from_callback_.size());
  CheckFrame(0);
  CheckFrame(2 * kStartSize);
  CheckFrame(3 * kStartSize + 1);
}

TEST_F(PacketBufferTest, OneIncompleteFrame) {
  const uint16_t seq_num = Rand();

  EXPECT_TRUE(Insert(seq_num, kDeltaFrame, kFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + 1, kDeltaFrame, kNotFirst, kLast));
  EXPECT_TRUE(Insert(seq_num - 1, kDeltaFrame, kNotFirst, kLast));

  ASSERT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(seq_num);
}

TEST_F(PacketBufferTest, TwoIncompleteFramesFullBuffer) {
  const uint16_t seq_num = Rand();

  for (int i = 1; i < kMaxSize - 1; ++i)
    EXPECT_TRUE(Insert(seq_num + i, kDeltaFrame, kNotFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num, kDeltaFrame, kFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num - 1, kDeltaFrame, kNotFirst, kLast));

  ASSERT_EQ(0UL, frames_from_callback_.size());
}

TEST_F(PacketBufferTest, FramesReordered) {
  const uint16_t seq_num = Rand();

  EXPECT_TRUE(Insert(seq_num + 1, kDeltaFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(seq_num + 3, kDeltaFrame, kFirst, kLast));
  EXPECT_TRUE(Insert(seq_num + 2, kDeltaFrame, kFirst, kLast));

  ASSERT_EQ(4UL, frames_from_callback_.size());
  CheckFrame(seq_num);
  CheckFrame(seq_num + 1);
  CheckFrame(seq_num + 2);
  CheckFrame(seq_num + 3);
}

TEST_F(PacketBufferTest, GetBitstream) {
  // "many bitstream, such data" with null termination.
  uint8_t many_data[] = {0x6d, 0x61, 0x6e, 0x79, 0x20};
  uint8_t bitstream_data[] = {0x62, 0x69, 0x74, 0x73, 0x74, 0x72,
                              0x65, 0x61, 0x6d, 0x2c, 0x20};
  uint8_t such_data[] = {0x73, 0x75, 0x63, 0x68, 0x20};
  uint8_t data_data[] = {0x64, 0x61, 0x74, 0x61, 0x0};

  uint8_t* many = new uint8_t[sizeof(many_data)];
  uint8_t* bitstream = new uint8_t[sizeof(bitstream_data)];
  uint8_t* such = new uint8_t[sizeof(such_data)];
  uint8_t* data = new uint8_t[sizeof(data_data)];

  memcpy(many, many_data, sizeof(many_data));
  memcpy(bitstream, bitstream_data, sizeof(bitstream_data));
  memcpy(such, such_data, sizeof(such_data));
  memcpy(data, data_data, sizeof(data_data));

  const size_t result_length = sizeof(many_data) + sizeof(bitstream_data) +
                               sizeof(such_data) + sizeof(data_data);

  const uint16_t seq_num = Rand();

  EXPECT_TRUE(
      Insert(seq_num, kKeyFrame, kFirst, kNotLast, sizeof(many_data), many));
  EXPECT_TRUE(Insert(seq_num + 1, kDeltaFrame, kNotFirst, kNotLast,
                     sizeof(bitstream_data), bitstream));
  EXPECT_TRUE(Insert(seq_num + 2, kDeltaFrame, kNotFirst, kNotLast,
                     sizeof(such_data), such));
  EXPECT_TRUE(Insert(seq_num + 3, kDeltaFrame, kNotFirst, kLast,
                     sizeof(data_data), data));

  ASSERT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(seq_num);
  EXPECT_EQ(frames_from_callback_[seq_num]->size(), result_length);
  EXPECT_EQ(memcmp(frames_from_callback_[seq_num]->data(),
                   "many bitstream, such data", result_length),
            0);
}

TEST_F(PacketBufferTest, GetBitstreamOneFrameOnePacket) {
  uint8_t bitstream_data[] = "All the bitstream data for this frame!";
  uint8_t* data = new uint8_t[sizeof(bitstream_data)];
  memcpy(data, bitstream_data, sizeof(bitstream_data));

  EXPECT_TRUE(
      Insert(0, kKeyFrame, kFirst, kLast, sizeof(bitstream_data), data));

  ASSERT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(0);
  EXPECT_EQ(frames_from_callback_[0]->size(), sizeof(bitstream_data));
  EXPECT_EQ(memcmp(frames_from_callback_[0]->data(), bitstream_data,
                   sizeof(bitstream_data)),
            0);
}

TEST_F(PacketBufferTest, GetBitstreamOneFrameFullBuffer) {
  uint8_t* data_arr[kStartSize];
  uint8_t expected[kStartSize];

  for (uint8_t i = 0; i < kStartSize; ++i) {
    data_arr[i] = new uint8_t[1];
    data_arr[i][0] = i;
    expected[i] = i;
  }

  EXPECT_TRUE(Insert(0, kKeyFrame, kFirst, kNotLast, 1, data_arr[0]));
  for (uint8_t i = 1; i < kStartSize - 1; ++i)
    EXPECT_TRUE(Insert(i, kKeyFrame, kNotFirst, kNotLast, 1, data_arr[i]));
  EXPECT_TRUE(Insert(kStartSize - 1, kKeyFrame, kNotFirst, kLast, 1,
                     data_arr[kStartSize - 1]));

  ASSERT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(0);
  EXPECT_EQ(frames_from_callback_[0]->size(), static_cast<size_t>(kStartSize));
  EXPECT_EQ(memcmp(frames_from_callback_[0]->data(), expected, kStartSize), 0);
}

TEST_F(PacketBufferTest, InsertPacketAfterOldFrameObjectIsRemoved) {
  uint16_t kFirstSeqNum = 0;
  uint32_t kTimestampDelta = 100;
  uint32_t timestamp = 10000;
  uint16_t seq_num = kFirstSeqNum;

  // Loop until seq_num wraps around.
  SeqNumUnwrapper<uint16_t> unwrapper;
  while (unwrapper.Unwrap(seq_num) < std::numeric_limits<uint16_t>::max()) {
    Insert(seq_num++, kKeyFrame, kFirst, kNotLast, 0, nullptr, timestamp);
    for (int i = 0; i < 5; ++i) {
      Insert(seq_num++, kKeyFrame, kNotFirst, kNotLast, 0, nullptr, timestamp);
    }
    Insert(seq_num++, kKeyFrame, kNotFirst, kLast, 0, nullptr, timestamp);
    timestamp += kTimestampDelta;
  }

  size_t number_of_frames = frames_from_callback_.size();
  // Delete old frame object while receiving frame with overlapping sequence
  // numbers.
  Insert(seq_num++, kKeyFrame, kFirst, kNotLast, 0, nullptr, timestamp);
  for (int i = 0; i < 5; ++i) {
    Insert(seq_num++, kKeyFrame, kNotFirst, kNotLast, 0, nullptr, timestamp);
  }
  // Delete FrameObject connected to packets that have already been cleared.
  DeleteFrame(kFirstSeqNum);
  Insert(seq_num++, kKeyFrame, kNotFirst, kLast, 0, nullptr, timestamp);

  // Regardless of the initial size, the number of frames should be constant
  // after removing and then adding a new frame object.
  EXPECT_EQ(number_of_frames, frames_from_callback_.size());
}

// If |sps_pps_idr_is_keyframe| is true, we require keyframes to contain
// SPS/PPS/IDR and the keyframes we create as part of the test do contain
// SPS/PPS/IDR. If |sps_pps_idr_is_keyframe| is false, we only require and
// create keyframes containing only IDR.
class PacketBufferH264Test : public PacketBufferTest {
 protected:
  explicit PacketBufferH264Test(bool sps_pps_idr_is_keyframe)
      : PacketBufferTest(sps_pps_idr_is_keyframe
                             ? "WebRTC-SpsPpsIdrIsH264Keyframe/Enabled/"
                             : ""),
        sps_pps_idr_is_keyframe_(sps_pps_idr_is_keyframe) {}

  bool InsertH264(uint16_t seq_num,         // packet sequence number
                  IsKeyFrame keyframe,      // is keyframe
                  IsFirst first,            // is first packet of frame
                  IsLast last,              // is last packet of frame
                  uint32_t timestamp,       // rtp timestamp
                  int data_size = 0,        // size of data
                  uint8_t* data = nullptr,  // data pointer
                  uint32_t width = 0,       // width of frame (SPS/IDR)
                  uint32_t height = 0) {    // height of frame (SPS/IDR)
    VCMPacket packet;
    packet.video_header.codec = kVideoCodecH264;
    auto& h264_header =
        packet.video_header.video_type_header.emplace<RTPVideoHeaderH264>();
    packet.seqNum = seq_num;
    packet.timestamp = timestamp;
    if (keyframe == kKeyFrame) {
      if (sps_pps_idr_is_keyframe_) {
        h264_header.nalus[0].type = H264::NaluType::kSps;
        h264_header.nalus[1].type = H264::NaluType::kPps;
        h264_header.nalus[2].type = H264::NaluType::kIdr;
        h264_header.nalus_length = 3;
      } else {
        h264_header.nalus[0].type = H264::NaluType::kIdr;
        h264_header.nalus_length = 1;
      }
    }
    packet.video_header.width = width;
    packet.video_header.height = height;
    packet.video_header.is_first_packet_in_frame = first == kFirst;
    packet.video_header.is_last_packet_in_frame = last == kLast;
    packet.sizeBytes = data_size;
    packet.dataPtr = data;

    return packet_buffer_.InsertPacket(&packet);
  }

  bool InsertH264KeyFrameWithAud(
      uint16_t seq_num,         // packet sequence number
      IsKeyFrame keyframe,      // is keyframe
      IsFirst first,            // is first packet of frame
      IsLast last,              // is last packet of frame
      uint32_t timestamp,       // rtp timestamp
      int data_size = 0,        // size of data
      uint8_t* data = nullptr,  // data pointer
      uint32_t width = 0,       // width of frame (SPS/IDR)
      uint32_t height = 0) {    // height of frame (SPS/IDR)
    VCMPacket packet;
    packet.video_header.codec = kVideoCodecH264;
    auto& h264_header =
        packet.video_header.video_type_header.emplace<RTPVideoHeaderH264>();
    packet.seqNum = seq_num;
    packet.timestamp = timestamp;

    // this should be the start of frame
    if (kFirst != first) {
      return false;
    }

    // Insert a AUD NALU / packet without width/height.
    h264_header.nalus[0].type = H264::NaluType::kAud;
    h264_header.nalus_length = 1;
    packet.video_header.is_first_packet_in_frame = true;
    packet.video_header.is_last_packet_in_frame = false;
    packet.sizeBytes = 0;
    packet.dataPtr = nullptr;
    if (packet_buffer_.InsertPacket(&packet)) {
      // insert IDR
      return InsertH264(seq_num + 1, keyframe, kNotFirst, last, timestamp,
                        data_size, data, width, height);
    }
    return false;
  }

  const bool sps_pps_idr_is_keyframe_;
};

// This fixture is used to test the general behaviour of the packet buffer
// in both configurations.
class PacketBufferH264ParameterizedTest
    : public ::testing::WithParamInterface<bool>,
      public PacketBufferH264Test {
 protected:
  PacketBufferH264ParameterizedTest() : PacketBufferH264Test(GetParam()) {}
};

INSTANTIATE_TEST_SUITE_P(SpsPpsIdrIsKeyframe,
                         PacketBufferH264ParameterizedTest,
                         ::testing::Bool());

TEST_P(PacketBufferH264ParameterizedTest, DontRemoveMissingPacketOnClearTo) {
  EXPECT_TRUE(InsertH264(0, kKeyFrame, kFirst, kLast, 0));
  EXPECT_TRUE(InsertH264(2, kDeltaFrame, kFirst, kNotLast, 2));
  packet_buffer_.ClearTo(0);
  EXPECT_TRUE(InsertH264(3, kDeltaFrame, kNotFirst, kLast, 2));

  ASSERT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(0);
}

TEST_P(PacketBufferH264ParameterizedTest, GetBitstreamOneFrameFullBuffer) {
  uint8_t* data_arr[kStartSize];
  uint8_t expected[kStartSize];

  for (uint8_t i = 0; i < kStartSize; ++i) {
    data_arr[i] = new uint8_t[1];
    data_arr[i][0] = i;
    expected[i] = i;
  }

  EXPECT_TRUE(InsertH264(0, kKeyFrame, kFirst, kNotLast, 1, 1, data_arr[0]));
  for (uint8_t i = 1; i < kStartSize - 1; ++i) {
    EXPECT_TRUE(
        InsertH264(i, kKeyFrame, kNotFirst, kNotLast, 1, 1, data_arr[i]));
  }
  EXPECT_TRUE(InsertH264(kStartSize - 1, kKeyFrame, kNotFirst, kLast, 1, 1,
                         data_arr[kStartSize - 1]));

  ASSERT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(0);
  EXPECT_EQ(frames_from_callback_[0]->size(), static_cast<size_t>(kStartSize));
  EXPECT_EQ(memcmp(frames_from_callback_[0]->data(), expected, kStartSize), 0);
}

TEST_P(PacketBufferH264ParameterizedTest, GetBitstreamBufferPadding) {
  uint16_t seq_num = Rand();
  uint8_t data_data[] = "some plain old data";
  uint8_t* data = new uint8_t[sizeof(data_data)];
  memcpy(data, data_data, sizeof(data_data));

  VCMPacket packet;
  auto& h264_header =
      packet.video_header.video_type_header.emplace<RTPVideoHeaderH264>();
  h264_header.nalus_length = 1;
  h264_header.nalus[0].type = H264::NaluType::kIdr;
  h264_header.packetization_type = kH264SingleNalu;
  packet.seqNum = seq_num;
  packet.video_header.codec = kVideoCodecH264;
  packet.insertStartCode = true;
  packet.dataPtr = data;
  packet.sizeBytes = sizeof(data_data);
  packet.video_header.is_first_packet_in_frame = true;
  packet.video_header.is_last_packet_in_frame = true;
  packet_buffer_.InsertPacket(&packet);

  ASSERT_EQ(1UL, frames_from_callback_.size());
  EXPECT_EQ(frames_from_callback_[seq_num]->EncodedImage().size(),
            sizeof(data_data));
  EXPECT_EQ(frames_from_callback_[seq_num]->EncodedImage().capacity(),
            sizeof(data_data));
  EXPECT_EQ(memcmp(frames_from_callback_[seq_num]->data(), data_data,
                   sizeof(data_data)),
            0);
}

TEST_P(PacketBufferH264ParameterizedTest, FrameResolution) {
  uint16_t seq_num = 100;
  uint8_t data_data[] = "some plain old data";
  uint8_t* data = new uint8_t[sizeof(data_data)];
  memcpy(data, data_data, sizeof(data_data));
  uint32_t width = 640;
  uint32_t height = 360;
  uint32_t timestamp = 1000;

  EXPECT_TRUE(InsertH264(seq_num, kKeyFrame, kFirst, kLast, timestamp,
                         sizeof(data_data), data, width, height));

  ASSERT_EQ(1UL, frames_from_callback_.size());
  EXPECT_EQ(frames_from_callback_[seq_num]->EncodedImage().size(),
            sizeof(data_data));
  EXPECT_EQ(frames_from_callback_[seq_num]->EncodedImage().capacity(),
            sizeof(data_data));
  EXPECT_EQ(width,
            frames_from_callback_[seq_num]->EncodedImage()._encodedWidth);
  EXPECT_EQ(height,
            frames_from_callback_[seq_num]->EncodedImage()._encodedHeight);
  EXPECT_EQ(memcmp(frames_from_callback_[seq_num]->data(), data_data,
                   sizeof(data_data)),
            0);
}

TEST_P(PacketBufferH264ParameterizedTest, FrameResolutionNaluBeforeSPS) {
  uint16_t seq_num = 100;
  uint8_t data_data[] = "some plain old data";
  uint8_t* data = new uint8_t[sizeof(data_data)];
  memcpy(data, data_data, sizeof(data_data));
  uint32_t width = 640;
  uint32_t height = 360;
  uint32_t timestamp = 1000;

  EXPECT_TRUE(InsertH264KeyFrameWithAud(seq_num, kKeyFrame, kFirst, kLast,
                                        timestamp, sizeof(data_data), data,
                                        width, height));

  CheckFrame(seq_num);
  ASSERT_EQ(1UL, frames_from_callback_.size());
  EXPECT_EQ(frames_from_callback_[seq_num]->EncodedImage().size(),
            sizeof(data_data));
  EXPECT_EQ(frames_from_callback_[seq_num]->EncodedImage().capacity(),
            sizeof(data_data));
  EXPECT_EQ(width,
            frames_from_callback_[seq_num]->EncodedImage()._encodedWidth);
  EXPECT_EQ(height,
            frames_from_callback_[seq_num]->EncodedImage()._encodedHeight);

  EXPECT_EQ(memcmp(frames_from_callback_[seq_num]->data(), data_data,
                   sizeof(data_data)),
            0);
}

TEST_F(PacketBufferTest, FreeSlotsOnFrameCreation) {
  const uint16_t seq_num = Rand();

  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + 1, kDeltaFrame, kNotFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + 2, kDeltaFrame, kNotFirst, kLast));
  EXPECT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(seq_num);

  // Insert frame that fills the whole buffer.
  EXPECT_TRUE(Insert(seq_num + 3, kKeyFrame, kFirst, kNotLast));
  for (int i = 0; i < kMaxSize - 2; ++i)
    EXPECT_TRUE(Insert(seq_num + i + 4, kDeltaFrame, kNotFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + kMaxSize + 2, kKeyFrame, kNotFirst, kLast));
  EXPECT_EQ(2UL, frames_from_callback_.size());
  CheckFrame(seq_num + 3);

  frames_from_callback_.clear();
}

TEST_F(PacketBufferTest, Clear) {
  const uint16_t seq_num = Rand();

  EXPECT_TRUE(Insert(seq_num, kKeyFrame, kFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + 1, kDeltaFrame, kNotFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + 2, kDeltaFrame, kNotFirst, kLast));
  EXPECT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(seq_num);

  packet_buffer_.Clear();

  EXPECT_TRUE(Insert(seq_num + kStartSize, kKeyFrame, kFirst, kNotLast));
  EXPECT_TRUE(
      Insert(seq_num + kStartSize + 1, kDeltaFrame, kNotFirst, kNotLast));
  EXPECT_TRUE(Insert(seq_num + kStartSize + 2, kDeltaFrame, kNotFirst, kLast));
  EXPECT_EQ(2UL, frames_from_callback_.size());
  CheckFrame(seq_num + kStartSize);
}

TEST_F(PacketBufferTest, FramesAfterClear) {
  Insert(9025, kDeltaFrame, kFirst, kLast);
  Insert(9024, kKeyFrame, kFirst, kLast);
  packet_buffer_.ClearTo(9025);
  Insert(9057, kDeltaFrame, kFirst, kLast);
  Insert(9026, kDeltaFrame, kFirst, kLast);

  CheckFrame(9024);
  CheckFrame(9025);
  CheckFrame(9026);
  CheckFrame(9057);
}

TEST_F(PacketBufferTest, SameFrameDifferentTimestamps) {
  Insert(0, kKeyFrame, kFirst, kNotLast, 0, nullptr, 1000);
  Insert(1, kKeyFrame, kNotFirst, kLast, 0, nullptr, 1001);

  ASSERT_EQ(0UL, frames_from_callback_.size());
}

TEST_F(PacketBufferTest, DontLeakPayloadData) {
  // NOTE! Any eventual leak is suppose to be detected by valgrind
  //       or any other similar tool.
  uint8_t* data1 = new uint8_t[5];
  uint8_t* data2 = new uint8_t[5];
  uint8_t* data3 = new uint8_t[5];
  uint8_t* data4 = new uint8_t[5];

  // Expected to free data1 upon PacketBuffer destruction.
  EXPECT_TRUE(Insert(2, kKeyFrame, kFirst, kNotLast, 5, data1));

  // Expect to free data2 upon insertion.
  EXPECT_TRUE(Insert(2, kKeyFrame, kFirst, kNotLast, 5, data2));

  // Expect to free data3 upon insertion (old packet).
  packet_buffer_.ClearTo(1);
  EXPECT_TRUE(Insert(1, kKeyFrame, kFirst, kNotLast, 5, data3));

  // Expect to free data4 upon insertion (packet buffer is full).
  EXPECT_FALSE(Insert(2 + kMaxSize, kKeyFrame, kFirst, kNotLast, 5, data4));
}

TEST_F(PacketBufferTest, ContinuousSeqNumDoubleMarkerBit) {
  Insert(2, kKeyFrame, kNotFirst, kNotLast);
  Insert(1, kKeyFrame, kFirst, kLast);
  frames_from_callback_.clear();
  Insert(3, kKeyFrame, kNotFirst, kLast);

  EXPECT_EQ(0UL, frames_from_callback_.size());
}

TEST_F(PacketBufferTest, PacketTimestamps) {
  absl::optional<int64_t> packet_ms;
  absl::optional<int64_t> packet_keyframe_ms;

  packet_ms = packet_buffer_.LastReceivedPacketMs();
  packet_keyframe_ms = packet_buffer_.LastReceivedKeyframePacketMs();
  EXPECT_FALSE(packet_ms);
  EXPECT_FALSE(packet_keyframe_ms);

  int64_t keyframe_ms = clock_->TimeInMilliseconds();
  EXPECT_TRUE(Insert(100, kKeyFrame, kFirst, kLast));
  packet_ms = packet_buffer_.LastReceivedPacketMs();
  packet_keyframe_ms = packet_buffer_.LastReceivedKeyframePacketMs();
  EXPECT_TRUE(packet_ms);
  EXPECT_TRUE(packet_keyframe_ms);
  EXPECT_EQ(keyframe_ms, *packet_ms);
  EXPECT_EQ(keyframe_ms, *packet_keyframe_ms);

  clock_->AdvanceTimeMilliseconds(100);
  int64_t delta_ms = clock_->TimeInMilliseconds();
  EXPECT_TRUE(Insert(101, kDeltaFrame, kFirst, kLast));
  packet_ms = packet_buffer_.LastReceivedPacketMs();
  packet_keyframe_ms = packet_buffer_.LastReceivedKeyframePacketMs();
  EXPECT_TRUE(packet_ms);
  EXPECT_TRUE(packet_keyframe_ms);
  EXPECT_EQ(delta_ms, *packet_ms);
  EXPECT_EQ(keyframe_ms, *packet_keyframe_ms);

  packet_buffer_.Clear();
  packet_ms = packet_buffer_.LastReceivedPacketMs();
  packet_keyframe_ms = packet_buffer_.LastReceivedKeyframePacketMs();
  EXPECT_FALSE(packet_ms);
  EXPECT_FALSE(packet_keyframe_ms);
}

TEST_F(PacketBufferTest, IncomingCodecChange) {
  VCMPacket packet;
  packet.video_header.is_first_packet_in_frame = true;
  packet.video_header.is_last_packet_in_frame = true;
  packet.sizeBytes = 0;
  packet.dataPtr = nullptr;

  packet.video_header.codec = kVideoCodecVP8;
  packet.video_header.video_type_header.emplace<RTPVideoHeaderVP8>();
  packet.timestamp = 1;
  packet.seqNum = 1;
  packet.video_header.frame_type = VideoFrameType::kVideoFrameKey;
  EXPECT_TRUE(packet_buffer_.InsertPacket(&packet));

  packet.video_header.codec = kVideoCodecH264;
  auto& h264_header =
      packet.video_header.video_type_header.emplace<RTPVideoHeaderH264>();
  h264_header.nalus_length = 1;
  packet.timestamp = 3;
  packet.seqNum = 3;
  EXPECT_TRUE(packet_buffer_.InsertPacket(&packet));

  packet.video_header.codec = kVideoCodecVP8;
  packet.video_header.video_type_header.emplace<RTPVideoHeaderVP8>();
  packet.timestamp = 2;
  packet.seqNum = 2;
  packet.video_header.frame_type = VideoFrameType::kVideoFrameDelta;

  EXPECT_TRUE(packet_buffer_.InsertPacket(&packet));

  EXPECT_EQ(3UL, frames_from_callback_.size());
}

TEST_F(PacketBufferTest, TooManyNalusInPacket) {
  VCMPacket packet;
  packet.video_header.codec = kVideoCodecH264;
  packet.timestamp = 1;
  packet.seqNum = 1;
  packet.video_header.frame_type = VideoFrameType::kVideoFrameKey;
  packet.video_header.is_first_packet_in_frame = true;
  packet.video_header.is_last_packet_in_frame = true;
  auto& h264_header =
      packet.video_header.video_type_header.emplace<RTPVideoHeaderH264>();
  h264_header.nalus_length = kMaxNalusPerPacket;
  packet.sizeBytes = 0;
  packet.dataPtr = nullptr;
  EXPECT_TRUE(packet_buffer_.InsertPacket(&packet));

  EXPECT_EQ(0UL, frames_from_callback_.size());
}

TEST_P(PacketBufferH264ParameterizedTest, OneFrameFillBuffer) {
  InsertH264(0, kKeyFrame, kFirst, kNotLast, 1000);
  for (int i = 1; i < kStartSize - 1; ++i)
    InsertH264(i, kKeyFrame, kNotFirst, kNotLast, 1000);
  InsertH264(kStartSize - 1, kKeyFrame, kNotFirst, kLast, 1000);

  EXPECT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(0);
}

TEST_P(PacketBufferH264ParameterizedTest, CreateFramesAfterFilledBuffer) {
  InsertH264(kStartSize - 2, kKeyFrame, kFirst, kLast, 0);
  ASSERT_EQ(1UL, frames_from_callback_.size());
  frames_from_callback_.clear();

  InsertH264(kStartSize, kDeltaFrame, kFirst, kNotLast, 2000);
  for (int i = 1; i < kStartSize; ++i)
    InsertH264(kStartSize + i, kDeltaFrame, kNotFirst, kNotLast, 2000);
  InsertH264(kStartSize + kStartSize, kDeltaFrame, kNotFirst, kLast, 2000);
  ASSERT_EQ(0UL, frames_from_callback_.size());

  InsertH264(kStartSize - 1, kKeyFrame, kFirst, kLast, 1000);
  ASSERT_EQ(2UL, frames_from_callback_.size());
  CheckFrame(kStartSize - 1);
  CheckFrame(kStartSize);
}

TEST_P(PacketBufferH264ParameterizedTest, OneFrameMaxSeqNum) {
  InsertH264(65534, kKeyFrame, kFirst, kNotLast, 1000);
  InsertH264(65535, kKeyFrame, kNotFirst, kLast, 1000);

  EXPECT_EQ(1UL, frames_from_callback_.size());
  CheckFrame(65534);
}

TEST_P(PacketBufferH264ParameterizedTest, ClearMissingPacketsOnKeyframe) {
  InsertH264(0, kKeyFrame, kFirst, kLast, 1000);
  InsertH264(2, kKeyFrame, kFirst, kLast, 3000);
  InsertH264(3, kDeltaFrame, kFirst, kNotLast, 4000);
  InsertH264(4, kDeltaFrame, kNotFirst, kLast, 4000);

  ASSERT_EQ(3UL, frames_from_callback_.size());

  InsertH264(kStartSize + 1, kKeyFrame, kFirst, kLast, 18000);

  ASSERT_EQ(4UL, frames_from_callback_.size());
  CheckFrame(0);
  CheckFrame(2);
  CheckFrame(3);
  CheckFrame(kStartSize + 1);
}

TEST_P(PacketBufferH264ParameterizedTest, FindFramesOnPadding) {
  InsertH264(0, kKeyFrame, kFirst, kLast, 1000);
  InsertH264(2, kDeltaFrame, kFirst, kLast, 1000);

  ASSERT_EQ(1UL, frames_from_callback_.size());
  packet_buffer_.PaddingReceived(1);
  ASSERT_EQ(2UL, frames_from_callback_.size());
  CheckFrame(0);
  CheckFrame(2);
}

class PacketBufferH264XIsKeyframeTest : public PacketBufferH264Test {
 protected:
  const uint16_t kSeqNum = 5;

  explicit PacketBufferH264XIsKeyframeTest(bool sps_pps_idr_is_keyframe)
      : PacketBufferH264Test(sps_pps_idr_is_keyframe) {
    packet_.video_header.codec = kVideoCodecH264;
    packet_.seqNum = kSeqNum;

    packet_.video_header.is_first_packet_in_frame = true;
    packet_.video_header.is_last_packet_in_frame = true;
  }

  VCMPacket packet_;
};

class PacketBufferH264IdrIsKeyframeTest
    : public PacketBufferH264XIsKeyframeTest {
 protected:
  PacketBufferH264IdrIsKeyframeTest()
      : PacketBufferH264XIsKeyframeTest(false) {}
};

TEST_F(PacketBufferH264IdrIsKeyframeTest, IdrIsKeyframe) {
  auto& h264_header =
      packet_.video_header.video_type_header.emplace<RTPVideoHeaderH264>();
  h264_header.nalus[0].type = H264::NaluType::kIdr;
  h264_header.nalus_length = 1;
  packet_buffer_.InsertPacket(&packet_);

  ASSERT_EQ(1u, frames_from_callback_.size());
  EXPECT_EQ(VideoFrameType::kVideoFrameKey,
            frames_from_callback_[kSeqNum]->frame_type());
}

TEST_F(PacketBufferH264IdrIsKeyframeTest, SpsPpsIdrIsKeyframe) {
  auto& h264_header =
      packet_.video_header.video_type_header.emplace<RTPVideoHeaderH264>();
  h264_header.nalus[0].type = H264::NaluType::kSps;
  h264_header.nalus[1].type = H264::NaluType::kPps;
  h264_header.nalus[2].type = H264::NaluType::kIdr;
  h264_header.nalus_length = 3;

  packet_buffer_.InsertPacket(&packet_);

  ASSERT_EQ(1u, frames_from_callback_.size());
  EXPECT_EQ(VideoFrameType::kVideoFrameKey,
            frames_from_callback_[kSeqNum]->frame_type());
}

class PacketBufferH264SpsPpsIdrIsKeyframeTest
    : public PacketBufferH264XIsKeyframeTest {
 protected:
  PacketBufferH264SpsPpsIdrIsKeyframeTest()
      : PacketBufferH264XIsKeyframeTest(true) {}
};

TEST_F(PacketBufferH264SpsPpsIdrIsKeyframeTest, IdrIsNotKeyframe) {
  auto& h264_header =
      packet_.video_header.video_type_header.emplace<RTPVideoHeaderH264>();
  h264_header.nalus[0].type = H264::NaluType::kIdr;
  h264_header.nalus_length = 1;

  packet_buffer_.InsertPacket(&packet_);

  ASSERT_EQ(1u, frames_from_callback_.size());
  EXPECT_EQ(VideoFrameType::kVideoFrameDelta,
            frames_from_callback_[5]->frame_type());
}

TEST_F(PacketBufferH264SpsPpsIdrIsKeyframeTest, SpsPpsIsNotKeyframe) {
  auto& h264_header =
      packet_.video_header.video_type_header.emplace<RTPVideoHeaderH264>();
  h264_header.nalus[0].type = H264::NaluType::kSps;
  h264_header.nalus[1].type = H264::NaluType::kPps;
  h264_header.nalus_length = 2;

  packet_buffer_.InsertPacket(&packet_);

  ASSERT_EQ(1u, frames_from_callback_.size());
  EXPECT_EQ(VideoFrameType::kVideoFrameDelta,
            frames_from_callback_[kSeqNum]->frame_type());
}

TEST_F(PacketBufferH264SpsPpsIdrIsKeyframeTest, SpsPpsIdrIsKeyframe) {
  auto& h264_header =
      packet_.video_header.video_type_header.emplace<RTPVideoHeaderH264>();
  h264_header.nalus[0].type = H264::NaluType::kSps;
  h264_header.nalus[1].type = H264::NaluType::kPps;
  h264_header.nalus[2].type = H264::NaluType::kIdr;
  h264_header.nalus_length = 3;

  packet_buffer_.InsertPacket(&packet_);

  ASSERT_EQ(1u, frames_from_callback_.size());
  EXPECT_EQ(VideoFrameType::kVideoFrameKey,
            frames_from_callback_[kSeqNum]->frame_type());
}

}  // namespace video_coding
}  // namespace webrtc
