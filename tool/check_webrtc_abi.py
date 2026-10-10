#!/usr/bin/env python3
"""Check common/cpp/include/webrtc_abi.h against a prebuilt libwebrtc.so.

The frame hooks (common/cpp/include/flutter_frame_hooks.h) reach libwebrtc's
internal C++ interfaces through virtual calls only, using the vtable slots
and object offsets mirrored in webrtc_abi.h. This tool reads the symbol
table, the relocations and a little disassembly of an unstripped
libwebrtc.so (linux-x64, as webrtc-sdk/libwebrtc publishes it) and checks
that every slot and offset the mirror relies on is still where it says.

    tool/check_webrtc_abi.py [LIBWEBRTC_SO]

The default is third_party/libwebrtc/lib/libwebrtc.so, which the Linux
build downloads (third_party/CMakeLists.txt). Needs readelf, nm and objdump
(binutils). Exit status 0 when everything matches, 1 when something moved
(fix webrtc_abi.h before using the hooks with that libwebrtc), 2 when the
library cannot be read.

Run it for every libwebrtc bump (third_party/libwebrtc_version.ini).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Slot (in the primary vtable, address point = 0) -> a symbol substring the
# entry must resolve to, per class whose vtable is in libwebrtc.so. Slots
# whose entries were folded with unrelated code (identical code folding)
# are checked through another class that has the real name.
ABI_SLOTS = {
    "vtable for webrtc::RtpReceiverProxyWithInternal<webrtc::RtpReceiverInternal>": {
        4: "::track() const",
        8: "::media_type() const",
        18: "::SetFrameTransformer(",
    },
    "vtable for webrtc::VideoTrackProxyWithInternal<webrtc::VideoTrackInterface>": {
        9: "::AddOrUpdateSink(",
        10: "::RemoveSink(",
    },
    "vtable for webrtc::FrameCryptorTransformer": {
        0: "::AddRef() const",
        1: "::Release() const",
        4: "::Transform(std::__Cr::unique_ptr<",
        5: "::RegisterTransformedFrameCallback(",
        6: "::RegisterTransformedFrameSinkCallback(",
        7: "::UnregisterTransformedFrameCallback()",
        8: "::UnregisterTransformedFrameSinkCallback(",
    },
    "vtable for webrtc::RtpVideoStreamReceiverFrameTransformerDelegate": {
        4: "::OnTransformedFrame(std::__Cr::unique_ptr<",
        5: "::StartShortCircuiting()",
    },
    "vtable for webrtc::TransformableVideoReceiverFrame": {
        1: "::~TransformableVideoReceiverFrame()",
        2: "::GetData() const",
        4: "::GetPayloadType() const",
        7: "::GetSsrc() const",
        8: "::GetTimestamp() const",
        19: "::IsKeyFrame() const",
    },
    "vtable for libwebrtc::VideoSinkAdapter": {
        2: "::OnFrame(webrtc::VideoFrame const&)",
    },
    "vtable for webrtc::I420Buffer": {
        7: "::ToI420()",
        13: "::ChromaWidth() const",
        15: "::StrideY() const",
        16: "::StrideU() const",
        17: "::StrideV() const",
        # 18 (DataY) is folded with unrelated getters; it sits between
        # StrideV and DataU in the header.
        19: "::DataU() const",
        20: "::DataV() const",
    },
    "vtable for webrtc::NV12Buffer": {
        5: "::width() const",
        6: "::height() const",
        7: "::ToI420()",
    },
}

# Functions whose code shows a field offset: (symbol prefix, regex on the
# objdump of its first 160 bytes).
ABI_CODE = [
    # RTCRtpReceiverImpl::rtp_receiver_ at +0x10.
    ("libwebrtc::RTCRtpReceiverImpl::track() const", r"mov\s+0x10\(%rsi\),%rsi"),
    # webrtc::VideoFrame: timestamp_us_ at +0x20 and rotation_ at +0x48,
    # so timestamp_rtp_ is at +0x10 and the buffer at +0x8 (header order).
    ("libwebrtc::VideoSinkAdapter::OnFrame(webrtc::VideoFrame const&)", r"mov\s+0x20\(%r15\),%rax"),
    # The receiver frame transformer gets the unique_ptr as a bare pointer
    # (trivial ABI): it calls a virtual on the frame object directly.
    ("webrtc::FrameCryptorTransformer::Transform(std::__Cr::unique_ptr", r"mov\s+\(%r14\),%rax"),
]


def run(*cmd: str) -> str:
    return subprocess.run(cmd, capture_output=True, text=True, check=False).stdout


def check(so: Path) -> int:
    if not so.is_file():
        print(f"{so}: not found", file=sys.stderr)
        return 2
    rel = {}
    for line in run("readelf", "-rW", str(so)).splitlines():
        m = re.match(r"\s*([0-9a-f]+)\s+\S+\s+R_X86_64_RELATIVE\s+([0-9a-f]+)", line)
        if m:
            rel[int(m.group(1), 16)] = int(m.group(2), 16)
    names: dict[int, list[str]] = {}
    by: dict[str, int] = {}
    for line in run("nm", "-C", "-S", str(so)).splitlines():
        m = re.match(r"([0-9a-f]+) ([0-9a-f]+) \S (.*)", line) or re.match(r"([0-9a-f]+)() \S (.*)", line)
        if m:
            a = int(m.group(1), 16)
            names.setdefault(a, []).append(m.group(3))
            by[m.group(3)] = a
    if not by:
        print(f"{so}: no symbol table", file=sys.stderr)
        return 2
    bad = 0
    for vt, slots in ABI_SLOTS.items():
        base = by.get(vt)
        if base is None:
            print(f"MISSING {vt}")
            bad += 1
            continue
        for slot, want in slots.items():
            target = rel.get(base + 16 + 8 * slot)
            got = names.get(target, ["?"]) if target is not None else ["(no entry)"]
            ok = any(want in g for g in got)
            bad += not ok
            shown = next((g for g in got if want in g), got[0])
            print(f"{'ok ' if ok else 'BAD'} {vt.removeprefix('vtable for ')} [{slot}] {shown[:100]}")
    for sym, pattern in ABI_CODE:
        addr = next((a for n, a in by.items() if n.startswith(sym)), None)
        if addr is None:
            print(f"MISSING {sym}")
            bad += 1
            continue
        code = run("objdump", "-d", "--no-show-raw-insn", f"--start-address={addr:#x}",
                   f"--stop-address={addr + 160:#x}", str(so))
        ok = re.search(pattern, code) is not None
        bad += not ok
        print(f"{'ok ' if ok else 'BAD'} {sym[:70]} ~ /{pattern}/")
    print("ABI check:", "passed" if bad == 0 else f"{bad} problem(s)")
    return 0 if bad == 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("so", nargs="?", type=Path, default=ROOT / "third_party/libwebrtc/lib/libwebrtc.so")
    return check(ap.parse_args().so)


if __name__ == "__main__":
    sys.exit(main())
