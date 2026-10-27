# Frame hooks: encoded frame observer and decoded frame hook

This branch adds two native per-frame hooks on a received video track
(Linux, macOS and Windows desktop for now):

```text
RTP -> jitter buffer -> [frame transformer] -> decoder -> [video sink] -> texture
                         encoded frame                     decoded frame hook:
                         observer: bytes, RTP              writable I420 copy +
                         timestamp, frame type,            RTP timestamp; may
                         payload type (read only)          change or drop it
```

- **Encoded frame observer**: called with every encoded frame the receiver
  hands to the decoder (for H.264/H.265 an access unit in Annex B), its RTP
  timestamp, its frame type (`0` delta, `1` key) and its RTP payload type.
  It runs in a pass-through frame transformer on the receiver, the mechanism
  the E2EE `FrameCryptor` uses; the frame goes on unchanged.
- **Decoded frame hook**: called with every decoded frame, copied to I420,
  and its RTP timestamp, before the renderer gets it. The copy is writable:
  whatever the hook draws into the planes is what the texture shows. Return
  a negative value to drop the frame (the texture keeps the previous one).

The RTP timestamp links the two: a decoded frame carries the timestamp of
the encoded frame it was decoded from, so anything learnt from the encoded
frame can be applied to exactly the picture it belongs to.

## Use

The hooks are native C functions, typically in your own shared library,
passed by address (dart:ffi `Pointer.address`):

```c
// common/cpp/include/flutter_frame_hooks.h
void on_encoded(int64_t tag, uint32_t rtp_timestamp, int32_t frame_type,
                int32_t payload_type, const uint8_t* data, size_t size);
int32_t on_decoded(int64_t tag, uint32_t rtp_timestamp,
                   int32_t width, int32_t height,
                   uint8_t* y, int32_t stride_y,
                   uint8_t* u, int32_t stride_u,
                   uint8_t* v, int32_t stride_v);
```

```dart
final lib = DynamicLibrary.open('libmy_hooks.so');
final onEncoded = lib.lookup<NativeFunction<EncodedC>>('on_encoded');
final onDecoded = lib.lookup<NativeFunction<DecodedC>>('on_decoded');

pc.onTrack = (event) async {
  if (event.track.kind != 'video') return;
  renderer.srcObject = event.streams.first;
  await FrameHooks.attach(
    renderer: renderer,
    receiver: event.receiver!,
    encodedFrameObserver: onEncoded.address, // or 0
    decodedFrameHook: onDecoded.address,     // or 0
  );
};

// Before disposing the renderer:
await FrameHooks.detach(renderer);
```

- Either hook may be `0`, not both. Without a decoded frame hook the
  renderer keeps taking frames from the track as usual.
- `tag` (the first argument of both functions) defaults to the renderer's
  texture id; pass `tag:` to choose another value.
- The functions run on libwebrtc's threads (the encoded frame observer on
  the receive thread, the decoded frame hook on the decoder's thread). They
  must not block and must not call back into Dart.
- Attaching fails with a `PlatformException` on platforms without an
  implementation, and when the receiver does not look like the libwebrtc
  build the hooks were checked against (see below).
- With `FLUTTER_WEBRTC_FRAME_HOOKS_LOG=<file>` (or `-` for stderr) the
  plugin logs what it adds per frame (the I420 copy and the hook) every 300
  frames.
- `DynamicLibrary.open` takes a `.dylib` on macOS, typically bundled in the
  app's `Contents/Frameworks`.

## How it works

The prebuilt libwebrtc (webrtc-sdk/libwebrtc) exports only its wrapper API
(`libwebrtc::*`), so `webrtc::RtpReceiverInterface::SetFrameTransformer`
and `webrtc::VideoTrackInterface::AddOrUpdateSink` cannot be linked
against. `common/cpp/include/webrtc_abi.h` mirrors the few native
interfaces involved and reaches them with virtual calls only, never passing
a `std::` type across the boundary (libwebrtc is built against Chromium's
libc++). No libwebrtc rebuild is needed.

The mirror depends on the exact libwebrtc build. `tool/check_webrtc_abi.py`
checks every vtable slot and object offset it relies on against the
unstripped `libwebrtc.so` the Linux build downloads:

```bash
tool/check_webrtc_abi.py third_party/libwebrtc/lib/libwebrtc.so
```

