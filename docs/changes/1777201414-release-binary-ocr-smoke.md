# 릴리즈 바이너리 OCR smoke 전환

## 배경

현재 릴리즈 workflow의 OCR smoke는 `cargo test --release`로 테스트 바이너리 안의
`릴리즈_ocr_smoke는_모델_보장후_1회_ocr를_성공한다`를 실행한다.

이 방식은 OCR 엔진 생성과 `run_ocr()` 경로는 검증하지만, 최종 릴리즈 실행 파일을
압축 해제한 뒤 사용자가 실행하는 조건을 검증하지 못한다. 따라서 Linux의
`libopencv_core.so.*` 누락처럼 실제 `./buzhidao` 실행 전 동적 로더 단계에서 실패하는
문제를 놓칠 수 있다.

## 목표

- smoke 테스트가 `cargo test`가 아니라 릴리즈 산출물의 실제 `buzhidao`/`buzhidao.exe`를 실행하게 한다.
- 실제 바이너리가 OCR 모델 보장, FFI 엔진 생성, warmup, fixture 이미지 OCR 실행까지 마친 뒤 종료하게 한다.
- 릴리즈 아카이브를 압축 해제한 위치에서 실행해 동봉 DLL/`.so` 누락과 rpath 문제를 함께 검증한다.
- GUI 육안 검증이 아니라 headless smoke로 자동 판정한다.

## 계획

1. 실제 바이너리용 headless smoke 진입점을 추가한다.
   - `src/main.rs`에서 Tauri 앱 시작 전에 CLI 인자를 확인한다.
   - 예: `buzhidao --release-ocr-smoke`
   - smoke 인자가 있으면 `buzhidao_lib::run()` 대신 OCR smoke 함수를 실행하고 exit code로 성공/실패를 반환한다.

2. 기존 테스트 전용 OCR smoke 흐름을 공용 런타임 함수로 분리한다.
   - 모델 보장, 모델 루트 검증, `OcrBackend` 생성, warmup, `run_ocr()` 실행을 테스트 모듈 밖으로 이동한다.
   - 기본 설정은 환경 변수에서 읽는다.
   - `OCR_SERVER_DEVICE`, `BUZHIDAO_PADDLE_MODEL_ROOT`, `BUZHIDAO_RELEASE_OCR_SMOKE_SOURCE`를 유지한다.
   - 이미지 경로는 `BUZHIDAO_RELEASE_OCR_SMOKE_IMAGE`가 있으면 사용하고, 없으면 중국어 `testdata/ocr/test.png` fixture를 사용한다.
   - 기본 source는 fixture 언어에 맞춰 `ch`로 둔다.

3. 성공 조건을 실제 OCR 성공으로 강화한다.
   - 빈 흰색 이미지 대신 fixture 이미지를 사용한다.
   - 최소 조건:
     - 모델 보장 성공
     - 모델 루트 검증 성공
     - FFI 엔진 생성 성공
     - warmup 성공
     - 이미지 로드 성공
     - OCR 실행 성공
     - OCR 결과 detection/text가 1개 이상 존재

4. 릴리즈 archive smoke 스크립트를 추가한다.
   - 예: `tools/scripts/release_binary_smoke.py`
   - 입력: 아카이브 경로, OS 이름, 실행 파일명, smoke 이미지 경로, timeout.
   - 동작:
     - 임시 디렉터리에 앱 아카이브 압축 해제
     - 압축 해제된 실제 바이너리를 `--release-ocr-smoke`로 실행
     - stdout/stderr를 요약 출력
     - non-zero exit code 또는 timeout을 실패로 처리

5. GitHub Actions release workflow를 변경한다.
   - 기존 `Run OCR smoke after build`의 `cargo test --release ...`를 제거한다.
   - `Prepare archives` 뒤에 `Run release binary OCR smoke` 단계를 둔다.
   - CPU matrix에서만 실제 OCR smoke를 실행한다.
   - Linux는 가능하면 추가 `LD_LIBRARY_PATH` 없이 실행해 `$ORIGIN` rpath와 동봉 `.so`를 검증한다.
   - Windows는 압축 해제된 `buzhidao.exe --release-ocr-smoke` 실행으로 DLL 로더 실패를 잡는다.

