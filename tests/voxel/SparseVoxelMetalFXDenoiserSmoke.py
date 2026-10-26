"""Verify native reconstruction -> graph consumers -> post effects, across frame slots."""
import array
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sample = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="restir-metalfx-") as directory:
    root = Path(directory)
    for mode, scale in (("static", "1"), ("motion", "0.5"), ("crt", "1"), ("upscaler", "0.5"), ("interpolated", "0.5"), ("interpolated-motion", "0.5")):
        prefix = root / mode
        # Native authored BLAS installation is asynchronous. This test inspects
        # frames 30/31; early unknown visibility during installation is expected.
        args = [sample, "--no-terrain", "--frames", "32", "--test-static-scene",
                "--test-capture-start-frame", "30",
                "--resolution", "320x180", "--render-scale", scale,
                "--disable-vsync", "--dump-frame", str(prefix)]
        if not mode.startswith("interpolated"):
            args += ["--disable-frame-generation"]
        if mode in ("motion", "interpolated-motion"):
            args += ["--test-camera-yaw", "--test-camera-yaw-start-frame", "8"]
        if mode in ("crt", "interpolated", "interpolated-motion"):
            args += ["--crt-preset", "consumer90s", "--crt-internal", "320x180", "--crt-output", "320x180"]

        if mode == "upscaler":
            args += ["--disable-metalfx-denoiser"]
        run = subprocess.run(args, capture_output=True, text=True, timeout=120)
        log = run.stdout + run.stderr
        if "ReSTIR denoiser: MetalFX unsupported" in log:
            print("MetalFX temporal denoising unavailable on this device/configuration")
            sys.exit(77)
        if mode.startswith("interpolated") and "MetalFX frame interpolation requires macOS 26 and a supported GPU" in log:
            print("Frame interpolation unavailable; reconstruction checks passed")
            continue
        assert run.returncode == 0, log
        expected = "240x135 -> internal 320x180" if scale == "1" else "120x68 -> internal 160x90"
        assert expected in log, log
        if mode.startswith("interpolated"):
            assert "Registering pass svr.sample.MetalFXInterpolation" in log, log
            assert "Registering pass svr.sample.GeneratedCRTSignal" in log, log
        if mode != "upscaler":
            assert "ReSTIR denoiser: MetalFX temporal denoised scaler" in log, log
            assert "Registering pass svr.sample.SVGF." not in log, log
            assert "Registering pass svr.sample.LightingTemporalAccumulation" not in log, log
        else:
            assert "Registering pass svr.sample.SVGF.ATrous" in log, log
            assert "Registering pass svr.sample.LightingTemporalAccumulation" in log, log
        # Check both slots. A native readback must match the graph's independent
        # compute textureLoad, proving the native producer executes before it.
        for frame in (30, 31):
            images = []
            for suffix in (f"{frame}.rgba32f", f"presented.{frame}.rgba32f",
                           f"presented.{frame}.rgba32f.metalfx.rgba32f"):
                data = Path(f"{prefix}.{suffix}").read_bytes()
                assert struct.unpack("II", data[:8]) == (320, 180)
                rgba = array.array("f", data[8:])
                assert len(rgba) == 320 * 180 * 4
                assert all(math.isfinite(x) for x in rgba), f"{mode}: non-finite output"
                images.append(rgba)
            rgb = [x for i, x in enumerate(images[1]) if i % 4 != 3]
            assert max(rgb) > 0.1 and sum(rgb) / len(rgb) > 0.005, "Presented output is black"
            producer_delta = max(abs(x-y) for x, y in zip(images[0], images[2]))
            assert producer_delta < 1e-5, f"Graph consumer missed native output: {producer_delta}"
            effect_delta = sum(abs(x-y) for x, y in zip(images[0], images[1])) / len(images[0])
            if mode in ("crt", "interpolated", "interpolated-motion"):
                assert effect_delta > 1e-4, "Post-MetalFX CRT effect was bypassed"
            else:
                assert effect_delta < 1e-5, "Presentation unexpectedly changed reconstruction"
            print(f"MetalFX {mode} frame {frame}: native/graph delta={producer_delta:.6f}; post effect delta={effect_delta:.6f}")
            if mode.startswith("interpolated"):
                data = Path(f"{prefix}.presented.{frame}.rgba32f.interpolated.rgba32f").read_bytes()
                assert struct.unpack("II", data[:8]) == (320, 180)
                generated = array.array("f", data[8:])
                assert len(generated) == len(images[1]) and all(math.isfinite(x) for x in generated)
                assert max(generated[0::4]) > 0.1, "Generated texture is black"
                post_delta = sum(abs(x-y) for x,y in zip(generated, images[1])) / len(generated)
                raw_delta = sum(abs(x-y) for x,y in zip(generated, images[0])) / len(generated)
                assert raw_delta < post_delta, "CRT leaked into frame interpolation inputs"
                data = Path(f"{prefix}.presented.{frame}.rgba32f.generated-post.rgba32f").read_bytes()
                assert struct.unpack("II", data[:8]) == (320, 180)
                filtered = array.array("f", data[8:])
                assert all(math.isfinite(x) for x in filtered) and max(filtered[0::4]) > 0.1
                crt_delta = sum(abs(x-y) for x,y in zip(filtered, generated)) / len(filtered)
                assert crt_delta > 1e-4, "Generated frame bypassed downstream CRT"
                real_delta = sum(abs(x-y) for x,y in zip(filtered, images[1])) / len(filtered)
                assert real_delta < crt_delta, "Generated and real CRT chains disagree"
                print(f"Interpolated frame {frame}: post-effect delta={post_delta:.6f}, reconstruction delta={raw_delta:.6f}")
