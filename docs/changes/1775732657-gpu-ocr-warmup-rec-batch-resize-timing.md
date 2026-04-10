# GPU OCR 성능 개선 2차: 워밍업·rec 배치·resize 필터·타이밍 로그

## 배경

cls 배치 처리 후에도 OCR이 여전히 느린 문제. 원인 파악을 위한 타이밍 로그 추가와
함께 세 가지 추가 개선을 적용했다.

## 원인 분석

| 원인 | 설명 |
|------|------|
| CUDA 첫 실행 지연 | 앱 기동 후 첫 캡처에서 CUDA 커널이 JIT 컴파일됨 (10–30초 지연 가능) |
| rec 순차 GPU 호출 | 박스 수 N만큼 rec가 개별 GPU 호출 — N번 host↔device 전송 발생 |
| Lanczos3 resize | OCR 전처리에서 가장 느린 필터 사용 (det/cls/rec 전체) |
| 타이밍 없음 | 어느 단계가 느린지 알 수 없어 최적화 방향 불명확 |

## 변경 내용

### 1. `src/ocr/mod.rs` — CUDA 워밍업 (`OcrEngine::warmup`)

`OcrEngine::new` 내에서 더미 이미지로 det/cls/rec 각 세션을 한 번씩 실행한다.
앱 기동 시 CUDA 커널이 사전 컴파일되어 첫 캡처 지연이 없어진다.

### 2. `src/ocr/rec.rs` — `recognize_batch` 추가

cls와 동일하게 N개 이미지를 `[N, 3, 48, max_W]` 배치 텐서로 묶어
한 번의 GPU 호출로 인식한다.
- 패딩 값 `-1.0` = 정규화 기준 검은색 픽셀, CTC blank 출력 유도
- 동적 배치 미지원 시 순차 처리로 자동 폴백

### 3. `src/ocr/mod.rs` — `recognize_boxes`에서 `recognize_batch` 사용

```
before: cls_batch(N) + rec(1) × N  → 1 + N GPU 호출
after:  cls_batch(N) + rec_batch(N) → 1 + 1 GPU 호출
```

### 4. `det.rs` / `cls.rs` / `rec.rs` — resize 필터 Lanczos3 → Triangle

- Lanczos3: 고품질 but CPU 집약적
- Triangle (bilinear): OCR 정밀도 영향 미미, 속도 대폭 향상

### 5. `services.rs` / `mod.rs` — 단계별 타이밍 로그

다음 단계별 소요 시간이 터미널에 출력된다:
```
[OCR] 워밍업 완료: Xms
[OCR] det 전체: Xms (N 박스, W×H)
[OCR] det 타일 3×3: Xms (N 박스)
[OCR] cls (N 박스): Xms
[OCR] rec (N 박스): Xms
```

## 테스트 결과

- `cargo test` — 43개 통과
- `cargo test --features gpu` — 44개 통과
