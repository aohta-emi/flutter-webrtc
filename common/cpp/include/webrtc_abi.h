// A minimal mirror of the libwebrtc C++ interfaces that the frame hooks
// (flutter_frame_hooks.h) call or implement, so that the plugin can install
// a webrtc::FrameTransformerInterface on a receiver and add its own
// webrtc::VideoSinkInterface to a remote track without rebuilding libwebrtc.
//
// The prebuilt libwebrtc (webrtc-sdk/libwebrtc) exports only its own wrapper
// API (libwebrtc::*), compiled against Chromium's libc++ (std::__Cr). The
// mirror therefore uses only virtual calls through the webrtc api/
// interfaces and never passes a std:: type across: the vtable slots, the two
// object layouts read below and the argument passing were checked against
// libwebrtc.m150.7871.02 for linux-x64 with tool/check_webrtc_abi.py (the
// prebuilt .so keeps its symbol table).
//
// macOS (common/darwin/Classes/FlutterFrameHooks.mm) uses the frame
// transformer part only (RefCounted to RtpReceiver::SetFrameTransformer),
// against the WebRTC-SDK framework 150.7871.01 (x86_64 and arm64), built
// from the same libwebrtc branch with the same C++ ABI (Itanium vtables,
// Chromium's libc++). tool/check_webrtc_abi.py checks what that framework
// exports: RtpReceiverInterface's slots up to SetFrameTransformer, the
// receiver transformer's Transform slot and calling convention, and the
// Objective-C instance variable that holds the native receiver. The video
// sink part and the object offsets are not used there.
//
// Windows (x64): the prebuilt libwebrtc.dll is built with clang-cl, so it
// uses the MSVC C++ ABI, which the plugin's compiler (MSVC) produces for
// this mirror too. Slot numbers are not written down there: the compiler
// lays the mirror out as it lays out the real interfaces, provided the
// mirror declares the same virtual functions in the same order with the
// same bases. That matters where MSVC differs from the Itanium ABI:
// - one destructor slot instead of two;
// - a function that overrides one of a secondary base stays in that base's
//   vtable: RtpReceiverInterface::SetFrameTransformer (FrameTransformerHost)
//   and VideoTrackInterface::AddOrUpdateSink / RemoveSink
//   (VideoSourceInterface), so those two classes are mirrored with their
//   real bases (#if defined(_WIN32) below);
// - std::vector in VideoSinkWants: MSVC's debug STL adds a member, so the
//   mirror uses a plain struct of the same layout as libc++'s.
// The .dll keeps no symbols (no PDB is published), so no tool checks this
// build: the plugin checks at runtime that the objects belong to
// libwebrtc.dll and that a few plain getters answer as they must before it
// installs anything (flutter_frame_hooks.cc). Checked against
// libwebrtc.m150.7871.02 win-x64 and the m150_release headers.
//
// Re-run tool/check_webrtc_abi.py for every libwebrtc bump
// (third_party/libwebrtc_version.ini, the WebRTC-SDK version of the darwin
// podspecs and Package.swift files) and update the slots it reports; on
// Windows compare the headers of the new branch with the mirror.
#ifndef FLUTTER_WEBRTC_WEBRTC_ABI_H_
#define FLUTTER_WEBRTC_WEBRTC_ABI_H_

#include <climits>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace webrtc_abi {

// The wrapper's libwebrtc::RTCRtpReceiverImpl: vptr, the vptr of its
// webrtc::RtpReceiverObserverInterface base, then
// webrtc::scoped_refptr<webrtc::RtpReceiverInterface> rtp_receiver_.
constexpr size_t kRtpReceiverImplNativeOffset = 16;

// webrtc::VideoFrame: uint16_t id_; scoped_refptr<VideoFrameBuffer>
// video_frame_buffer_; uint32_t timestamp_rtp_; ...
constexpr size_t kVideoFrameBufferOffset = 8;
constexpr size_t kVideoFrameRtpTimestampOffset = 16;

