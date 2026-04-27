import argparse
import json
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path


BENCH_PREFIX = "[FFI_BENCH] "


@dataclass(frozen=True)
class BenchResult:
    image: str
    detection_count: int
    elapsed_ms: tuple[float, ...]

    @property
    def median_ms(self) -> float:
        return float(statistics.median(self.elapsed_ms))


def configure_utf8_stdio() -> None:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def parse_bench_output(text: str) -> dict[str, BenchResult]:
    results: dict[str, BenchResult] = {}
    for line in text.splitlines():
        if BENCH_PREFIX not in line:
            continue
        payload_text = line.split(BENCH_PREFIX, 1)[1]
        payload = json.loads(payload_text)
        image = str(payload["image"])
        elapsed = tuple(float(value) for value in payload["elapsed_ms"])
        if not elapsed:
            raise ValueError(f"elapsed_ms가 비어 있습니다: {image}")
        results[image] = BenchResult(
            image=image,
            detection_count=int(payload["detection_count"]),
            elapsed_ms=elapsed,
        )
    if not results:
        raise ValueError("FFI benchmark 결과를 찾지 못했습니다.")
    return results


def load_bench_results(path: Path) -> dict[str, BenchResult]:
    return parse_bench_output(path.read_text(encoding="utf-8"))


def compare_bench_results(
    baseline: dict[str, BenchResult],
    current: dict[str, BenchResult],
    max_median_ratio: float,
) -> list[str]:
    failures: list[str] = []
    for image, base in baseline.items():
        now = current.get(image)
        if now is None:
            failures.append(f"{image}: 현재 결과가 없습니다.")
            continue
        if now.detection_count != base.detection_count:
            failures.append(
                f"{image}: detection count 변경 baseline={base.detection_count}, current={now.detection_count}"
            )
            continue
        allowed = base.median_ms * max_median_ratio
        if now.median_ms > allowed:
            failures.append(
                f"{image}: median latency 증가 baseline={base.median_ms:.3f}ms, "
                f"current={now.median_ms:.3f}ms, allowed={allowed:.3f}ms"
            )
    for image in current:
        if image not in baseline:
            failures.append(f"{image}: baseline에 없는 현재 결과입니다.")
    return failures


def main() -> int:
    configure_utf8_stdio()
    parser = argparse.ArgumentParser(description="FFI OCR benchmark 결과 회귀를 검사합니다.")
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--current", required=True, type=Path)
    parser.add_argument("--max-median-ratio", type=float, default=1.15)
    args = parser.parse_args()

    baseline = load_bench_results(args.baseline)
    current = load_bench_results(args.current)
    failures = compare_bench_results(baseline, current, args.max_median_ratio)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("FFI benchmark guard 통과")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
