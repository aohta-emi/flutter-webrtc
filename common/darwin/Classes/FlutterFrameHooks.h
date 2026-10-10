// Per-frame hooks on a received video track: an encoded frame observer and
// a decoded frame hook (Documentation/frame-hooks.md). The darwin side of
// common/cpp/include/flutter_frame_hooks.h, with the same C signatures.
//
// * The encoded frame observer is called with every received encoded frame
//   (an H.264 access unit in Annex B, ...), its RTP timestamp, frame type
//   and RTP payload type, from a pass-through frame transformer on the
//   receiver (the mechanism RTCFrameCryptor uses). The WebRTC framework has
//   no Objective-C API for it: the transformer is installed on the native
//   webrtc::RtpReceiverInterface through webrtc_abi.h.
// * The decoded frame hook is called by FlutterRTCVideoRenderer with every
//   frame it gets, as a writable I420 copy, and the frame's RTP timestamp
//   (RTCVideoFrame.timeStamp), before it is drawn into the texture.
#import <Foundation/Foundation.h>
#import <WebRTC/WebRTC.h>

#include <stddef.h>
#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN

// Called with every received encoded frame. `data` is valid for the call
// only and must not be written to. `frame_type`: 0 delta, 1 key.
typedef void (*FlutterEncodedFrameObserver)(int64_t tag,
                                            uint32_t rtp_timestamp,
                                            int32_t frame_type,
                                            int32_t payload_type,
                                            const uint8_t* data,
                                            size_t size);

// Called with every decoded frame, an I420 copy the hook may write to
// (`height` rows of Y, `(height + 1) / 2` rows of U and V). Returns a
// negative value to drop the frame, anything else to render it.
typedef int32_t (*FlutterDecodedFrameHook)(int64_t tag,
                                           uint32_t rtp_timestamp,
                                           int32_t width,
                                           int32_t height,
                                           uint8_t* y,
                                           int32_t stride_y,
                                           uint8_t* u,
                                           int32_t stride_u,
                                           uint8_t* v,
                                           int32_t stride_v);

@interface FlutterFrameHooks : NSObject

// Installs the hooks (either may be NULL, not both) on `receiver`. Returns
// nil and sets `error` when the receiver is not a video receiver or does
// not look like the WebRTC framework build webrtc_abi.h was checked
// against.
+ (nullable instancetype)attachToReceiver:(RTCRtpReceiver*)receiver
                                      tag:(int64_t)tag
                     encodedFrameObserver:(nullable FlutterEncodedFrameObserver)encoded
                         decodedFrameHook:(nullable FlutterDecodedFrameHook)decoded
                                    error:(NSString* _Nullable* _Nonnull)error;

@property(nonatomic, readonly) BOOL hasDecodedFrameHook;

// Runs the decoded frame hook on `frame`: returns the frame to render (a
// new one, with the hook's changes), or nil when the hook drops it. Called
// on the thread that delivers the track's frames.
- (nullable RTCVideoFrame*)processDecodedFrame:(RTCVideoFrame*)frame;

// Stops calling the hooks; the transformer stays on the receiver as a plain
// pass-through (libwebrtc keeps its own reference). Once this returns,
// neither hook is running or called again.
- (void)detach;

@end

NS_ASSUME_NONNULL_END
