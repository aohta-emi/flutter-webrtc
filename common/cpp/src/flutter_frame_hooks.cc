// Per-frame hooks on a received video track: see flutter_frame_hooks.h,
// webrtc_abi.h and Documentation/frame-hooks.md.
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS  // getenv and fopen, as on the other platforms
#endif
#include "flutter_frame_hooks.h"

#if defined(__linux__) && !defined(_MSC_VER)
#include <dlfcn.h>
#define FLUTTER_WEBRTC_FRAME_HOOKS_SUPPORTED 1
#elif defined(_WIN64)
#include <windows.h>
#define FLUTTER_WEBRTC_FRAME_HOOKS_SUPPORTED 1
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "libwebrtc.h"

namespace flutter_webrtc_plugin {

#if defined(FLUTTER_WEBRTC_FRAME_HOOKS_SUPPORTED)

using namespace webrtc_abi;

namespace {

// True when `obj`'s vtable lies in the same shared object as libwebrtc's
// exported entry point: the pointer read by offset is a libwebrtc object.
bool IsLibwebrtcObject(const void* obj) {
  if (obj == nullptr)
    return false;
  const void* vtable = *reinterpret_cast<const void* const*>(obj);
#if defined(_WIN32)
  HMODULE lib = GetModuleHandleW(L"libwebrtc.dll");
  HMODULE vt = nullptr;
  if (lib == nullptr ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(vtable), &vt))
    return false;
  return vt == lib;
#else
  Dl_info lib{}, vt{};
  if (dladdr(reinterpret_cast<void*>(&libwebrtc::LibWebRTC::Initialize),
             &lib) == 0 ||
      dladdr(vtable, &vt) == 0)
    return false;
  return lib.dli_fbase != nullptr && lib.dli_fbase == vt.dli_fbase;
#endif
}

// How libwebrtc passes the std::unique_ptr<TransformableFrameInterface> of
// Transform and OnTransformedFrame. Chromium's libc++ gives unique_ptr the
// trivial ABI on Linux and macOS: the bare pointer (kDirect). On Windows
// the MSVC ABI passes a class type with a non-trivial destructor by the
// address of a temporary instead, unless the type has the trivial ABI too;
// the first frame tells which (kIndirect: `arg` is that address).
enum class FramePassing { kUnknown, kDirect, kIndirect };

// webrtc::MediaType::VIDEO and MediaStreamTrackInterface::kLive.
constexpr int kMediaTypeVideo = 1;
constexpr int kTrackStateLive = 0;

}  // namespace

// The frame transformer: shows each encoded frame to the observer and
// passes it on unchanged.
class FrameHooks::Transformer : public FrameTransformer {
 public:
  Transformer(int64_t tag, EncodedFrameObserver observer)
      : tag_(tag), observer_(observer) {}

  void AddRef() const override { refs_.fetch_add(1); }
  int Release() const override {
    if (refs_.fetch_sub(1) == 1) {
      delete this;
      return 0;  // kDroppedLastRef
    }
    return 1;  // kOtherRefsRemained
  }

  void Detach() { observer_.store(nullptr); }

