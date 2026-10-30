"""Compare one 1920x1080 NV12 reference frame with a decoded GPU pattern frame."""
import argparse
import math
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("reference", type=Path)
parser.add_argument("decoded", type=Path)
parser.add_argument("--max-mae", type=float, default=4.0,
                    help="maximum mean absolute error per plane (8-bit code values)")
args = parser.parse_args()
expected, actual = args.reference.read_bytes(), args.decoded.read_bytes()
luma = 1920 * 1080
size = luma * 3 // 2
if len(expected) != size or len(actual) != size:
    raise SystemExit(f"need exactly one NV12 frame ({size} bytes), got {len(expected)}, {len(actual)}")
passed = True
for name, begin, end, step in (("Y", 0, luma, 1), ("U", luma, size, 2), ("V", luma+1, size, 2)):
    diffs = [int(a) - int(e) for e, a in zip(expected[begin:end:step], actual[begin:end:step])]
    errors = sorted(abs(d) for d in diffs)
    mae = sum(errors) / len(errors)
    mse = sum(d*d for d in diffs) / len(diffs)
    psnr = 10 * math.log10(255**2 / mse) if mse else float("inf")
    print(f"{name}: MAE={mae:.3f} bias={sum(diffs)/len(diffs):.3f} "
          f"p99={errors[int(len(errors)*.99)]} max={errors[-1]} PSNR={psnr:.2f} dB")
    passed &= mae <= args.max_mae
raise SystemExit(0 if passed else 1)
