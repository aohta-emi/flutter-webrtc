# Frame hooks: encoded frame observer and decoded frame hook

This branch adds two native per-frame hooks on a received video track
(Linux desktop for now):

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
  sink logs what it adds per frame (the I420 copy and the hook) every 300
  frames.

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

## Platforms

| Platform | State |
| :-- | :-- |
| Linux (x64) | implemented, checked against libwebrtc m150.7871.02 |
| Linux (arm64), embedded Linux | compiled in, not yet checked (the ABI check reads x86-64 code) |
| Windows | not yet: MSVC lays out vtables differently; attaching fails |
| macOS, iOS | not yet: the method calls are not handled |
| Android, Web | not available |
