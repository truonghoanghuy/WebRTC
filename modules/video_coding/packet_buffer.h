/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_PACKET_BUFFER_H_
#define MODULES_VIDEO_CODING_PACKET_BUFFER_H_

#include <memory>
#include <queue>
#include <set>
#include <vector>

#include "api/video/encoded_image.h"
#include "modules/video_coding/packet.h"
#include "rtc_base/critical_section.h"
#include "rtc_base/numerics/sequence_number_util.h"
#include "rtc_base/thread_annotations.h"

namespace webrtc {

class Clock;

namespace video_coding {

class RtpFrameObject;

// A frame is assembled when all of its packets have been received.
class OnAssembledFrameCallback {
 public:
  virtual ~OnAssembledFrameCallback() {}
  virtual void OnAssembledFrame(std::unique_ptr<RtpFrameObject> frame) = 0;
};

class PacketBuffer {
 public:
  // Both |start_buffer_size| and |max_buffer_size| must be a power of 2.
  PacketBuffer(Clock* clock,
               size_t start_buffer_size,
               size_t max_buffer_size,
               OnAssembledFrameCallback* frame_callback);
  ~PacketBuffer();

  // Returns true unless the packet buffer is cleared, which means that a key
  // frame request should be sent. The PacketBuffer will always take ownership
  // of the |packet.dataPtr| when this function is called.
  bool InsertPacket(VCMPacket* packet);
  void ClearTo(uint16_t seq_num);
  void Clear();
  void PaddingReceived(uint16_t seq_num);

  // Timestamp (not RTP timestamp) of the last received packet/keyframe packet.
  absl::optional<int64_t> LastReceivedPacketMs() const;
  absl::optional<int64_t> LastReceivedKeyframePacketMs() const;

  // Returns number of different frames seen in the packet buffer
  int GetUniqueFramesSeen() const;

 private:
  struct StoredPacket {
    uint16_t seq_num() const { return data.seqNum; }

    // If this is the first packet of the frame.
    bool frame_begin() const { return data.is_first_packet_in_frame(); }

    // If this is the last packet of the frame.
    bool frame_end() const { return data.is_last_packet_in_frame(); }

    // If this slot is currently used.
    bool used = false;

    // If all its previous packets have been inserted into the packet buffer.
    bool continuous = false;

    VCMPacket data;
  };

  Clock* const clock_;

  // Tries to expand the buffer.
  bool ExpandBufferSize() RTC_EXCLUSIVE_LOCKS_REQUIRED(crit_);

  // Test if all previous packets has arrived for the given sequence number.
  bool PotentialNewFrame(uint16_t seq_num) const
      RTC_EXCLUSIVE_LOCKS_REQUIRED(crit_);

  // Test if all packets of a frame has arrived, and if so, creates a frame.
  // Returns a vector of received frames.
  std::vector<std::unique_ptr<RtpFrameObject>> FindFrames(uint16_t seq_num)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(crit_);

  rtc::scoped_refptr<EncodedImageBuffer> GetEncodedImageBuffer(
      size_t frame_size,
      uint16_t first_seq_num,
      uint16_t last_seq_num) RTC_EXCLUSIVE_LOCKS_REQUIRED(crit_);

  // Get the packet with sequence number |seq_num|.
  VCMPacket* GetPacket(uint16_t seq_num) RTC_EXCLUSIVE_LOCKS_REQUIRED(crit_);

  // Clears the packet buffer from |start_seq_num| to |stop_seq_num| where the
  // endpoints are inclusive.
  void ClearInterval(uint16_t start_seq_num, uint16_t stop_seq_num)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(crit_);

  void UpdateMissingPackets(uint16_t seq_num)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(crit_);

  // Counts unique received timestamps and updates |unique_frames_seen_|.
  void OnTimestampReceived(uint32_t rtp_timestamp)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(crit_);

  rtc::CriticalSection crit_;

  // buffer_.size() and max_size_ must always be a power of two.
  const size_t max_size_;

  // The fist sequence number currently in the buffer.
  uint16_t first_seq_num_ RTC_GUARDED_BY(crit_);

  // If the packet buffer has received its first packet.
  bool first_packet_received_ RTC_GUARDED_BY(crit_);

  // If the buffer is cleared to |first_seq_num_|.
  bool is_cleared_to_first_seq_num_ RTC_GUARDED_BY(crit_);

  // Buffer that holds the the inserted packets and information needed to
  // determine continuity between them.
  std::vector<StoredPacket> buffer_ RTC_GUARDED_BY(crit_);

  // Called when all packets in a frame are received, allowing the frame
  // to be assembled.
  OnAssembledFrameCallback* const assembled_frame_callback_;

  // Timestamp (not RTP timestamp) of the last received packet/keyframe packet.
  absl::optional<int64_t> last_received_packet_ms_ RTC_GUARDED_BY(crit_);
  absl::optional<int64_t> last_received_keyframe_packet_ms_
      RTC_GUARDED_BY(crit_);

  int unique_frames_seen_ RTC_GUARDED_BY(crit_);

  absl::optional<uint16_t> newest_inserted_seq_num_ RTC_GUARDED_BY(crit_);
  std::set<uint16_t, DescendingSeqNumComp<uint16_t>> missing_packets_
      RTC_GUARDED_BY(crit_);

  // Indicates if we should require SPS, PPS, and IDR for a particular
  // RTP timestamp to treat the corresponding frame as a keyframe.
  const bool sps_pps_idr_is_h264_keyframe_;

  // Stores several last seen unique timestamps for quick search.
  std::set<uint32_t> rtp_timestamps_history_set_ RTC_GUARDED_BY(crit_);
  // Stores the same unique timestamps in the order of insertion.
  std::queue<uint32_t> rtp_timestamps_history_queue_ RTC_GUARDED_BY(crit_);
};

}  // namespace video_coding
}  // namespace webrtc

#endif  // MODULES_VIDEO_CODING_PACKET_BUFFER_H_
