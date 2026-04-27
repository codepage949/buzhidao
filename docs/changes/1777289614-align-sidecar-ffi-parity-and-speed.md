# Sidecar와 FFI OCR parity 및 속도 정렬

## 목표

- sidecar와 현재 FFI OCR의 인식 결과를 코드 레벨에서 다시 일치시킨다.
- 결과 parity를 맞춘 뒤 같은 입력 조건에서 FFI가 sidecar보다 느리지 않게 만든다.
- 검출 수와 텍스트가 다르면 속도 비교가 의미 없으므로, 먼저 parity를 회복한다.

## 현재 관찰

- `ac0163a` 최신 리팩터링과 직전 커밋 모두에서 같은 불일치가 재현된다.
- 따라서 최신 파일 분리 리팩터링만의 회귀는 아니다.
- `--ffi-mode pipeline --sidecar-format png --ffi-format png` 조건에서:
  - `test.png`: sidecar 7, FFI 7
  - `test2.png`: sidecar 11, FFI 10
  - `test3.png`: sidecar 23, FFI 23
  - `test4.png`: sidecar 45, FFI 34
- 불일치는 앱 grouping 이후가 아니라 OCR detection 결과 단계에서 이미 발생한다.
- det box와 rec candidate 수는 같아도 최종 텍스트가 달라지는 케이스가 있었다.

## 구현 계획

1. sidecar와 FFI가 같은 이미지, 같은 resize, 같은 모델 설정을 사용하는지 확인한다.
2. detection 단계 출력 수와 polygon이 언제 갈라지는지 확인한다.
3. crop/cls/rec 입력이 sidecar와 FFI에서 같은지 비교한다.
4. 코드 레벨 차이가 발견되면 parity를 우선해 수정한다.
5. `compare`로 검출 수와 텍스트 일치를 확인한다.
6. parity 회복 뒤 `benchmark`로 sidecar 대비 FFI 속도를 확인한다.

## 원인

- rec 모델 자동 선택에서 언어 선호 조건이 모델 family 선호 조건보다 먼저 적용되어,
  FFI가 `PP-OCRv5_server_rec` 대신 더 오래된 `server_rec` 계열 모델을 선택할 수 있었다.
- detection 좌표를 원본 크기로 되돌릴 때 C++ `std::round`와 Python `round`의 `.5` 처리 방식이 달랐다.
  sidecar 경로는 Python 반올림 규칙에 따라 half-to-even이 적용되고, FFI는 half-away-from-zero가 적용되어
  일부 crop이 1px 달라졌다.
- OpenCV 사용 가능 빌드에서는 crop box 계산도 sidecar와 같은 `cv::minAreaRect`를 직접 사용해야 한다.

## 변경

- rec 모델 후보 선택에서 명시적인 모델 family를 언어 선호보다 먼저 적용하도록 조정했다.
- det scaled box 좌표 반올림을 Python과 같은 half-to-even 규칙으로 맞췄다.
- OpenCV crop 경로에서 sidecar와 같은 `cv::minAreaRect` 기반 box 계산을 사용하도록 맞췄다.

## 검증 결과

- `compare`: `test.png`, `test2.png`, `test3.png`, `test4.png` 모두 sidecar와 FFI의 검출 수 및 텍스트가 100% 일치.
- `benchmark`: 같은 네 이미지 모두에서 FFI가 평균/중앙값 기준 sidecar보다 빠르거나 같음.

| 이미지 | sidecar median ms | FFI median ms | delta ms | count |
| --- | ---: | ---: | ---: | ---: |
| `test.png` | 2322.234 | 2196.892 | -125.342 | 7 |
| `test2.png` | 2991.168 | 2687.554 | -303.614 | 11 |
| `test3.png` | 3172.751 | 3107.396 | -65.355 | 23 |
| `test4.png` | 5725.186 | 4964.293 | -760.893 | 45 |

## 검증 명령

```powershell
python tools\scripts\ocr_sidecar_ffi.py compare --image testdata\ocr\test.png --image testdata\ocr\test2.png --image testdata\ocr\test3.png --image testdata\ocr\test4.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline --pipeline-resize-mode long-side --cargo-profile release
```

```powershell
python tools\scripts\ocr_sidecar_ffi.py benchmark --image testdata\ocr\test.png --image testdata\ocr\test2.png --image testdata\ocr\test3.png --image testdata\ocr\test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format png --ffi-format png --ffi-mode pipeline --pipeline-resize-mode long-side --cargo-profile release
```
