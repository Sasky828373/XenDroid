/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENDROID_XE_AAUDIO_AUDIO_DRIVER_H
#define XENDROID_XE_AAUDIO_AUDIO_DRIVER_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <stack>
#include <thread>

#include <aaudio/AAudio.h>

#include "xenia/apu/audio_driver.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
namespace aaudio {

class AAudioAudioDriver : public AudioDriver {
 public:
  AAudioAudioDriver(Memory* memory, xe::threading::Semaphore* semaphore);
  ~AAudioAudioDriver() override;

  bool Initialize();
    void Pause() override;
    void Resume() override;
    void SetVolume(float volume) override;
  void SubmitFrame(float* frame) override;
  void Shutdown();

 protected:
  static aaudio_data_callback_result_t AudioCallback(
      AAudioStream* stream,
      void* userdata,
      void* audioData,
      int32_t numFrames);

  static void AudioErrorCallback(
      AAudioStream* stream,
      void* userdata,
      aaudio_result_t error);

  // Callback thread only.
  void ConcealGap(float* output, int32_t out_samples, int32_t copy_samples);
  void ApplyFadeIn();
  // AAudio has no stream volume control, so this is done in software.
  // Callback thread only.
  void ApplyGainAndClamp();

  // (Re)opens the stream on the current default device; caller holds stream_mutex_.
  bool BuildStream();
  // Rebuilds the stream on the current default device; true once a stream is
  // live (or we're shutting down). Runs only on recovery_thread_.
  bool RestartStream();
  // Performs rebuild requests from the error callback, retrying on failure.
  void RecoveryThreadMain();

  xe::threading::Semaphore* semaphore_ = nullptr;

  AAudioStreamBuilder* builder_ = nullptr;
  AAudioStream* stream_ = nullptr;
  bool stream_initialized_ = false;
  // Serializes stream lifecycle. The data callback uses its `stream` argument,
  // not stream_, so it never takes this.
  std::mutex stream_mutex_ = {};

  // Output-device-change recovery: the error callback flags a request and
  // recovery_thread_ does the close+reopen (AAudio forbids closing from the
  // callback thread). Started in Initialize, joined in Shutdown.
  std::thread recovery_thread_ = {};
  std::mutex recovery_mutex_ = {};
  std::condition_variable recovery_cv_ = {};
  bool restart_requested_ = false;
  bool recovery_quit_ = false;
  std::atomic<bool> shutting_down_{false};

  static constexpr uint32_t host_frame_frequency_ = 48000;
  static constexpr uint32_t host_frame_channels_ = 2;
  static constexpr uint32_t channel_samples_ = 256;
  static constexpr uint32_t x360_frame_samples_ = 6 * channel_samples_;
  static constexpr uint32_t host_block_samples_ = host_frame_channels_ * channel_samples_;
  std::queue<float*> frames_queued_ = {};
  std::stack<float*> frames_unused_ = {};
  std::mutex frames_mutex_ = {};

  // Underrun concealment: silence would put a step at both edges of every
  // gap, a ~187Hz click train at a 5.3ms block. Callback thread only.
  float last_block_[host_block_samples_] = {};
  bool last_block_valid_ = false;
  // Frames of last_block_ already handed to the device. A callback size that
  // is not exactly channel_samples_ would otherwise drop or duplicate audio.
  uint32_t last_block_pos_ = channel_samples_;
  uint32_t gap_blocks_ = 0;

  bool fade_in_pending_ = false;

  // Written by the realtime callback, drained by recovery_thread_: relaxed
  // atomics only, nothing that could block the callback.
  std::atomic<uint64_t> stat_callbacks_{0};
  std::atomic<uint64_t> stat_gaps_{0};
  std::atomic<uint64_t> stat_queue_depth_sum_{0};
  std::atomic<uint32_t> stat_queue_depth_max_{0};
  // A block size we did not ask for silently changes the drain rate.
  std::atomic<int32_t> stat_unexpected_frames_{0};
  // Samples the downmix pushed past full scale.
  std::atomic<uint64_t> stat_clipped_{0};
  void LogAndResetStats();

  // Per-driver volume (XMP): written by other threads, read by the callback.
  std::atomic<float> driver_volume_{1.0f};
};

}  // namespace aaudio
}  // namespace apu
}  // namespace xe

#endif //xendroid_XE_AAUDIO_AUDIO_DRIVER_H
