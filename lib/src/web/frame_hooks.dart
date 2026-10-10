import 'package:webrtc_interface/webrtc_interface.dart';

import 'rtc_video_renderer_impl.dart';

/// Per-frame hooks on a received video track: not available on the web.
class FrameHooks {
  static const int frameTypeDelta = 0;
  static const int frameTypeKey = 1;

  static Future<void> attach({
    required RTCVideoRenderer renderer,
    required RTCRtpReceiver receiver,
    int encodedFrameObserver = 0,
    int decodedFrameHook = 0,
    int? tag,
  }) =>
      Future.error(UnsupportedError('frame hooks are native only'));

  static Future<void> detach(RTCVideoRenderer renderer) async {}
}
