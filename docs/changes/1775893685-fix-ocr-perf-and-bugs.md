# OCR 모듈 버그 수정 및 성능 개선

## 변경 항목

### 1. `mod.rs` — dead if/else 분기 제거, `filtered_boxes` 불필요 할당 제거

`recognize_boxes`의 if/else 양쪽 분기가 완전히 동일했다 (`CLS_SAMPLE_SIZE` 조건 사문화).
`filtered_boxes: Vec<&DetBox> = boxes.iter().collect()`는 `boxes`를 그대로 재사용하는 불필요한 벡터였다.

- if/else → 단순 `classify_batch` 직접 호출
- `filtered_boxes` 제거, `boxes`를 직접 이터레이션

### 2. `rec.rs:27`, `cls.rs:54` — NaN에서 패닉 가능한 `partial_cmp().unwrap()` 수정

ONNX 출력에 NaN이 섞이면 `partial_cmp`가 `None` → `unwrap()` 패닉.
`unwrap_or(std::cmp::Ordering::Equal)`로 교체.

### 3. `rec.rs` — `data.to_vec()` 불필요 복사 제거

`try_extract_tensor::<f32>()`가 반환하는 `&[f32]`를 `.to_vec()`으로 `Vec`에 복사 후 `&[..]`로 재사용.
`Vec` 할당 없이 `data` 슬라이스를 직접 `ArrayView2::from_shape`에 전달.

영향 위치: `try_batch_run`, `recognize_from_array`, `recognize` (3곳)

### 4. `det.rs`, `rec.rs`, `cls.rs` — `get_pixel()` → raw 버퍼 직접 접근

중첩 루프 + `get_pixel(x, y)` 방식은 매 픽셀마다 bounds check 포함.
`rgb.as_raw()`로 `&[u8]` 획득 후 `chunks_exact(3)`으로 순회:
bounds check 없이 연속 메모리 접근 → 컴파일러 SIMD/vectorize 최적화 유도.

rec.rs의 정규화 상수 `/ 255.0 / 0.5`를 `* (2.0/255.0)` 상수로 통합.

### 5. `cls.rs`, `rec.rs` — 폴백 경로 clone: 시그니처를 참조로 변경

`classify_from_tensor`, `recognize_from_array`가 소유권을 요구해 폴백 루프에서
매 텐서마다 `clone()`이 발생. 참조를 받도록 변경, 필요 시 함수 내부에서 clone.

## 파일별 변경

| 파일 | 변경 |
|------|------|
| `src/ocr/mod.rs` | dead if/else 제거, `filtered_boxes` 제거 |
| `src/ocr/rec.rs` | NaN 패닉 fix, to_vec() 제거, raw buffer preprocess, 참조 시그니처 |
| `src/ocr/cls.rs` | NaN 패닉 fix, raw buffer preprocess, 참조 시그니처 |
| `src/ocr/det.rs` | raw buffer preprocess |
