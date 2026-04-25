# OCR 입력 스크린샷 1024h 축소 검증 및 반영

## 배경

- 사용자는 스크린샷을 `1024h` 기준 비율로 축소한 뒤 `sidecar`와 `ffi`에 넣었을 때
  인식 결과가 같고 속도가 더 빠르면, 앱에도 같은 축소 기능을 넣어 달라고 요청했다.
- 현재 앱 OCR 경로에는 공용 입력 축소 단계가 있지만, 실제 런타임에서는 `resize_width_before_ocr()`
  가 `0`이라 비활성 상태다.
- 기존 회고에 따라 OCR 이미지 변환 최적화는 parity를 먼저 확인해야 하므로,
  기능 추가 전에 `sidecar`/`ffi` 비교 경로에도 동일한 전처리 옵션이 필요하다.
- 첨부 이미지는 워크스페이스 파일로 직접 접근할 수 없어, 이번 검증은 저장소 OCR fixture 기준으로 수행한다.

## 구현 계획

1. OCR 입력 이미지를 `max_height=1024` 기준으로 축소하는 공용 로직을 추가한다.
2. 축소가 일어났을 때 OCR 결과 좌표를 원본 스크린샷 좌표로 다시 복원한다.
3. `tools/scripts/ocr_sidecar_ffi.py`에 동일한 `1024h` 전처리 옵션을 넣어
   `sidecar`/`ffi` parity와 속도를 같은 조건으로 측정한다.
4. 비교 결과가 인식 동일성과 속도 개선을 만족하면 앱 OCR 경로에 기능을 연결한다.
5. 핵심 비즈니스 로직 테스트와 비교/벤치 실행 결과를 문서에 남긴다.

## 구현

- `tools/scripts/ocr_sidecar_ffi.py`에 `--resize-max-height`, `--sidecar-format`,
  `--ffi-format` 옵션을 추가했다.
- 이 옵션으로 같은 원본에서 sidecar/ffi가 동일한 축소 입력을 받도록 검증 조건을 고정했다.
- `src/services/ocr_pipeline.rs`에 공용 OCR 입력 축소 로직을 추가해,
  OCR 전 이미지를 `1024h` 기준 비율로 줄이고 좌표는 원본 기준으로 복원하도록 변경했다.
- `native/paddle_bridge/bridge.cc`의 rec batching을 sidecar와 같은 고정 배치 기준으로 맞췄다.
  - 기존 FFI는 `kRecBatchWidthBudget=3000`으로 긴 crop에서 batch를 더 쪼개고 있었다.
  - sidecar는 정렬 후 `batch_size=6` 고정 배치를 사용하므로, 같은 crop라도 배치 최대 폭이 달라져
    `max_wh_ratio`와 rec logits가 달라졌다.
- `tools/ocr_sidecar_compare/ocr_sidecar_compare.py`는 sidecar dump hook을 dump가 필요한 경우에만 설치하도록 정리했다.

## 검증 결과

- 기준 이미지: `testdata/ocr/test4.png` (`2879x1537`)
- 원본 입력 비교:
  - `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --sidecar-format bmp --ffi-format bmp`
  - 결과: sidecar `32`, ffi `32`, exact text match `32`
- 추가 원본 입력 비교:
  - `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test.png --source ch --score-thresh 0.1 --sidecar-format bmp --ffi-format bmp`
  - 결과: sidecar `7`, ffi `7`, exact text match `7`
  - `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test2.png --source ch --score-thresh 0.1 --sidecar-format bmp --ffi-format bmp`
  - 결과: sidecar `14`, ffi `14`, exact text match `14`
  - `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test3.png --source ch --score-thresh 0.1 --sidecar-format bmp --ffi-format bmp`
  - 결과: sidecar `27`, ffi `27`, exact text match `27`
- `1024h` 축소 입력 비교:
  - `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --resize-max-height 1024 --sidecar-format bmp --ffi-format bmp`
  - 결과: sidecar `37`, ffi `37`, exact text match `37`
- `1024h` 축소 입력 성능:
  - `python tools/scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --resize-max-height 1024 --sidecar-format bmp --ffi-format bmp`
  - 결과:
    - sidecar mean `8448.52ms`
    - ffi mean `7936.45ms`
    - ffi가 mean/median 모두 더 빠름
- 추가 원본 입력 성능:
  - `python tools/scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format bmp --ffi-format bmp`
  - 결과:
    - `test.png`
      - sidecar mean `2314.89ms`
      - ffi mean `2277.01ms`
      - ffi가 mean/median 모두 더 빠름
    - `test2.png`
      - sidecar mean `2966.72ms`
      - ffi mean `2964.78ms`
      - mean은 ffi가 약간 빠르지만 median은 sidecar가 `4.02ms` 더 빠름
    - `test3.png`
      - sidecar mean `5536.36ms`
      - ffi mean `5507.44ms`
      - ffi가 mean/median 모두 더 빠름

## 원인 분석

- 처음에는 `1024h` 축소 자체가 parity를 깨는 것으로 보였지만,
  `det map`과 `crop`은 sidecar/ffi가 동일했다.
- 실제 차이는 rec 단계에서 발생했다.
  - crop bitmap과 rec input tensor는 사실상 동일했다.
  - 그러나 sidecar와 ffi의 rec batch 구성 방식이 달라 batch 최대 폭이 달랐다.
- sidecar는 정렬 후 `6`개 고정 배치를 사용했고,
  FFI는 폭 예산(`3000`) 때문에 긴 항목에서 `5/4/3`개로 더 잘게 쪼갰다.
- rec 모델은 batch의 `max_wh_ratio`와 padding width에 영향을 받으므로,
  이 차이가 logits과 최종 문자열 차이로 이어졌다.
- 따라서 FFI rec batching을 sidecar와 같은 규칙으로 맞춰 parity를 복구했다.

## 판단

- 사용자가 정한 조건은 `1024h` 축소 후에도 sidecar와 ffi 인식이 같고, ffi가 더 빨라야 한다.
- 현재는 인식 parity와 속도 조건을 모두 만족한다.
- 따라서 스크린샷 `1024h` 축소를 제품 기능으로 반영했다.

## 테스트

- `cargo test --lib ocr_pipeline -- --nocapture`
- `python -m py_compile tools/ocr_sidecar_compare/ocr_sidecar_compare.py tools/scripts/ocr_sidecar_ffi.py`
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --sidecar-format bmp --ffi-format bmp`
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test.png --source ch --score-thresh 0.1 --sidecar-format bmp --ffi-format bmp`
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test2.png --source ch --score-thresh 0.1 --sidecar-format bmp --ffi-format bmp`
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test3.png --source ch --score-thresh 0.1 --sidecar-format bmp --ffi-format bmp`
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --resize-max-height 1024 --sidecar-format bmp --ffi-format bmp`
- `python tools/scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --resize-max-height 1024 --sidecar-format bmp --ffi-format bmp`
- `python tools/scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format bmp --ffi-format bmp`
