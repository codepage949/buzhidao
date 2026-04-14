# services.rs 모듈 분리 및 ocr_server.py 정리

## 배경

`app/src/services.rs`가 770줄로 비대해졌고 이질적 관심사가 한 파일에 섞여 있다.

- 화면 캡처 (xcap, Linux ScreenCast/PipeWire)
- OCR 파이프라인 (resize, run_ocr, 결과 좌표 변환)
- AI 게이트웨이 호출

또한 `OcrResultPayload`의 포인트 순회 패턴이 `offset_ocr_result`, `scale_ocr_result`에서 4번 반복된다.

## 변경

### 1. `services`를 디렉토리 모듈로 분할

```
app/src/services/
    mod.rs         # pub(crate) re-export만 수행
    capture.rs     # CaptureInfo, capture_screen, xcap, Linux ScreenCast/PipeWire 전체
    ocr_pipeline.rs# OcrResultPayload, run_ocr, resize, crop/offset/scale
    ai.rs          # call_ai + Chat 구조체
```

외부(`lib.rs`) 호출부는 기존과 동일한 심볼을 `crate::services::{...}`로 그대로 사용.

### 2. 포인트 좌표 변환 중복 제거

`offset_ocr_result`와 `scale_ocr_result`에서 detections/debug_detections 모든 포인트를 순회하는 코드를 `for_each_point` 헬퍼로 통합.

```rust
fn for_each_point(payload: &mut OcrResultPayload, mut f: impl FnMut(&mut [f64; 2])) {
    for (polygon, _) in &mut payload.detections {
        for p in polygon { f(p); }
    }
    for (polygon, _, _, _) in &mut payload.debug_detections {
        for p in polygon { f(p); }
    }
}
```

### 3. `lib.rs` OCR 결과 emit 중복 제거

`handle_prtsc`와 `run_region_ocr` 두 곳에서 동일한 패턴으로 overlay에 `ocr_result`/`ocr_error`를 emit한다. `emit_ocr_outcome(&Window, Result<OcrResultPayload, String>)` 헬퍼로 통합.

## ocr_server.py 정리

### 4. `WARMUP_IMAGE` 바이트 리터럴 압축

58줄짜리 `bytes([...])` 리터럴(1×1 24bpp BMP 헤더)을 `bytes.fromhex("...")` 한 줄로 교체. 동작 동일.

### 5. JSON 출력 중복 제거

`print(json.dumps({...}, ensure_ascii=False), flush=True)` 패턴이 서버 루프·단발 실행에서 4회 반복 → `emit(obj)` 헬퍼로 통합.

### 6. 요청 파싱 분리

`run_server` 루프의 파싱·검증 로직을 `parse_request(line) -> tuple[int, str, str, float]`로 추출해 루프 본문을 납작하게 정리.

## 검증

- `cargo test -p app` (기존 테스트 모두 통과해야 함)
- `cargo check` 통과
- `python -m py_compile ocr_server/ocr_server.py`
- WARMUP 페이로드 길이/내용 불변 확인