// webrtc::scoped_refptr<T> for any T derived from webrtc::RefCountInterface
// (AddRef at slot 0, Release at slot 1). Non-trivial, so it is passed and
// returned through memory exactly like webrtc's (which has no trivial_abi).
class RefCounted {
 public:
  virtual void AddRef() const = 0;  // 0
  virtual int Release() const = 0;  // 1 (RefCountReleaseStatus)
 protected:
  virtual ~RefCounted() {}  // 2, 3
};

template <class T>
class RefPtr {
 public:
  RefPtr() = default;
  explicit RefPtr(T* p) : p_(p) {
    if (p_)
      p_->AddRef();
  }
  RefPtr(const RefPtr& o) : RefPtr(o.p_) {}
  RefPtr& operator=(const RefPtr& o) {
    RefPtr tmp(o);
    std::swap(p_, tmp.p_);
    return *this;
  }
  ~RefPtr() {
    if (p_)
      p_->Release();
  }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

 private:
  T* p_ = nullptr;
};

// std::span<const uint8_t>: {data, size}, trivially copyable in both libc++
// and libstdc++, returned in registers.
struct ByteSpan {
  const uint8_t* data;
  size_t size;
};

// webrtc::TransformableFrameInterface (frames are owned by whoever holds
// them; the deleting destructor is slot 1).
class TransformableFrame {
 public:
  virtual ~TransformableFrame() {}             // 0, 1
  virtual ByteSpan GetData() const = 0;        // 2
  virtual void SetData_(ByteSpan) = 0;         // 3
  virtual uint8_t GetPayloadType() const = 0;  // 4
  virtual bool CanSetPayloadType_() const = 0;  // 5
  virtual void SetPayloadType_(uint8_t) = 0;   // 6
  virtual uint32_t GetSsrc() const = 0;        // 7
  virtual uint32_t GetTimestamp() const = 0;   // 8 (RTP)
  virtual void SetRTPTimestamp_(uint32_t) = 0;  // 9
  virtual void slot10_() = 0;
  virtual void slot11_() = 0;
  virtual void slot12_() = 0;
  virtual void GetMimeType_() = 0;              // 13
  virtual void slot14_() = 0;
  virtual void CaptureTime_() = 0;              // 15
  virtual void slot16_() = 0;
  virtual void slot17_() = 0;
  virtual void SenderCaptureTimeOffset_() = 0;  // 18
};

// webrtc::TransformableVideoFrameInterface: the frames a video receiver's
// transformer gets.
class TransformableVideoFrame : public TransformableFrame {
 public:
  virtual bool IsKeyFrame() const = 0;  // 19
};

// webrtc::TransformedFrameCallback. OnTransformedFrame takes a
// std::unique_ptr<TransformableFrameInterface>; Chromium's libc++ gives
// unique_ptr the trivial ABI, so it is passed as the bare pointer (seen in
// FrameCryptorTransformer::Transform's code).
class TransformedFrameCallback : public RefCounted {
 public:
  virtual void OnTransformedFrame(TransformableFrame* frame) = 0;  // 4
  virtual void StartShortCircuiting() = 0;                          // 5
};

// webrtc::FrameTransformerInterface, implemented by the plugin.
class FrameTransformer : public RefCounted {
 public:
  virtual void Transform(TransformableFrame* frame) = 0;  // 4 (unique_ptr)
  virtual void RegisterTransformedFrameCallback(
      RefPtr<TransformedFrameCallback> callback) = 0;  // 5
  virtual void RegisterTransformedFrameSinkCallback(
      RefPtr<TransformedFrameCallback> callback,
      uint32_t ssrc) = 0;                                         // 6
  virtual void UnregisterTransformedFrameCallback() = 0;          // 7
  virtual void UnregisterTransformedFrameSinkCallback(uint32_t) = 0;  // 8
};

