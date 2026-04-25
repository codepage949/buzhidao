# OCR 런타임 군더더기 제거 계획

## 배경

- 현재 저장소 기준의 code-level compare에서는 sidecar와 ffi의 OCR 코어 성능이 크게 벌어지지 않는다.
- 그런데 실제 프로그램 체감에서는 여전히 ffi가 sidecar보다 느리게 느껴지는 경우가 있다.
- 따라서 다음 라운드는 "막연한 최적화"가 아니라, 실제 앱 경로에서 남아 있는 군더더기를 영역별로 분리해 제거하는 방식으로 진행한다.

## 목표

- 한 번에 전체를 건드리지 않는다.
- OCR 결과 parity를 유지하면서, 실제 프로그램에서 보이는 총 시간(`OCR + 결과`)을 줄인다.
- 각 단계는 독립적으로 검증 가능해야 한다.

## 작업 원칙

1. 먼저 영역을 고정한다.
2. 각 영역마다 "무엇이 군더더기인지"를 코드 기준으로 정의한다.
3. 제거 전후를 같은 계측 기준으로 비교한다.
4. parity가 깨지면 되돌린다.
5. 한 영역이 끝난 뒤 다음 영역으로 이동한다.

## 1단계: 입력 준비 경로 정리

### 범위

- 캡처 직후 이미지 객체 전달 방식
- 불필요한 이미지 복사
- 포맷 변환
- 리사이즈
- 임시 버퍼/중간 객체 생성

### 현재 의심 포인트

- 같은 픽셀 데이터를 여러 번 새 버퍼로 옮기는 구간
- OCR에 직접 필요하지 않은 포맷 변환
- 리사이즈 판단과 실제 리사이즈가 분리되어 있는 구간

### 성공 기준

- OCR parity 유지
- 입력 준비 단계의 추가 복사/변환 수 감소
- `prepare_image_ms` 또는 대응 구간 감소

### 검증

- compare parity
- stage log
- 동일 fixture benchmark

## 2단계: FFI OCR 코어 경로 정리

### 범위

- `det`, `cls`, `rec` predictor 실행
- batch 준비
- decode/postprocess
- predictor 설정 차이

### 현재 의심 포인트

- sidecar와 ffi의 predictor 설정 차이
- rec 입력 준비/배치 구성의 차이
- stage 내부의 불필요한 메모리 재구성

### 성공 기준

- sidecar와 동일 입력 기준 exact parity 유지
- `ffi_ms` 및 `det/cls/rec` stage 중 병목 구간 감소

### 검증

- `--profile-stages`
- sidecar/ffi stage 비교
- 실제 앱 stage 로그

## 3단계: 앱 비동기 경계 정리

### 범위

- hotkey 처리 이후 OCR 작업 시작까지의 래핑
- `spawn_blocking` 대기
- 상태 전달과 중간 래퍼 구조

### 현재 의심 포인트

- OCR 코어와 무관한 대기 시간
- 작업 시작 전에 발생하는 중복 준비
- 불필요한 소유권 이동/복사

### 성공 기준

- OCR 코어 시간과 무관한 `spawn_wait_ms` 감소
- 전체 흐름 단순화

### 검증

- `[OCR_STAGE] app ... spawn_wait_ms=...`
- Rust 내부 경계 로그

## 4단계: 결과 payload/emit 경로 정리

### 범위

- OCR 결과 구조 변환
- bounds/groups 조립
- 이벤트 payload 직렬화
- emit 직전 후처리

### 현재 의심 포인트

- 화면 표시와 무관한 필드 유지
- 같은 데이터를 여러 구조로 재구성하는 경로
- emit 직전 추가 계산

### 성공 기준

- 표시용 최소 구조만 유지
- `emit_ms` 및 payload 준비 비용 감소

### 검증

- `emit_ms`
- payload 크기 비교
- overlay 수신 직전 데이터 형태 점검

## 5단계: 프런트 오버레이 렌더 경로 정리

### 범위

- 이벤트 수신 후 state 반영
- 파생 데이터 계산
- 불필요한 리렌더