  void Transform(TransformableFrame* arg) override {
    TransformableFrame* frame = Unwrap(arg);
    if (frame == nullptr)
      return;  // Not a frame we can read: leave it (never seen).
    if (auto observer = observer_.load()) {
      // A video receiver's transformer gets video frames only.
      auto* video = static_cast<TransformableVideoFrame*>(frame);
      ByteSpan data = frame->GetData();
      observer(tag_, frame->GetTimestamp(),
               video->IsKeyFrame() ? kEncodedFrameTypeKey
                                   : kEncodedFrameTypeDelta,
               frame->GetPayloadType(), data.data, data.size);
    }
    RefPtr<TransformedFrameCallback> cb;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = sink_callbacks_.find(frame->GetSsrc());
      cb = it != sink_callbacks_.end() ? it->second : callback_;
    }
    if (cb) {
      // Ownership passes on, the way it came.
      if (passing_.load() == FramePassing::kIndirect) {
        TransformableFrame* holder = frame;  // a unique_ptr's storage
        cb->OnTransformedFrame(reinterpret_cast<TransformableFrame*>(&holder));
      } else {
        cb->OnTransformedFrame(frame);
      }
    } else {
      delete frame;  // libwebrtc's deleting destructor
    }
  }

  void RegisterTransformedFrameCallback(
      RefPtr<TransformedFrameCallback> callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = callback;
  }
  void RegisterTransformedFrameSinkCallback(
      RefPtr<TransformedFrameCallback> callback,
      uint32_t ssrc) override {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_callbacks_[ssrc] = callback;
  }
  void UnregisterTransformedFrameCallback() override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = RefPtr<TransformedFrameCallback>();
  }
  void UnregisterTransformedFrameSinkCallback(uint32_t ssrc) override {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_callbacks_.erase(ssrc);
  }

 protected:
  ~Transformer() override = default;

 private:
  // The frame `arg` stands for (see FramePassing), or null.
  TransformableFrame* Unwrap(TransformableFrame* arg) {
    FramePassing passing = passing_.load();
    if (passing == FramePassing::kUnknown) {
      if (IsLibwebrtcObject(arg)) {
        passing = FramePassing::kDirect;
      } else if (arg != nullptr &&
                 IsLibwebrtcObject(*reinterpret_cast<void* const*>(arg))) {
        passing = FramePassing::kIndirect;
      } else {
        return nullptr;
      }
      passing_.store(passing);
    }
    if (passing == FramePassing::kIndirect) {
      // The temporary is the callee's to destroy: taking the pointer out
      // and leaving the storage alone owns the frame.
      return *reinterpret_cast<TransformableFrame* const*>(arg);
    }
    return arg;
  }

  std::atomic<FramePassing> passing_{FramePassing::kUnknown};
  const int64_t tag_;
  std::atomic<EncodedFrameObserver> observer_;
  mutable std::atomic<int> refs_{0};
  std::mutex mutex_;
  RefPtr<TransformedFrameCallback> callback_;
  std::map<uint32_t, RefPtr<TransformedFrameCallback>> sink_callbacks_;
};

// The video sink: copies each decoded frame into a wrapper RTCVideoFrame,
// lets the hook change it, and passes it to the renderer.
class FrameHooks::Sink : public VideoSink {
 public:
  Sink(int64_t tag, DecodedFrameHook hook, FrameOut out)
      : tag_(tag),
        hook_(hook),
        out_(std::move(out)),
        log_path_(getenv("FLUTTER_WEBRTC_FRAME_HOOKS_LOG")) {}

  void OnFrame(const VideoFrame& frame) override {
    const auto started = std::chrono::steady_clock::now();
    VideoFrameBuffer* buffer = frame.buffer();
    if (buffer == nullptr)
      return;
    RefPtr<I420Buffer> i420 = buffer->ToI420();
    if (!i420)
      return;
    const int w = i420->width(), h = i420->height();
    auto copy = libwebrtc::RTCVideoFrame::Create(
        w, h, i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
        i420->DataV(), i420->StrideV());
    if (!copy.get())
      return;
    // The copy is ours alone: the hook touches no decoder buffer.
    const int32_t verdict =
        hook_(tag_, frame.rtp_timestamp(), w, h,
              const_cast<uint8_t*>(copy->DataY()), copy->StrideY(),
              const_cast<uint8_t*>(copy->DataU()), copy->StrideU(),
              const_cast<uint8_t*>(copy->DataV()), copy->StrideV());
    if (log_path_ != nullptr)
      Measure(started);
    if (verdict >= 0)
      out_(copy);
  }