Run it after every libwebrtc bump (`third_party/libwebrtc_version.ini`) and
fix `webrtc_abi.h` before shipping if it reports a problem. At runtime the
plugin also checks that the objects it reads by offset belong to
libwebrtc's shared object.

### macOS

macOS uses the WebRTC-SDK framework (the `WebRTC-SDK` pod, or the
`WebRTC.xcframework` binary target with Swift Package Manager) and its
Objective-C API (`common/darwin/Classes/FlutterFrameHooks.mm`):

- **Decoded frame hook**: no native code needed. `FlutterRTCVideoRenderer`
  gets every frame of the track as an `RTCVideoFrame`, whose `timeStamp` is
  the RTP timestamp. With a hook attached it converts the buffer to I420
  first (`toI420`: a VideoToolbox NV12 pixel buffer becomes a new I420
  buffer; a buffer that is I420 already is copied, as other renderers may
  share it), calls the hook, and draws the result. The hook gets I420 as on
  Linux, so the same function works on both.
- **Encoded frame observer**: the framework has no Objective-C API for a
  frame transformer (`RTCFrameCryptor` installs its own AES-GCM one). The
  plugin reads the native `webrtc::RtpReceiverInterface` from
  `RTCRtpReceiver`'s `_nativeRtpReceiver` instance variable (its name and
  type encoding are checked at runtime through the Objective-C runtime, and
  the object's vtable must lie in the framework's image) and installs the
  same pass-through transformer as on Linux through `webrtc_abi.h`.

The framework exports the vtables of `webrtc::RtpReceiverInterface` and
`webrtc::FrameCryptorTransformer` and keeps RTTI, so the same tool checks
it, both slices (x86_64 and arm64):

```bash
tool/check_webrtc_abi.py <app>/build/macos/SourcePackages/artifacts/flutter-webrtc/WebRTC/WebRTC.xcframework/macos-arm64_x86_64/WebRTC.framework
```

(with CocoaPods: `macos/Pods/WebRTC-SDK/WebRTC.xcframework/macos-arm64_x86_64/WebRTC.framework`).
Run it after every `WebRTC-SDK` version change in `macos/flutter_webrtc.podspec`
and `macos/flutter_webrtc/Package.swift`.

### Windows

Windows x64 uses the Linux code (`common/cpp/src/flutter_frame_hooks.cc`)
against the prebuilt `libwebrtc.dll`, which is built with clang-cl and so
follows the MSVC C++ ABI, as the plugin does:

- The mirror needs no slot numbers there: MSVC lays it out as it lays out
  the real interfaces, given the same virtual functions in the same order
  and the same bases. Two interfaces are mirrored with their real secondary
  bases, because MSVC keeps an override of a secondary base's function in
  that base's vtable: `RtpReceiverInterface` (`SetFrameTransformer` is
  `FrameTransformerHost`'s) and `VideoTrackInterface` (`AddOrUpdateSink` and
  `RemoveSink` are `VideoSourceInterface`'s). `VideoSinkWants` uses a plain
  struct for its `std::vector`, whose MSVC debug layout differs.
- How `std::unique_ptr<TransformableFrameInterface>` is passed (the bare
  pointer, or the address of a temporary) is told from the first frame:
  whichever of the two is a libwebrtc object.
- `libwebrtc.dll` keeps no symbols and no PDB is published, so the ABI tool
  cannot check it. Instead, before installing anything, the plugin checks
  that the receiver and the track are objects of `libwebrtc.dll` and that
  `media_type()` and `state()` answer video and live; otherwise attaching
  fails with "unexpected libwebrtc layout". Compare the mirror with the
  headers of the new branch after every libwebrtc bump.

Checked against libwebrtc m150.7871.02 win-x64 (debug build, Windows 11):
both hooks on a 1280x720 H.264 track at 30 fps, every frame seen by the
encoded frame observer, no frames dropped, the I420 copy and the hook
about 2.3 ms a frame.

## Platforms

| Platform | State |
| :-- | :-- |
| Linux (x64) | implemented, checked against libwebrtc m150.7871.02 |
| macOS (x86_64, arm64) | implemented, checked against WebRTC-SDK 150.7871.01 |
| Linux (arm64), embedded Linux | compiled in, not yet checked (the ABI check reads x86-64 code) |
| Windows (x64) | implemented, checked at runtime against libwebrtc m150.7871.02 (see above) |
| iOS | compiled in (the darwin code is shared), not yet checked |
| Android, Web | not available |