### 현재 의심 포인트

- Rust에서 끝낼 수 있는 계산이 여전히 프런트에 남아 있는지
- 결과 표시 직전 불필요한 재가공이 있는지

### 성공 기준

- 첫 결과 표시까지의 경로 단순화
- 렌더 직전 불필요한 계산 제거

### 검증

- 오버레이 표시 시간 배지
- 프런트 수신 직후와 렌더 직전 경로 점검

## 우선순위

1. 입력 준비 경로
2. FFI OCR 코어 경로
3. 앱 비동기 경계
4. 결과 payload/emit 경로
5. 프런트 오버레이 렌더 경로

## 진행 방식

- 이번 계획 문서를 기준으로 한 번에 한 영역씩 처리한다.
- 각 영역 작업은 별도 구현 턴에서 수행한다.
- 구현 턴에서는 해당 영역만 건드리고, 테스트와 문서 갱신까지 함께 끝낸다.

## 진행 현황

### 1단계 입력 준비 경로 정리

- 완료

### 실제 반영 내용

- `CaptureInfo.image`를 `Arc<DynamicImage>`에서 `Arc<RgbaImage>`로 바꿨다.
- 영역 OCR crop도 `DynamicImage::crop_imm()` 대신 `RgbaImage` 기준 `imageops::crop_imm(...).to_image()`를 사용하게 정리했다.
- `run_ocr()` 입력 타입을 `&DynamicImage`에서 `&RgbaImage`로 바꿨다.
- 앱 OCR 리사이즈도 `DynamicImage::resize_exact()` 대신 `imageops::resize()`로 `RgbaImage`를 직접 유지하게 바꿨다.
- `OcrBackend::run_image()`도 `RgbaImage`를 직접 받아, FFI 진입 직전의 `DynamicImage -> RGBA` 준비 단계를 제거했다.

### 기대 효과

- 캡처, crop, 리사이즈, FFI 입력이 모두 같은 `RgbaImage` 타입으로 이어진다.
- OCR 경로에서 `DynamicImage` 래퍼와 추가 RGBA 준비 분기를 제거했다.
- 입력 준비 단계에서 타입 변환과 중간 래핑 비용을 줄였다.

### 검증

- `cargo test --lib ocr_pipeline -- --nocapture`
- `cargo test --lib pending_capture는_영역_ocr_후에도_재사용할_수_있다 -- --nocapture`
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline`

### 결과

- Rust 테스트 통과
- `test5.png` parity `45/45 exact match`

### 2단계 FFI OCR 코어 경로 정리

- 진행 중

### 실제 반영 내용

- `native/paddle_bridge/bridge.cc`의 rec 전처리에서 `resize -> canvas 생성 -> tensor 채움` 경로를 줄였다.
- 평상시 rec 전처리는 `resize_rec_input_image()`의 resize 결과에서 바로 tensor를 채우게 바꿨다.
- debug dump가 켜진 경우에만 `pad_rec_input_image()`로 padded 이미지를 따로 만든다.
- rec batch에서도 tensor width를 별도로 추적하고, 중간 canvas width에 의존하지 않게 정리했다.
- RGBA 메모리 입력은 더 이상 native 진입 시점에 per-pixel 채널 스왑을 하지 않는다.
- `Image`에 pixel layout을 추가하고, det/cls/rec 전처리와 OpenCV 변환이 `RGBA` 입력을 직접 해석하게 바꿨다.

### 기대 효과

- recognition crop마다 발생하던 중간 canvas 이미지 할당과 픽셀 복사를 제거한다.
- rec batch 준비 단계의 메모리 이동량을 줄인다.
- FFI 메모리 입력의 채널 재배치 비용을 row memcpy + layout 해석으로 낮춘다.

### 검증

- `cargo test --lib ocr_pipeline -- --nocapture`
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline`
- `python tools/scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format png --ffi-format png --ffi-mode pipeline`

### 결과

- Rust 테스트 통과
- `test5.png` parity `45/45 exact match`
- benchmark mean
  - sidecar `4911.38ms`
  - ffi `4926.23ms`

