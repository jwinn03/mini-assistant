#!/usr/bin/env python3
"""Phase 6.5 verification: diff on-device wake-word features against the
pymicro-features reference, frame by frame.

The firmware's feature_dump module (Core/Src/feature_dump.c) captures ~10 s of
the exact 16 kHz mono PCM fed to the microfrontend plus every uint16[40]
feature frame it produced. This script replays the same PCM through
pymicro-features (the same microfrontend C code microWakeWord's ecosystem
uses, compiled for the host) and compares.

Getting the dumps off the board (ST-Link, target halted or running):

    arm-none-eabi-gdb build/Debug/mini-assistant.elf
      (gdb) target extended-remote :61234        # or however ST-Link GDB server is set up
      (gdb) p feature_dump_done                  # wait for 1
      (gdb) p feature_dump_invalid               # must be 0, else rearm
      (gdb) dump binary memory pcm.bin  &feature_dump_pcm_buf   (char*)&feature_dump_pcm_buf  + feature_dump_pcm_count*2
      (gdb) dump binary memory feat.bin &feature_dump_frame_buf (char*)&feature_dump_frame_buf + feature_dump_frame_count*80
      # recapture (speak this time!): set var feature_dump_rearm = 1

  Alternative without GDB: get the buffer addresses with
  `arm-none-eabi-nm build/Debug/mini-assistant.elf | grep feature_dump`, read
  the counters with STM32CubeProgrammer, then
  `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -u <addr> <bytes> out.bin`.

Then:

    python tools/compare_features.py pcm.bin feat.bin

Pass criterion: every bin of every frame within a few raw LSBs (host libm vs
ARM newlib can differ by an ulp when the init code quantizes the window /
filterbank / PCAN lookup tables, so occasional +-1..2 LSB is expected;
anything larger means the pipelines genuinely disagree).

Requires: pip install pymicro-features numpy
"""

import argparse
import sys

import numpy as np

try:
    from pymicro_features import MicroFrontend
except ImportError:
    sys.exit("pymicro-features is not installed: pip install pymicro-features")

SAMPLE_RATE = 16000
HOP_SAMPLES = 160          # 10 ms
N_CHANNELS = 40
RAW_PER_FLOAT = 25.6       # microfrontend uint16 -> trained float domain


def reference_features(pcm: np.ndarray) -> np.ndarray:
    """Run PCM (int16) through pymicro-features in 10 ms hops, mirroring the
    firmware's micro_features_process_hop loop (consume the whole hop, collect
    a frame whenever one is emitted). pymicro-features hardcodes the same
    FrontendConfig micro_features.c uses and returns features scaled by
    0.0390625 (= raw uint16 / 25.6)."""
    frontend = MicroFrontend()
    frames = []
    n_hops = len(pcm) // HOP_SAMPLES
    for h in range(n_hops):
        chunk = pcm[h * HOP_SAMPLES : (h + 1) * HOP_SAMPLES].tobytes()
        while chunk:
            result = frontend.process_samples(chunk)
            chunk = chunk[result.samples_read * 2 :]
            if result.features:
                frames.append(result.features)
            if result.samples_read == 0:
                break
    return np.asarray(frames, dtype=np.float64)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pcm_bin", help="feature_dump_pcm_buf dump (int16 LE mono 16 kHz)")
    ap.add_argument("feat_bin", help="feature_dump_frame_buf dump (uint16 LE, frames x 40)")
    ap.add_argument("--tolerance-lsb", type=float, default=3.0,
                    help="max per-bin |diff| in raw microfrontend LSBs (default 3)")
    args = ap.parse_args()

    pcm = np.fromfile(args.pcm_bin, dtype="<i2")
    device_raw = np.fromfile(args.feat_bin, dtype="<u2")
    if device_raw.size % N_CHANNELS != 0:
        sys.exit(f"feat.bin size {device_raw.size} u16s is not a multiple of {N_CHANNELS}"
                 " — wrong dump length?")
    device = device_raw.reshape(-1, N_CHANNELS).astype(np.float64)

    print(f"PCM: {len(pcm)} samples ({len(pcm)/SAMPLE_RATE:.2f} s), "
          f"device frames: {len(device)}")

    ref = reference_features(pcm)
    if ref.size == 0:
        sys.exit("reference produced no frames — PCM dump too short?")

    # pymicro-features historically returns features already scaled into the
    # trained float domain (raw/25.6, values ~[0, 26]); detect and normalize
    # to the raw uint16 domain so tolerances read in LSBs either way.
    if np.median(np.abs(ref)) < 100.0:
        ref = ref * RAW_PER_FLOAT
        print("reference detected in float domain — rescaled by 25.6 to raw LSBs")

    if len(ref) != len(device):
        print(f"WARNING: frame count mismatch (reference {len(ref)} vs device "
              f"{len(device)}) — comparing the overlap. A mismatch beyond the "
              f"trailing partial hop suggests dump misalignment.")
    n = min(len(ref), len(device))
    diff = np.abs(ref[:n] - device[:n])

    worst = np.unravel_index(np.argmax(diff), diff.shape)
    print(f"frames compared : {n}")
    print(f"max |diff|      : {diff.max():.2f} LSB  (frame {worst[0]}, bin {worst[1]})")
    print(f"mean |diff|     : {diff.mean():.4f} LSB")
    print(f"bins > {args.tolerance_lsb:g} LSB : {(diff > args.tolerance_lsb).sum()}"
          f" of {diff.size}")

    if diff.max() <= args.tolerance_lsb:
        print("PASS — on-device front end matches the training-time reference.")
        return 0

    # Show a few of the worst frames to aid debugging.
    frame_max = diff.max(axis=1)
    for idx in np.argsort(frame_max)[::-1][:5]:
        print(f"  frame {idx}: max {frame_max[idx]:.1f} LSB, "
              f"t={idx*10} ms, worst bin {int(np.argmax(diff[idx]))}")
    print("FAIL — pipelines disagree beyond tolerance.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