// webrtc::I420BufferInterface (through VideoFrameBuffer, PlanarYuvBuffer,
// PlanarYuv8Buffer).
class I420Buffer : public RefCounted {
 public:
  virtual int type() const = 0;                  // 4
  virtual int width() const = 0;                 // 5
  virtual int height() const = 0;                // 6
  virtual RefPtr<I420Buffer> ToI420() = 0;       // 7
  virtual void GetI420_() = 0;                   // 8
  virtual void CropAndScale_() = 0;              // 9
  virtual void GetMappedFrameBuffer_() = 0;      // 10
  virtual void storage_representation_() = 0;    // 11
  virtual void PrepareMappedBufferAsync_() = 0;  // 12
  virtual int ChromaWidth() const = 0;           // 13
  virtual int ChromaHeight() const = 0;          // 14
  virtual int StrideY() const = 0;               // 15
  virtual int StrideU() const = 0;               // 16
  virtual int StrideV() const = 0;               // 17
  virtual const uint8_t* DataY() const = 0;      // 18
  virtual const uint8_t* DataU() const = 0;      // 19
  virtual const uint8_t* DataV() const = 0;      // 20
};
// A webrtc::VideoFrameBuffer of any type: slots 0-7 as above.
using VideoFrameBuffer = I420Buffer;

// webrtc::VideoFrame, read by offset (its accessors are inline).
struct VideoFrame {
  uint32_t rtp_timestamp() const {
    return *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(this) + kVideoFrameRtpTimestampOffset);
  }
  VideoFrameBuffer* buffer() const {
    return *reinterpret_cast<VideoFrameBuffer* const*>(
        reinterpret_cast<const uint8_t*>(this) + kVideoFrameBufferOffset);
  }
};

// webrtc::VideoSinkInterface<webrtc::VideoFrame>, implemented by the plugin.
class VideoSink {
 public:
  virtual ~VideoSink() {}                             // 0, 1
  virtual void OnFrame(const VideoFrame& frame) = 0;  // 2
  virtual void OnDiscardedFrame() {}                  // 3
  virtual void OnConstraintsChanged(const void*) {}   // 4
};

// webrtc::VideoSinkWants (api/video/video_source_interface.h), the
// defaults. Same layout with libstdc++'s optional and vector as with
// libc++'s; libwebrtc only copies it.
struct VideoSinkWants {
  struct FrameSize {
    int width;
    int height;
  };
  struct Aggregates {
    bool any_active_without_requested_resolution = false;
  };
#if defined(_WIN32)
  // libc++'s std::vector (begin, end, end of storage), empty: MSVC's own
  // has an extra member in debug builds.
  struct Vector {
    FrameSize* begin = nullptr;
    FrameSize* end = nullptr;
    FrameSize* capacity = nullptr;
  };
#endif
  bool rotation_applied = false;
  bool black_frames = false;
  int max_pixel_count = INT_MAX;
  std::optional<int> target_pixel_count;
  int max_framerate_fps = INT_MAX;
  int resolution_alignment = 1;
#if defined(_WIN32)
  Vector resolutions;
#else
  std::vector<FrameSize> resolutions;
#endif
  std::optional<FrameSize> requested_resolution;
  bool is_active = false;
  std::optional<Aggregates> aggregates;
};
#if (defined(__linux__) || defined(_WIN32)) && UINTPTR_MAX == 0xffffffffffffffffu
static_assert(sizeof(VideoSinkWants) == 64, "VideoSinkWants layout");
#endif

#if defined(_WIN32)

// webrtc::NotifierInterface, MediaStreamTrackInterface's secondary base.
class Notifier {
 public:
  virtual void RegisterObserver_(void*) = 0;
  virtual void UnregisterObserver_(void*) = 0;
  virtual ~Notifier() {}
};

// webrtc::MediaStreamTrackInterface.
class MediaStreamTrack : public RefCounted, public Notifier {
 public:
  virtual void kind_() = 0;
  virtual void id_() = 0;
  virtual bool enabled() const = 0;
  virtual void set_enabled_() = 0;
  virtual int state() const = 0;  // TrackState: 0 live, 1 ended
};

