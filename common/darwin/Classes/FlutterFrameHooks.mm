// Per-frame hooks on a received video track: see FlutterFrameHooks.h,
// webrtc_abi.h and Documentation/frame-hooks.md.
#import "FlutterFrameHooks.h"

#import <WebRTC/RTCMutableI420Buffer.h>
#import <WebRTC/RTCYUVHelper.h>
#import <os/lock.h>

#include <dlfcn.h>
#include <objc/runtime.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#include "webrtc_abi.h"

using namespace webrtc_abi;

namespace {

// RTCRtpReceiver keeps its native receiver in this instance variable, a
// webrtc::scoped_refptr<webrtc::RtpReceiverInterface> (one pointer). Its
// name and type encoding are checked at runtime before it is read.
constexpr char kNativeReceiverIvar[] = "_nativeRtpReceiver";
constexpr char kNativeReceiverType[] =
    "{scoped_refptr<webrtc::RtpReceiverInterface>=\"ptr_\"^"
    "{RtpReceiverInterface}}";

// True when `obj`'s vtable lies in the WebRTC framework's image: the
// pointer read from the instance variable is a libwebrtc object.
bool IsWebRTCObject(const void* obj) {
  if (obj == nullptr)
    return false;
  const void* vtable = *reinterpret_cast<const void* const*>(obj);
  Dl_info lib{}, vt{};
  if (dladdr((__bridge const void*)[RTCRtpReceiver class], &lib) == 0 ||
      dladdr(vtable, &vt) == 0)
    return false;
  return lib.dli_fbase != nullptr && lib.dli_fbase == vt.dli_fbase;
}

// The native receiver behind `receiver`, or nullptr with `error` set.
RtpReceiver* NativeReceiver(RTCRtpReceiver* receiver, NSString** error) {
  Ivar ivar = class_getInstanceVariable([RTCRtpReceiver class],
                                        kNativeReceiverIvar);
  const char* type = ivar ? ivar_getTypeEncoding(ivar) : nullptr;
  if (type == nullptr || strcmp(type, kNativeReceiverType) != 0) {
    *error = @"unexpected WebRTC framework layout (RTCRtpReceiver)";
    return nullptr;
  }
  auto* native = *reinterpret_cast<RtpReceiver* const*>(
      reinterpret_cast<const uint8_t*>((__bridge const void*)receiver) +
      ivar_getOffset(ivar));
  if (!IsWebRTCObject(native)) {
    *error = @"unexpected WebRTC framework layout (native receiver)";
    return nullptr;
  }
  return native;
}

// The frame transformer: shows each encoded frame to the observer and
// passes it on unchanged.
class Transformer : public FrameTransformer {
 public:
  Transformer(int64_t tag, FlutterEncodedFrameObserver observer)
      : tag_(tag), observer_(observer) {}

  void AddRef() const override { refs_.fetch_add(1); }
  int Release() const override {
    if (refs_.fetch_sub(1) == 1) {
      delete this;
      return 0;  // kDroppedLastRef
    }
    return 1;  // kOtherRefsRemained
  }

  // Once this returns the observer is neither running nor called again.
  void Detach() {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    observer_ = nullptr;
  }

