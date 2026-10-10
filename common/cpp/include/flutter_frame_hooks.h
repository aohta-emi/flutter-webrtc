// Per-frame hooks on a received video track: an encoded frame observer and
// a decoded frame hook (Documentation/frame-hooks.md).
//
// For one RTCRtpReceiver and the FlutterVideoRenderer that shows its track:
//
// * The encoded frame observer is called with every received encoded frame
//   (the frame as libwebrtc hands it to the decoder, e.g. an H.264 access
//   unit in Annex B), its RTP timestamp, frame type and RTP payload type.
//   It runs in a pass-through frame transformer on the receiver (the
//   mechanism E2EE's FrameCryptor uses): the frame goes on unchanged.
// * The decoded frame hook is called with every decoded frame, as a
//   writable I420 copy, and its RTP timestamp, before the renderer gets it.
//   Whatever it changes in the planes is what the texture shows.
//
// The RTP timestamp links the two: the decoded frame carries the timestamp
// of the encoded frame it was decoded from.
//
// Both are plain C functions (from dart:ffi `Pointer.address`, or any
// native library), called on libwebrtc's threads; they must not block. The
// plugin knows nothing about what they do. Linux (Itanium C++ ABI) only for
// now; elsewhere attaching fails with an error.
#ifndef FLUTTER_WEBRTC_FRAME_HOOKS_H_
#define FLUTTER_WEBRTC_FRAME_HOOKS_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "rtc_rtp_receiver.h"
#include "rtc_video_frame.h"
#include "webrtc_abi.h"

namespace flutter_webrtc_plugin {

// `frame_type` of the encoded frame observer.
constexpr int32_t kEncodedFrameTypeDelta = 0;
constexpr int32_t kEncodedFrameTypeKey = 1;

// Called with every received encoded frame. `tag` is the value given when
// attaching (by default the renderer's texture id); `data` is valid for the
// call only and must not be written to.
typedef void (*EncodedFrameObserver)(int64_t tag,
                                     uint32_t rtp_timestamp,
                                     int32_t frame_type,
                                     int32_t payload_type,
                                     const uint8_t* data,
                                     size_t size);

// Called with every decoded frame, an I420 copy the hook may write to
// (`height` rows of Y, `(height + 1) / 2` rows of U and V). Returns a
// negative value to drop the frame (the renderer keeps the previous one),
// anything else to render it.
typedef int32_t (*DecodedFrameHook)(int64_t tag,
                                    uint32_t rtp_timestamp,
                                    int32_t width,
                                    int32_t height,
                                    uint8_t* y,
                                    int32_t stride_y,
                                    uint8_t* u,
                                    int32_t stride_u,
                                    uint8_t* v,
                                    int32_t stride_v);

class FrameHooks {
 public:
  using FrameOut =
      std::function<void(libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame>)>;

  // Installs the hooks (either may be null, not both). With a decoded frame
  // hook, `out` gets every decoded frame after the hook, and the caller
  // stops taking frames from the track's own renderer interface. Returns
  // nullptr and sets `error` when the platform has no implementation or the
  // receiver does not look like the libwebrtc build webrtc_abi.h was checked
  // against.
  static std::unique_ptr<FrameHooks> Attach(
      libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> receiver,
      int64_t tag,
      EncodedFrameObserver encoded,
      DecodedFrameHook decoded,
      FrameOut out,
      std::string* error);

  // Removes the sink; the transformer stays on the receiver as a plain
  // pass-through (libwebrtc keeps its own reference).
  ~FrameHooks();

  bool has_decoded_hook() const { return sink_ != nullptr; }

  class Transformer;
  class Sink;

 private:
  FrameHooks() = default;
  webrtc_abi::RefPtr<webrtc_abi::RtpReceiver> native_receiver_;
  webrtc_abi::RefPtr<webrtc_abi::VideoTrack> native_track_;
  webrtc_abi::RefPtr<Transformer> transformer_;
  std::unique_ptr<Sink> sink_;
};

}  // namespace flutter_webrtc_plugin

#endif  // FLUTTER_WEBRTC_FRAME_HOOKS_H_
