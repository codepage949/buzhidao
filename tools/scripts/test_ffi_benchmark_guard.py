import sys
import unittest

from tools.scripts.ffi_benchmark_guard import (
    compare_bench_results,
    configure_utf8_stdio,
    parse_bench_output,
)


class FfiBenchmarkGuardTest(unittest.TestCase):
    def test_stdio를_utf8로_재설정한다(self):
        class FakeStream:
            def __init__(self):
                self.calls = []

            def reconfigure(self, **kwargs):
                self.calls.append(kwargs)

        stdout = FakeStream()
        stderr = FakeStream()
        original_stdout = sys.stdout
        original_stderr = sys.stderr
        try:
            sys.stdout = stdout
            sys.stderr = stderr

            configure_utf8_stdio()
        finally:
            sys.stdout = original_stdout
            sys.stderr = original_stderr

        self.assertEqual(stdout.calls, [{"encoding": "utf-8", "errors": "replace"}])
        self.assertEqual(stderr.calls, [{"encoding": "utf-8", "errors": "replace"}])

    def test_ffi_bench_json_line을_파싱한다(self):
        results = parse_bench_output(
            'noise\n'
            '[FFI_BENCH] {"image":"testdata/ocr/test.png","detection_count":7,'
            '"elapsed_ms":[10.0,12.0,11.0]}\n'
        )

        result = results["testdata/ocr/test.png"]
        self.assertEqual(result.detection_count, 7)
        self.assertEqual(result.median_ms, 11.0)

    def test_detection_count가_바뀌면_실패한다(self):
        baseline = parse_bench_output(
            '[FFI_BENCH] {"image":"a.png","detection_count":7,"elapsed_ms":[10,11,12]}'
        )
        current = parse_bench_output(
            '[FFI_BENCH] {"image":"a.png","detection_count":6,"elapsed_ms":[9,10,11]}'
        )

        failures = compare_bench_results(baseline, current, max_median_ratio=1.15)

        self.assertEqual(len(failures), 1)
        self.assertIn("detection count", failures[0])

    def test_latency_median이_허용비율_안이면_통과한다(self):
        baseline = parse_bench_output(
            '[FFI_BENCH] {"image":"a.png","detection_count":7,"elapsed_ms":[100,110,120]}'
        )
        current = parse_bench_output(
            '[FFI_BENCH] {"image":"a.png","detection_count":7,"elapsed_ms":[110,120,125]}'
        )

        failures = compare_bench_results(baseline, current, max_median_ratio=1.15)

        self.assertEqual(failures, [])

    def test_latency_median이_허용비율을_넘으면_실패한다(self):
        baseline = parse_bench_output(
            '[FFI_BENCH] {"image":"a.png","detection_count":7,"elapsed_ms":[100,110,120]}'
        )
        current = parse_bench_output(
            '[FFI_BENCH] {"image":"a.png","detection_count":7,"elapsed_ms":[130,140,150]}'
        )

        failures = compare_bench_results(baseline, current, max_median_ratio=1.15)

        self.assertEqual(len(failures), 1)
        self.assertIn("median latency", failures[0])


if __name__ == "__main__":
    unittest.main()
