import 'package:webrtc_interface/webrtc_interface.dart';

import 'rtc_rtp_receiver_impl.dart';
import 'rtc_video_renderer_impl.dart';
import 'utils.dart';

/// Per-frame hooks on a received video track: an encoded frame observer and
/// a decoded frame hook (Documentation/frame-hooks.md). Linux and macOS for
/// now.
///
/// Both are addresses of native C functions, for example from dart:ffi
/// `Pointer.address` of a function in a native library, with the signatures
/// in `common/cpp/include/flutter_frame_hooks.h` (the same on all
/// platforms):
///
/// * the encoded frame observer gets every encoded frame the receiver gets
///   (its bytes, RTP timestamp, frame type and RTP payload type), which then
///   goes on to the decoder unchanged;
/// * the decoded frame hook gets every decoded frame as a writable I420
///   copy, with its RTP timestamp, before the renderer shows it. What it
///   changes is what the renderer shows.
///
/// The functions are called on libwebrtc's threads, not on the Dart
/// isolate; they must not block or call back into Dart.
class FrameHooks {
  /// The `frame_type` the encoded frame observer gets.
  static const int frameTypeDelta = 0;
  static const int frameTypeKey = 1;

  /// Attaches the hooks (either address may be 0, not both) to [receiver],
  /// whose track [renderer] shows. [tag] is passed to both functions as
  /// their first argument; it defaults to the renderer's texture id.
  /// Attaching again replaces the previous hooks. Throws a
  /// PlatformException when the platform has no implementation or the
  /// libwebrtc build does not match the one the hooks were checked against.
  static Future<void> attach({
    required RTCVideoRenderer renderer,
    required RTCRtpReceiver receiver,
    int encodedFrameObserver = 0,
    int decodedFrameHook = 0,
    int? tag,
  }) async {
    if (encodedFrameObserver == 0 && decodedFrameHook == 0) {
      throw ArgumentError('FrameHooks.attach: no hook given');
    }
    final native = receiver as RTCRtpReceiverNative;
    await WebRTC.invokeMethod(
      'videoRendererAttachFrameHooks',
      <String, dynamic>{
        'textureId': renderer.textureId,
        'peerConnectionId': native.peerConnectionId,
        'rtpReceiverId': native.receiverId,
        'tag': tag ?? renderer.textureId,
        'encodedFrameObserver': encodedFrameObserver,
        'decodedFrameHook': decodedFrameHook,
      },
    );
  }

  /// Removes the hooks; [renderer] shows the track as before.
  static Future<void> detach(RTCVideoRenderer renderer) async {
    await WebRTC.invokeMethod(
      'videoRendererDetachFrameHooks',
      <String, dynamic>{'textureId': renderer.textureId},
    );
  }
}
