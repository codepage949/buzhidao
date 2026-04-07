from pathlib import Path
import argparse
import json
import sys
import time

import httpx


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a smoke test against a running OCR server endpoint."
    )
    parser.add_argument(
        "--base-url",
        default="http://127.0.0.1:8000",
        help="OCR server base URL. Default: http://127.0.0.1:8000",
    )
    parser.add_argument(
        "--source",
        default="en",
        choices=("en", "ch"),
        help="OCR source language to test. Default: en",
    )
    parser.add_argument(
        "--image",
        default=str(Path(__file__).with_name("test.png")),
        help="Path to the image file to upload.",
    )
    parser.add_argument(
        "--wait-seconds",
        type=float,
        default=60.0,
        help="How long to wait for the server to accept requests. Default: 60",
    )
    parser.add_argument(
        "--retry-interval",
        type=float,
        default=2.0,
        help="Seconds between retries while the server is starting. Default: 2",
    )
    return parser.parse_args()


def post_infer(args: argparse.Namespace, image_path: Path) -> httpx.Response:
    with image_path.open("rb") as image_file, httpx.Client(timeout=30.0) as client:
        return client.post(
            f"{args.base_url}/infer/{args.source}",
            files={"file": (image_path.name, image_file, "image/png")},
        )

def main() -> int:
    args = parse_args()
    image_path = Path(args.image)
    if not image_path.exists():
        print(f"이미지 파일을 찾을 수 없습니다: {image_path}", file=sys.stderr)
        return 1

    deadline = time.monotonic() + args.wait_seconds
    last_error = ""

    while time.monotonic() < deadline:
        try:
            response = post_infer(args, image_path)
            break
        except httpx.HTTPError as exc:
            last_error = str(exc)
            time.sleep(args.retry_interval)
    else:
        print(
            f"엔드포인트 테스트 실패: 서버 연결 대기 시간 초과 ({last_error})",
            file=sys.stderr,
        )
        return 1

    if response.status_code != 200:
        print(
            f"엔드포인트 테스트 실패: status={response.status_code} body={response.text}",
            file=sys.stderr,
        )
        return 1

    payload = response.json()
    if not isinstance(payload, list):
        print(
            f"엔드포인트 테스트 실패: 응답이 리스트가 아닙니다: {json.dumps(payload, ensure_ascii=False)}",
            file=sys.stderr,
        )
        return 1

    print(
        json.dumps(
            {
                "base_url": args.base_url,
                "source": args.source,
                "detections": len(payload),
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