  void Transform(TransformableFrame* frame) override {
    {
      std::lock_guard<std::mutex> lock(observer_mutex_);
      if (observer_ != nullptr) {
        // A video receiver's transformer gets video frames only.
        auto* video = static_cast<TransformableVideoFrame*>(frame);
        ByteSpan data = frame->GetData();
        observer_(tag_, frame->GetTimestamp(), video->IsKeyFrame() ? 1 : 0,
                  frame->GetPayloadType(), data.data, data.size);
      }
    }
    RefPtr<TransformedFrameCallback> cb;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = sink_callbacks_.find(frame->GetSsrc());
      cb = it != sink_callbacks_.end() ? it->second : callback_;
    }
    if (cb) {
      cb->OnTransformedFrame(frame);  // ownership passes on
    } else {
      delete frame;  // libwebrtc's deleting destructor (slot 1)
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
  const int64_t tag_;
  std::mutex observer_mutex_;
  FlutterEncodedFrameObserver observer_;
  mutable std::atomic<int> refs_{0};
  std::mutex mutex_;
  RefPtr<TransformedFrameCallback> callback_;
  std::map<uint32_t, RefPtr<TransformedFrameCallback>> sink_callbacks_;
};

}  // namespace

@implementation FlutterFrameHooks {
  int64_t _tag;
  RefPtr<RtpReceiver> _nativeReceiver;
  RefPtr<Transformer> _transformer;
  // Guards _decoded: held while the hook runs, so that -detach waits for
  // a running hook.
  os_unfair_lock _lock;
  FlutterDecodedFrameHook _decoded;
  BOOL _hasDecodedFrameHook;
  const char* _logPath;
  int64_t _frames;
  int64_t _totalUs;
}

+ (nullable instancetype)attachToReceiver:(RTCRtpReceiver*)receiver
                                      tag:(int64_t)tag
                     encodedFrameObserver:(nullable FlutterEncodedFrameObserver)encoded
                         decodedFrameHook:(nullable FlutterDecodedFrameHook)decoded
                                    error:(NSString* _Nullable* _Nonnull)error {
  if (encoded == nullptr && decoded == nullptr) {
    *error = @"no hook given";
    return nil;
  }
  if (![receiver isKindOfClass:[RTCRtpReceiver class]] ||
      ![receiver.track.kind isEqualToString:kRTCMediaStreamTrackKindVideo]) {
    *error = @"not a video receiver";
    return nil;
  }
  FlutterFrameHooks* hooks = [[FlutterFrameHooks alloc] init];
  hooks->_tag = tag;
  hooks->_lock = OS_UNFAIR_LOCK_INIT;
  hooks->_decoded = decoded;
  hooks->_hasDecodedFrameHook = decoded != nullptr;
  hooks->_logPath = getenv("FLUTTER_WEBRTC_FRAME_HOOKS_LOG");
  if (encoded != nullptr) {
    RtpReceiver* native = NativeReceiver(receiver, error);
    if (native == nullptr)
      return nil;
    hooks->_nativeReceiver = RefPtr<RtpReceiver>(native);
    hooks->_transformer = RefPtr<Transformer>(new Transformer(tag, encoded));
    native->SetFrameTransformer(
        RefPtr<FrameTransformer>(hooks->_transformer.get()));
  }
  return hooks;
}

- (BOOL)hasDecodedFrameHook {
  return _hasDecodedFrameHook;
}

- (nullable RTCVideoFrame*)processDecodedFrame:(RTCVideoFrame*)frame {
  const auto started = std::chrono::steady_clock::now();
  os_unfair_lock_lock(&_lock);
  FlutterDecodedFrameHook hook = _decoded;
  if (hook == nullptr) {
    os_unfair_lock_unlock(&_lock);
    return frame;
  }
  id<RTCVideoFrameBuffer> source = frame.buffer;
  id<RTCI420Buffer> i420 = [source toI420];
  if (i420 != nil && (id)i420 == (id)source) {
    // Already I420 and possibly shared with other renderers: copy it, so
    // that the hook touches no decoder buffer.
    RTCMutableI420Buffer* copy =
        [[RTCMutableI420Buffer alloc] initWithWidth:i420.width height:i420.height];
    [RTCYUVHelper I420Rotate:i420.dataY
                  srcStrideY:i420.strideY
                        srcU:i420.dataU
                  srcStrideU:i420.strideU
                        srcV:i420.dataV
                  srcStrideV:i420.strideV
                        dstY:copy.mutableDataY
                  dstStrideY:copy.strideY
                        dstU:copy.mutableDataU
                  dstStrideU:copy.strideU
                        dstV:copy.mutableDataV
                  dstStrideV:copy.strideV
                       width:i420.width
                      height:i420.height
                        mode:RTCVideoRotation_0];
    i420 = copy;
  }
  if (i420 == nil) {
    os_unfair_lock_unlock(&_lock);
    return frame;
  }
  // Otherwise toI420 converted the buffer (a VideoToolbox NV12 pixel
  // buffer) into a new one that is ours alone.
  const int32_t verdict =
      hook(_tag, static_cast<uint32_t>(frame.timeStamp), i420.width, i420.height,
           const_cast<uint8_t*>(i420.dataY), i420.strideY,
           const_cast<uint8_t*>(i420.dataU), i420.strideU,
           const_cast<uint8_t*>(i420.dataV), i420.strideV);
  os_unfair_lock_unlock(&_lock);
  if (_logPath != nullptr)
    [self measure:started];
  if (verdict < 0)
    return nil;
  RTCVideoFrame* out = [[RTCVideoFrame alloc] initWithBuffer:i420
                                                   rotation:frame.rotation
                                                timeStampNs:frame.timeStampNs];
  out.timeStamp = frame.timeStamp;
  return out;
}

// With FLUTTER_WEBRTC_FRAME_HOOKS_LOG set (a file, or "-" for stderr): what
// the hook adds per frame (the I420 conversion or copy and the hook), every
// 300 frames.
- (void)measure:(std::chrono::steady_clock::time_point)started {
  _totalUs += std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - started)
                  .count();
  if (++_frames % 300 != 0)
    return;
  const bool toStderr = _logPath[0] == '-' && _logPath[1] == '\0';
  FILE* f = toStderr ? stderr : fopen(_logPath, "a");
  if (f == nullptr)
    return;
  fprintf(f, "[frame hooks] tag %lld: %lld frames, i420+hook %.0f us/frame\n",
          static_cast<long long>(_tag), static_cast<long long>(_frames),
          static_cast<double>(_totalUs) / static_cast<double>(_frames));
  if (!toStderr)
    fclose(f);
}

- (void)detach {
  os_unfair_lock_lock(&_lock);
  _decoded = nullptr;
  os_unfair_lock_unlock(&_lock);
  if (_transformer)
    _transformer->Detach();
}

- (void)dealloc {
  [self detach];
}

@end