// webrtc::VideoSourceInterface<webrtc::VideoFrame>, VideoTrackInterface's
// secondary base: AddOrUpdateSink and RemoveSink are in its vtable.
class VideoSource {
 public:
  virtual ~VideoSource() {}
  virtual void AddOrUpdateSink(VideoSink* sink,
                               const VideoSinkWants& wants) = 0;
  virtual void RemoveSink(VideoSink* sink) = 0;
  virtual void RequestRefreshFrame_() {}
};

// webrtc::VideoTrackInterface (the proxy the receiver returns).
class VideoTrack : public MediaStreamTrack, public VideoSource {};

// webrtc::FrameTransformerHost, RtpReceiverInterface's secondary base:
// SetFrameTransformer is in its vtable.
class FrameTransformerHost {
 public:
  virtual ~FrameTransformerHost() {}
  virtual void SetFrameTransformer(RefPtr<FrameTransformer> transformer) = 0;
};

// webrtc::RtpReceiverInterface (the proxy the wrapper holds).
class RtpReceiver : public RefCounted, public FrameTransformerHost {
 public:
  virtual RefPtr<VideoTrack> track() const = 0;  // MediaStreamTrack
  virtual void dtls_transport_() = 0;
  virtual void stream_ids_() = 0;
  virtual void streams_() = 0;
  virtual int media_type() const = 0;  // MediaType: 0 audio, 1 video
  virtual void id_() = 0;
  virtual void GetParameters_() = 0;
  virtual void SetParameters_() = 0;
  virtual void SetObserver_() = 0;
  virtual void SetJitterBufferMinimumDelay_() = 0;
  virtual void GetSources_() = 0;
  virtual void SetFrameDecryptor_() = 0;
  virtual void GetFrameDecryptor_() = 0;
  virtual void SetDepacketizerToDecoderFrameTransformer_() = 0;
  virtual void CreateSframeDecrypterOrError_() = 0;
};

#else  // Itanium

// webrtc::VideoTrackInterface (the proxy the receiver returns).
class VideoTrack : public RefCounted {
 public:
  virtual void kind_() = 0;          // 4
  virtual void id_() = 0;            // 5
  virtual bool enabled() const = 0;  // 6
  virtual void set_enabled_() = 0;   // 7
  virtual int state() const = 0;     // 8
  virtual void AddOrUpdateSink(VideoSink* sink,
                               const VideoSinkWants& wants) = 0;  // 9
  virtual void RemoveSink(VideoSink* sink) = 0;                   // 10
};

// webrtc::RtpReceiverInterface (the proxy the wrapper holds).
class RtpReceiver : public RefCounted {
 public:
  virtual RefPtr<VideoTrack> track() const = 0;  // 4 (MediaStreamTrack)
  virtual void dtls_transport_() = 0;            // 5
  virtual void stream_ids_() = 0;                // 6
  virtual void streams_() = 0;                   // 7
  virtual int media_type() const = 0;            // 8
  virtual void id_() = 0;                        // 9
  virtual void GetParameters_() = 0;             // 10
  virtual void SetParameters_() = 0;             // 11
  virtual void SetObserver_() = 0;               // 12
  virtual void SetJitterBufferMinimumDelay_() = 0;  // 13
  virtual void GetSources_() = 0;                // 14
  virtual void SetFrameDecryptor_() = 0;         // 15
  virtual void GetFrameDecryptor_() = 0;         // 16
  virtual void SetDepacketizerToDecoderFrameTransformer_() = 0;  // 17
  virtual void SetFrameTransformer(
      RefPtr<FrameTransformer> transformer) = 0;  // 18
};

#endif  // _WIN32

}  // namespace webrtc_abi

#endif  // FLUTTER_WEBRTC_WEBRTC_ABI_H_
