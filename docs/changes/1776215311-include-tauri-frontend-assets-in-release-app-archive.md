# 릴리즈 app 아카이브에 Tauri 빌드 결과 반영

## 목적

- 릴리즈용 `app` 아카이브가 Tauri production 빌드 결과를 기준으로 정상 동작하게 한다.
- 분리 배포된 Windows/Linux `app` 자산만 풀었을 때 `page not found`가 발생하는 문제를 막는다.
- Windows 릴리즈 실행 시 불필요한 콘솔 창이 뜨지 않게 한다.

## 구현 계획

1. 릴리즈 앱 빌드를 `cargo tauri build --no-bundle` 경로로 통일한다.
2. app 아카이브는 다시 최소화해 실행 파일만 포함하도록 정리한다.
3. Windows OCR 서버 빌드에서 콘솔 창이 뜨지 않게 한다.
4. 패키징 테스트와 워크플로우 구문 검증으로 변경을 확인한다.

## 구현 내용

### Tauri production 빌드 경로로 수정

- 릴리즈 문제의 핵심은 외부 HTML 파일 누락이 아니라, `cargo build --release`로는 Tauri production asset 경로를 안정적으로 타지 못한다는 점이었다.
- 데스크톱 앱 빌드를 `cargo tauri build --no-bundle`로 변경해 `frontendDist`를 기준으로 production 자산을 embed하는 경로를 사용하도록 맞췄다.
- 그 결과 `localhost 연결 거부`로 보이던 설정/로딩 창 오류를 해소한다.

### app 아카이브 최소화

- `scripts/release_helper.py`의 app 레이아웃은 다시 `buzhidao(.exe)`만 복사하도록 정리했다.
- Tauri production 빌드 결과를 사용하므로 릴리즈 app zip에 `loading.html`, `settings.html`, `assets/` 같은 외부 프런트 파일을 따로 넣지 않는다.

### 릴리즈 워크플로우 연결

- 데스크톱 앱 빌드는 `cargo build --release` 대신 `cargo tauri build --no-bundle`로 수행한다.
- Tauri CLI가 `frontendDist`를 기준으로 production 자산을 embed하는 경로를 타도록 맞춘다.

### Windows 콘솔 창 정리

- 앱 본체는 이미 release에서 `windows_subsystem = "windows"`를 사용하고 있으므로 별도 콘솔 창이 뜨지 않는다.
- OCR 서버는 PyInstaller 빌드에 `--noconsole`을 추가해 Windows에서 디버깅용 콘솔 창이 보이지 않게 했다.
- 추가로 앱에서 OCR 서버 자식 프로세스를 띄울 때 Windows `CREATE_NO_WINDOW | DETACHED_PROCESS` 플래그를 적용해 모델 로딩 중 잠깐 나타나는 터미널 창도 막는다.
- 다만 앱 본체는 `cargo tauri dev`의 Ctrl+C 종료 동작을 유지해야 하므로, GUI 서브시스템 강제 적용은 release 빌드에만 남긴다.

### Paddle 런타임 probe 우회

- Windows에서 `ocr_server.exe` 단독 실행 시에도 짧은 콘솔 창이 두 번 보였고, 원인은 Tauri가 아니라 Paddle import 경로였다.
- Paddle은 `paddle.utils.cpp_extension` import 시점에 `where nvcc`, `where ccache`를 실행한다.
- OCR 서버는 런타임에 커스텀 확장을 빌드하지 않으므로, `paddleocr` import 전에 harmless shim 경로를 `CUDA_HOME`/`CUDA_PATH`와 `PATH`에 선세팅해 이 probe를 우회한다.

## 테스트

- `python -m unittest scripts.test_release_helper`
- `cargo test --manifest-path app/Cargo.toml`
- `deno eval "import { parse } from 'jsr:@std/yaml'; parse(await Deno.readTextFile('.github/workflows/release.yml')); console.log('ok');"`

## 리팩토링 검토

- 초기의 외부 프런트 파일 복사 접근은 제거하고, Tauri production 빌드와 최소 패키징이라는 더 단순한 구조로 정리했다.