### 메모

- rec 준비 단순화와 RGBA 채널 스왑 제거는 parity는 유지했지만, `test5.png` 3회 벤치에서는 큰 속도 차이로 이어지지 않았다.
- 다음 우선순위는 3단계 앱 비동기 경계 정리다.

### 3단계 앱 비동기 경계 정리

- 진행 중

### 실제 반영 내용

- hotkey 경로에서 `store_pending_capture(&app, info.clone())`를 없앴다.
- OCR 실행에 필요한 `image`, `orig_width`, `orig_height`를 먼저 분리한 뒤, `CaptureInfo` 원본은 그대로 pending capture에 넘기게 정리했다.
- 그 결과 hotkey OCR 경로의 `CaptureInfo` 전체 clone 한 번을 제거했다.

### 기대 효과

- 앱 경계에서 불필요한 구조체 clone을 줄인다.
- pending capture 보관과 OCR 실행 준비가 같은 캡처 데이터를 공유하게 한다.

### 검증

- `cargo test --lib pending_capture는_영역_ocr_후에도_재사용할_수_있다 -- --nocapture`
- `cargo test --lib ocr_pipeline -- --nocapture`

### 결과

- 관련 테스트 통과

### 메모

- 앱 비동기 경계는 현재 코드 기준으로 큰 오버헤드 지점이 잘 보이지 않는다.
- 이 단계의 정리는 구조 단순화 성격이 크고, 다음 우선순위는 4단계 결과 payload/emit 경로 정리다.

### 4단계 결과 payload/emit 경로 정리

- 진행 중

### 실제 반영 내용

- `OcrResultPayload` 직렬화에서 내부용 `source`, `word_gap`, `line_gap`를 제외했다.
- `debug_trace=false`일 때는 `debug_trace` 필드 자체도 emit payload에서 제외한다.
- 프런트 오버레이는 `debug_trace`가 없으면 `false`로 해석하게 맞췄다.
- 직렬화 테스트를 추가해 오버레이 emit JSON에 내부 필드가 실리지 않음을 고정했다.

### 기대 효과

- 오버레이로 보내는 이벤트 payload 크기를 줄인다.
- 화면 표시와 무관한 내부 상태 전달을 제거한다.

### 검증

- `cargo test --lib ocr_pipeline -- --nocapture`
- `deno task build`

### 결과

- Rust 테스트 통과
- 프런트 빌드 통과

### 5단계 프런트 오버레이 렌더 경로 정리

- 완료

### 실제 반영 내용

- 오버레이에서 `debug_trace` 판정을 `debugTraceEnabled`로 한 번만 계산하게 정리했다.
- 디버그용 `console.log`는 `import.meta.env.DEV`일 때만 실행되게 제한했다.

### 기대 효과

- prod 빌드에서 debug trace 관련 콘솔 직렬화 비용을 제거한다.
- 렌더 경로의 조건 분기를 조금 더 단순하게 유지한다.

### 검증

- `cargo test --lib ocr_pipeline -- --nocapture`
- `deno task build`

### 결과

- Rust 테스트 통과
- 프런트 빌드 통과

## 현재 정리

- 1단계 입력 준비, 2단계 FFI 코어, 3단계 앱 경계, 4단계 payload/emit, 5단계 프런트 렌더를 모두 한 번씩 정리했다.
- 이 중 실제 의미 있는 변화는 입력 준비 타입 정리, rec 전처리 단순화, payload 직렬화 축소였다.
- 반면 앱 경계와 프런트 렌더 쪽은 코드 군더더기는 줄였지만, 체감 시간을 크게 바꿀 만한 병목은 아니었다.

## 다음 판단

- 현재 남아 있는 속도 이슈를 더 줄이려면 다시 "측정 기반" 단계로 돌아가야 한다.
- 특히 실제 프로그램 로그 기준 `ffi_ms`가 다시 큰 경우, 더 이상의 군더더기 제거보다 predictor stage별 병목 재측정이 우선이다.
