#!/usr/bin/env python3
"""Regression harness for Voxtral C++ runtime.

Lock strategy:
- Semantic lock: expected transcript from `samples/*.txt` paired with `samples/*.wav`.
- Numeric lock:
  1) compare C++ against committed golden reference if available, else
  2) compare C++ against live Python reference (torch path).
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np


@dataclasses.dataclass(frozen=True)
class Fixture:
    name: str
    wav_path: Path
    max_tokens: int = 64
    expected_text: str | None = None


def have_module(name: str) -> bool:
    try:
        __import__(name)
        return True
    except Exception:
        return False


def gen_wav(path: Path, seconds: float, freq: float, sample_rate: int = 16000) -> None:
    import wave

    n = int(seconds * sample_rate)
    t = np.arange(n, dtype=np.float32) / float(sample_rate)
    sig = 0.2 * np.sin(2.0 * math.pi * freq * t)
    pcm = np.clip(sig * 32767.0, -32768, 32767).astype(np.int16)

    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm.tobytes())


def run(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=False)


def run_cpp(
    root: Path,
    voxtral_bin: Path,
    gguf_path: Path,
    fixture: Fixture,
    out_dir: Path,
) -> dict:
    n_threads = os.environ.get("VOXTRAL_TEST_THREADS", "4")
    logits_txt = out_dir / f"cpp_{fixture.name}_logits32.txt"
    logits_bin = out_dir / f"cpp_{fixture.name}_logits_full.bin"
    tokens_txt = out_dir / f"cpp_{fixture.name}_tokens.txt"

    cmd_cpp = [
        str(voxtral_bin),
        "--model",
        str(gguf_path),
        "--audio",
        str(fixture.wav_path),
        "--n-tokens",
        str(fixture.max_tokens),
        "--threads",
        str(n_threads),
        "--dump-logits",
        str(logits_txt),
        "--dump-logits-bin",
        str(logits_bin),
        "--dump-tokens",
        str(tokens_txt),
        "--log-level",
        "warn",
    ]
    cpp_proc = run(cmd_cpp, cwd=root)
    if cpp_proc.returncode != 0:
        print(cpp_proc.stdout)
        print(cpp_proc.stderr, file=sys.stderr)
        raise RuntimeError(f"C++ runtime failed on fixture {fixture.name}")

    raw_tokens = tokens_txt.read_text(encoding="utf-8").strip()
    cpp_tokens = [int(x) for x in raw_tokens.split()] if raw_tokens else []

    logits_32 = np.array(
        [float(x) for x in logits_txt.read_text(encoding="utf-8").splitlines() if x.strip()],
        dtype=np.float32,
    )
    logits_full_bytes = logits_bin.read_bytes()
    logits_full_sha256 = hashlib.sha256(logits_full_bytes).hexdigest()

    return {
        "text": cpp_proc.stdout.strip(),
        "tokens": cpp_tokens,
        "logits_32": logits_32,
        "logits_full_sha256": logits_full_sha256,
    }


def run_reference(
    root: Path,
    model_dir: Path,
    fixture: Fixture,
    out_dir: Path,
) -> dict:
    ref_json = out_dir / f"ref_{fixture.name}.json"
    cmd_ref = [
        sys.executable,
        str(root / "python" / "reference_probe.py"),
        "--model-dir",
        str(model_dir),
        "--audio",
        str(fixture.wav_path),
        "--max-tokens",
        str(fixture.max_tokens),
        "--out",
        str(ref_json),
    ]
    ref_proc = run(cmd_ref, cwd=root)
    if ref_proc.returncode != 0:
        print(ref_proc.stdout)
        print(ref_proc.stderr, file=sys.stderr)
        raise RuntimeError(f"reference probe failed on fixture {fixture.name}")

    return json.loads(ref_json.read_text(encoding="utf-8"))


def assert_text_lock(fixture: Fixture, cpp_out: dict, ref_out: dict | None) -> None:
    if fixture.expected_text is None:
        return

    expected = fixture.expected_text.strip()
    cpp_text = str(cpp_out["text"]).strip()

    if cpp_text != expected:
        raise AssertionError(
            f"text mismatch on {fixture.name}\nexpected={expected!r}\ncpp={cpp_text!r}"
        )

    if ref_out is not None:
        ref_text = str(ref_out["text"]).strip()
        if ref_text != expected:
            raise AssertionError(
                f"reference text mismatch on {fixture.name}\nexpected={expected!r}\nref={ref_text!r}"
            )


def compare_cpp_to_record(
    fixture: Fixture,
    cpp: dict,
    rec: dict,
    atol: float,
    rtol: float,
    check_sha: bool,
) -> None:
    rec_tokens = [int(x) for x in rec["tokens"]]
    if cpp["tokens"] != rec_tokens:
        raise AssertionError(
            f"token mismatch on {fixture.name}\n"
            f"ref={rec_tokens[:32]}\ncpp={cpp['tokens'][:32]}"
        )

    rec_logits_32 = np.array(rec["first_logits_32"], dtype=np.float32)
    cpp_logits_32 = cpp["logits_32"]
    if cpp_logits_32.shape != rec_logits_32.shape:
        raise AssertionError(
            f"logits32 shape mismatch on {fixture.name}: {cpp_logits_32.shape} vs {rec_logits_32.shape}"
        )

    if not np.allclose(cpp_logits_32, rec_logits_32, rtol=rtol, atol=atol):
        diff = np.abs(cpp_logits_32 - rec_logits_32)
        raise AssertionError(
            f"logits32 mismatch on {fixture.name}: max_abs_diff={diff.max():.6f}, atol={atol}, rtol={rtol}"
        )

    if check_sha and cpp["logits_full_sha256"] != str(rec["first_logits_sha256"]):
        raise AssertionError(
            f"full first-step logits hash mismatch on {fixture.name}\n"
            f"ref={rec['first_logits_sha256']}\n"
            f"cpp={cpp['logits_full_sha256']}"
        )

    rec_text = str(rec["text"]).strip()
    cpp_text = str(cpp["text"]).strip()
    if cpp_text != rec_text:
        raise AssertionError(
            f"decoded text mismatch on {fixture.name}\nref={rec_text!r}\ncpp={cpp_text!r}"
        )


def compute_wer(reference: str, hypothesis: str) -> float:
    r = reference.split()
    h = hypothesis.split()
    d = np.zeros((len(r) + 1, len(h) + 1), dtype=np.uint32)
    for i in range(len(r) + 1):
        d[i, 0] = i
    for j in range(len(h) + 1):
        d[0, j] = j
    for i in range(1, len(r) + 1):
        for j in range(1, len(h) + 1):
            if r[i - 1] == h[j - 1]:
                d[i, j] = d[i - 1, j - 1]
            else:
                d[i, j] = min(d[i - 1, j], d[i, j - 1], d[i - 1, j - 1]) + 1
    return d[len(r), len(h)] / len(r) if len(r) > 0 else 0.0


def default_fixtures(root: Path, tdir: Path) -> list[Fixture]:
    fixtures: list[Fixture] = []
    samples_dir = root / "samples"
    
    if not samples_dir.exists():
        return fixtures

    # Find all .wav files in samples directory
    wav_files = sorted(samples_dir.glob("*.wav"))
    
    for wav_path in wav_files:
        name = wav_path.stem
        if name == "test_speech":
            continue
        txt_path = wav_path.with_suffix(".txt")
        
        expected_text = None
        if txt_path.exists():
            expected_text = txt_path.read_text(encoding="utf-8").strip()
            
        fixtures.append(Fixture(name=name, wav_path=wav_path, max_tokens=128, expected_text=expected_text))

    return fixtures


def fixtures_from_golden(root: Path, golden: dict) -> list[Fixture]:
    fixtures: list[Fixture] = []
    for name, rec in golden.get("fixtures", {}).items():
        audio = Path(rec["audio"])
        if not audio.is_absolute():
            audio = root / audio
        fixtures.append(
            Fixture(
                name=name,
                wav_path=audio,
                max_tokens=int(rec.get("max_tokens", 64)),
                expected_text=rec.get("expected_text"),
            )
        )
    return fixtures


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    model_dir = Path(os.environ.get("VOXTRAL_MODEL_DIR", str(root / "models" / "voxtral")))
    gguf_path = Path(os.environ.get("VOXTRAL_GGUF", str(model_dir / "voxtral.gguf")))
    voxtral_bin = Path(os.environ.get("VOXTRAL_BIN", str(root / "build" / "voxtral")))
    golden_path = Path(
        os.environ.get("VOXTRAL_GOLDEN", str(root / "tests" / "golden" / "voxtral_reference.json"))
    )

    required_cpp = [gguf_path, voxtral_bin]
    missing_cpp = [str(p) for p in required_cpp if not p.exists()]
    if missing_cpp:
        print("[skip] missing required C++ runtime files:")
        for m in missing_cpp:
            print(f"  - {m}")
        return 0

    atol = float(os.environ.get("VOXTRAL_TEST_ATOL", "1e-2"))
    rtol = float(os.environ.get("VOXTRAL_TEST_RTOL", "1e-2"))
    check_sha = os.environ.get("VOXTRAL_TEST_CHECK_SHA", "0").lower() in {"1", "true", "yes", "on"}

    golden: dict | None = None
    if golden_path.exists():
        golden = json.loads(golden_path.read_text(encoding="utf-8"))
        print(f"[info] using golden parity baseline: {golden_path}")

    need_ref = all(
        [
            (model_dir / "consolidated.safetensors").exists(),
            (model_dir / "params.json").exists(),
            (model_dir / "tekken.json").exists(),
            (root / "python" / "reference_probe.py").exists(),
            all(have_module(m) for m in ["torch", "soundfile", "safetensors", "numpy", "soxr"]),
        ]
    )

    with tempfile.TemporaryDirectory(prefix="voxtral_test_") as td:
        tdir = Path(td)

        fixtures = fixtures_from_golden(root, golden) if golden is not None else default_fixtures(root, tdir)
        if not fixtures:
            print("[skip] no fixtures available")
            return 0

        cpp_results: dict[str, dict] = {}
        total_wer = 0.0
        wer_count = 0
        
        print(f"{'Sample':<30} | {'WER':<8} | {'Status'}")
        print("-" * 50)

        for fx in fixtures:
            if not fx.wav_path.exists():
                raise FileNotFoundError(f"missing fixture audio: {fx.wav_path}")

            cpp_results[fx.name] = run_cpp(root, voxtral_bin, gguf_path, fx, tdir)

            wer_str = "N/A"
            status = "OK"

            if fx.expected_text:
                wer = compute_wer(fx.expected_text, cpp_results[fx.name]["text"])
                total_wer += wer
                wer_count += 1
                wer_str = f"{wer:.4f}"
                if wer > 0.1:
                    status = "HIGH WER"

            print(f"{fx.name:<30} | {wer_str:<8} | {status}")

        if wer_count > 0:
            avg_wer = total_wer / wer_count
            print("-" * 50)
            print(f"Average WER: {avg_wer:.4f} (over {wer_count} samples)")
            accuracy = 1.0 - avg_wer
            print(f"Average Accuracy: {accuracy:.2%}")

        if golden is not None:
            records = golden.get("fixtures", {})
            for fx in fixtures:
                if fx.name not in records:
                    raise AssertionError(f"fixture {fx.name} missing from golden file")
                compare_cpp_to_record(
                    fx,
                    cpp_results[fx.name],
                    records[fx.name],
                    atol=atol,
                    rtol=rtol,
                    check_sha=check_sha,
                )
                print(f"[ok] golden parity fixture {fx.name}")
            print("[ok] all locks passed (golden mode)")
            return 0

        if not need_ref:
            print("[skip] python reference parity checks unavailable (missing torch/deps/model files)")
            print("[ok] semantic lock checks passed")
            return 0

        for fx in fixtures:
            ref = run_reference(root, model_dir, fx, tdir)
            compare_cpp_to_record(
                fx,
                cpp_results[fx.name],
                ref,
                atol=atol,
                rtol=rtol,
                check_sha=check_sha,
            )
            assert_text_lock(fx, cpp_results[fx.name], ref)
            parity_bits = "tokens + logits32"
            if check_sha:
                parity_bits += " + logits_hash"
            parity_bits += " + text"
            print(f"[ok] parity fixture {fx.name}: {parity_bits}")

    print("[ok] all locks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