6. 테스트를 추가한다.
   - Python 테스트:
     - 아카이브 압축 해제
     - OS별 실행 파일 경로 계산
     - smoke 명령 구성
     - timeout/non-zero 처리
   - Rust 테스트:
     - smoke env 파싱
     - 기본 이미지/override 이미지 선택
     - 실제 OCR 실행 함수는 기존 release smoke 테스트에서 재사용
   - workflow 구조 테스트:
     - release workflow에서 `cargo test --release ... 릴리즈_ocr_smoke`가 제거됐는지 확인
     - archive 생성 이후 실제 binary smoke가 실행되는지 확인
     - CPU matrix에서만 실행되는지 확인

## 테스트 계획

- `python3 -m unittest tools.scripts.test_release_binary_smoke`
- `python3 -m unittest tools.scripts.test_release_workflow`
- `cargo test --no-default-features`
- OpenCV/Paddle SDK가 준비된 환경에서는 `cargo test --release 릴리즈_ocr_smoke는_모델_보장후_1회_ocr를_성공한다 -- --nocapture`
- CI에서 최종 검증: release workflow의 CPU matrix가 아카이브 압축 해제 후 실제 바이너리 smoke를 통과해야 한다.

## 구현

- `src/main.rs`
  - `--release-ocr-smoke` 인자를 받으면 Tauri GUI를 시작하지 않고 headless OCR smoke를 실행한다.
- `src/lib.rs`
  - release smoke CLI 인자 판별 함수와 smoke 실행 wrapper를 추가했다.
- `src/services/ocr_pipeline.rs`
  - release smoke 설정을 환경 변수에서 읽는다.
  - 기본 fixture인 `testdata/ocr/test.png`에 맞춰 기본 source를 `ch`로 둔다.
  - 실제 fixture 이미지를 로드해 OCR을 수행하고, 인식된 텍스트가 1개 이상 있어야 성공으로 본다.
  - 기존 `cargo test` 기반 release smoke는 같은 런타임 함수를 호출하는 얇은 경로로 유지했다.
- `src/paddle_models.rs`
  - 모델 루트 검증 함수를 테스트 전용이 아니라 release smoke 런타임에서도 사용할 수 있게 했다.
- `tools/scripts/release_binary_smoke.py`
  - 릴리즈 앱 아카이브를 임시 디렉터리에 압축 해제한다.
  - 압축 해제된 `buzhidao`/`buzhidao.exe`를 `--release-ocr-smoke`로 실행한다.
  - non-zero exit code와 timeout을 실패로 처리한다.
  - Windows runner의 기본 cp1252 stdout/stderr에서도 한글 로그 출력이 실패하지 않도록 표준 입출력을 UTF-8로 재설정한다.
- `.github/workflows/release.yml`
  - `cargo test --release ... 릴리즈_ocr_smoke...` 단계를 제거했다.
  - `Prepare archives` 뒤에 실제 앱 아카이브를 대상으로 하는 `Run release binary OCR smoke` 단계를 추가했다.
- `README.md`
  - release OCR smoke 실행 예시를 실제 앱 아카이브 기반 스크립트로 교체했다.

## 테스트 결과

- `python3 -m unittest tools.scripts.test_release_binary_smoke tools.scripts.test_release_workflow` 통과.
- `cargo check --no-default-features` 통과.
- `cargo test --no-default-features 릴리즈_ocr_smoke_cli_인자를_판별한다 -- --nocapture` 통과.
- `cargo test --no-default-features 릴리즈_ocr_smoke_기본_이미지는_testdata_fixture다 -- --nocapture` 통과.
- `cargo test --no-default-features` 통과.
- `python3 -m py_compile tools/scripts/release_binary_smoke.py tools/scripts/test_release_binary_smoke.py` 통과.
- `cargo fmt --check`는 저장소 기존 포맷 차이를 함께 보고해 전체 적용하지 않았다.

## 마무리 기준

- release workflow의 OCR smoke가 더 이상 `cargo test`에 의존하지 않는다.
- 최종 앱 아카이브에 들어간 실제 실행 파일로 OCR smoke를 수행한다.
- 문서, 테스트, workflow가 같은 계약을 설명한다.