 private:
  // With FLUTTER_WEBRTC_FRAME_HOOKS_LOG set (a file, or "-" for stderr):
  // what the sink adds per frame (the copy and the hook), every 300 frames.
  void Measure(std::chrono::steady_clock::time_point started) {
    total_us_ += std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - started)
                     .count();
    if (++frames_ % 300 != 0)
      return;
    const bool to_stderr = log_path_[0] == '-' && log_path_[1] == '\0';
    FILE* f = to_stderr ? stderr : fopen(log_path_, "a");
    if (f == nullptr)
      return;
    fprintf(f, "[frame hooks] tag %lld: %lld frames, copy+hook %.0f us/frame\n",
            static_cast<long long>(tag_), static_cast<long long>(frames_),
            static_cast<double>(total_us_) / static_cast<double>(frames_));
    if (!to_stderr)
      fclose(f);
  }

  const int64_t tag_;
  const DecodedFrameHook hook_;
  const FrameOut out_;
  const char* const log_path_;
  int64_t frames_ = 0;
  int64_t total_us_ = 0;
};

std::unique_ptr<FrameHooks> FrameHooks::Attach(
    libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> receiver,
    int64_t tag,
    EncodedFrameObserver encoded,
    DecodedFrameHook decoded,
    FrameOut out,
    std::string* error) {
  if (encoded == nullptr && decoded == nullptr) {
    *error = "no hook given";
    return nullptr;
  }
  if (!receiver.get() ||
      receiver->media_type() != libwebrtc::RTCMediaType::VIDEO) {
    *error = "not a video receiver";
    return nullptr;
  }
  auto* native = *reinterpret_cast<RtpReceiver* const*>(
      reinterpret_cast<const uint8_t*>(receiver.get()) +
      kRtpReceiverImplNativeOffset);
  if (!IsLibwebrtcObject(native)) {
    *error = "unexpected libwebrtc layout (RTCRtpReceiverImpl)";
    return nullptr;
  }
  // A plain getter before anything is installed: a mirror that does not
  // match the build reads the wrong slot here, not when installing.
  if (native->media_type() != kMediaTypeVideo) {
    *error = "unexpected libwebrtc layout (RtpReceiverInterface)";
    return nullptr;
  }
  std::unique_ptr<FrameHooks> hooks(new FrameHooks());
  hooks->native_receiver_ = RefPtr<RtpReceiver>(native);
  if (decoded != nullptr) {
    hooks->native_track_ = native->track();
    if (!IsLibwebrtcObject(hooks->native_track_.get())) {
      *error = "the receiver has no libwebrtc video track";
      return nullptr;
    }
    if (hooks->native_track_->state() != kTrackStateLive) {
      *error = "unexpected libwebrtc layout (VideoTrackInterface)";
      return nullptr;
    }
  }
  if (encoded != nullptr) {
    hooks->transformer_ = RefPtr<Transformer>(new Transformer(tag, encoded));
    native->SetFrameTransformer(
        RefPtr<FrameTransformer>(hooks->transformer_.get()));
  }
  if (decoded != nullptr) {
    hooks->sink_.reset(new Sink(tag, decoded, std::move(out)));
    hooks->native_track_->AddOrUpdateSink(hooks->sink_.get(),
                                          VideoSinkWants());
  }
  return hooks;
}

FrameHooks::~FrameHooks() {
  if (native_track_ && sink_)
    native_track_->RemoveSink(sink_.get());
  if (transformer_)
    transformer_->Detach();
}

#else  // !FLUTTER_WEBRTC_FRAME_HOOKS_SUPPORTED

class FrameHooks::Transformer : public webrtc_abi::RefCounted {
 public:
  void AddRef() const override {}
  int Release() const override { return 1; }
};
class FrameHooks::Sink {};

std::unique_ptr<FrameHooks> FrameHooks::Attach(
    libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>,
    int64_t,
    EncodedFrameObserver,
    DecodedFrameHook,
    FrameOut,
    std::string* error) {
  *error = "frame hooks are not implemented on this platform";
  return nullptr;
}

FrameHooks::~FrameHooks() {}

#endif  // FLUTTER_WEBRTC_FRAME_HOOKS_SUPPORTED

}  // namespace flutter_webrtc_plugin
