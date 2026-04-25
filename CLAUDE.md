## 회고

### Tauri ACL 권한 누락

Tauri에서 JS API(`window.close()`, `window.hide()` 등)를 사용하려면
`capabilities/*.json`의 `permissions` 배열에 해당 권한을 명시해야 한다.
권한 없이 호출하면 런타임에 `Command plugin:window|XXX not allowed by ACL` 오류가 발생한다.

→ JS에서 새 Tauri API를 호출할 때마다 대응하는 `core:window:allow-XXX`(또는
`plugin-name:allow-XXX`) 권한이 capabilities에 있는지 반드시 확인할 것.
권한 이름은 `core:window:allow-<메서드명>` 패턴을 따른다(예: `allow-close`, `allow-hide`, `allow-set-focus`).

### bbox 그루핑 canMerge 비대칭 조건 버그

`item.x - group.right <= xGap` 단방향 조건은 item이 group 왼쪽에 있을 때
음수가 되어 항상 통과한다 (예: 오른쪽 컬럼 아래 줄에 왼쪽 컬럼이 잘못 병합).

→ X 범위 겹침 기준으로 교체:
`item.x <= group.right + xGap AND item.right >= group.x - xGap`
좌우 양방향 대칭이어야 한다.

### Tauri 투명 오버레이 창 (Windows)

투명 WebView2 창에서 마우스 이벤트가 아래 창으로 통과하는 문제가 있다.
두 가지를 모두 적용해야 한다:
1. Rust: `window.set_ignore_cursor_events(false)` 명시 호출
2. HTML: `body { background: rgba(0,0,0,0.002); }` — 픽셀 알파값 비-제로

### Tauri WebView2 서스펜드 문제

오버레이에서 `await getCurrentWindow().hide()` 후 `invoke()`를 호출하면
WebView2가 서스펜드되어 IPC가 전달되지 않는다.
오버레이 닫기 + 후속 작업은 Rust 커맨드 하나에서 일괄 처리할 것.

### tauri-plugin-global-shortcut 중복 등록

`on_shortcut`은 OS 등록 + 콜백 설정을 함께 처리한다.
같은 단축키로 `register`를 추가 호출하면 `os error 6` 패닉 발생.

### 전역 단축키 콜백에서 비동기 작업

`on_shortcut` 콜백은 Tokio 런타임 밖에서 실행된다.
`tokio::spawn` 대신 `tauri::async_runtime::spawn` 사용.

### rdev::grab + Tauri 전역 단축키 충돌 (Windows)

`tauri-plugin-global-shortcut`(RegisterHotKey)은 수식키 없는 PrintScreen을 전역 등록할 수 없다.
`rdev::grab`(WH_KEYBOARD_LL)으로 교체했으나, Tauri 기본 설정에서 창이 포커스될 때
창 메시지 큐가 raw 키 이벤트를 이중 처리해 훅과 충돌한다.

→ `.device_event_filter(tauri::DeviceEventFilter::Always)` 추가로 해결.
Tauri가 장치 이벤트를 자체 이벤트 루프에서 처리하게 해 창 메시지 큐와의 이중 처리를 막는다.

### 팝업 창 포커스와 오버레이 키 이벤트 단절

팝업 창을 표시하며 `set_focus()`를 호출하면 포커스가 팝업으로 이동한다.
이후 오버레이 창의 `keydown` 이벤트 리스너는 이벤트를 받지 못해 ESC 등 키 처리가 동작하지 않는다.

→ 팝업 창에도 동일한 키 핸들러를 추가해야 한다.
특히 오버레이·팝업을 함께 닫는 동작(ESC)은 두 창 모두에서 처리할 것.

### 팝업 닫기 동작 범위

팝업의 닫기 버튼과 ESC는 팝업만 닫아야 한다.
오버레이 유지가 필요한 흐름에서 `close_overlay`를 재사용하면 의도치 않게 전체 UI가 사라진다.

→ 팝업 전용 닫기 동작은 별도 Rust 커맨드로 분리하고,
필요하면 `popup.hide()` 후 `overlay.set_focus()`로 포커스를 복구할 것.

### paddle2onnx Windows 빌드 문제

`paddle2onnx`는 Windows Python 3.12/3.13에서 DLL 로드 실패가 발생한다.
Docker Linux 컨테이너(python:3.12-slim + libgomp1)에서 변환하면 문제없다.
`tools/scripts/export_onnx_docker.py` 참고.

### PaddlePaddle 3.x 모델 형식 변경

PaddlePaddle 3.x부터 추론 모델이 `.pdmodel` 대신 `inference.json`(PIR 형식) + `.pdiparams`를 사용한다.
`paddle2onnx 2.1.0`은 이 형식을 지원하므로 `model_filename`에 `.json` 파일을 전달하면 된다.

### ort 크레이트 API (2.0.0-rc.12)

- `Session::run()`은 `&mut self`를 요구한다. 공유 상태로 사용하려면 `Mutex<Session>` 필요.
- `try_extract_tensor::<f32>()`는 `(&Shape, &[f32])` 튜플을 반환한다. `ArrayView`가 아님.
- `ort::inputs![]` 매크로는 배열을 직접 반환하므로 `.map_err()` 불필요.
- `ndarray` 버전은 `ort`가 사용하는 버전과 일치시켜야 한다 (`cargo tree -p ort`로 확인).

### OCR det 전처리는 모델 학습 분포를 유지해야 한다

화면 텍스트 인식률 향상을 위해 det 입력에 그레이스케일 변환 + 언샤프 마스킹을 적용했으나,
오히려 인식률이 하락했다.

원인: det 모델은 컬러 BGR + ImageNet mean/std 정규화로 학습되었다.
그레이스케일 변환은 3채널에 동일 값을 넣어 모델이 기대하는 채널별 분포를 망가뜨린다.

→ ONNX 모델에 전처리를 추가할 때는 학습 시 사용한 입력 분포를 반드시 유지할 것.
컬러 공간 변환, 정규화 방식 변경 등은 모델 재학습 없이는 역효과를 낼 수 있다.

### Docker 실행 구성과 테스트 경로 분리

사용자가 "컨테이너로 올라간 프로그램 대상 테스트"를 원할 때
실행용 `docker-compose`에 테스트 서비스를 섞지 말 것.

→ 배포/실행 Compose는 그대로 두고,
호스트 스크립트나 별도 테스트 진입점으로 실행 중인 컨테이너의 HTTP 엔드포인트를 검증할 것.

### Deno/Vite 프로젝트 디렉토리 이동 시 node_modules 복사 금지

`ui/` 같은 Deno `nodeModulesDir=auto` 프로젝트를 디렉토리 이동/복사할 때
기존 `node_modules`를 함께 복사하면 npm 링크 구조가 깨져
Vite가 `rollup` 같은 간접 의존성을 찾지 못할 수 있다.

→ 디렉토리 이동 후에는 `node_modules`를 복사하지 말고 삭제한 뒤
대상 위치에서 `deno install`로 재생성할 것.

### PowerShell로 Git 파일 내용을 되쓸 때 BOM/줄바꿈 주의

`git show ... | Out-File` 또는 `Set-Content -Encoding UTF8`로 파일을 복원하면
UTF-8 BOM이 붙거나 줄바꿈이 바뀌어 전체 파일 diff가 날 수 있다.

→ 저장소 blob을 그대로 복원해야 할 때는 `git cat-file -p ... > file`처럼
Git 출력 바이트를 그대로 쓰는 방식을 우선 사용할 것.

### 플랫폼별 wheel 내용과 인덱스 차이는 직접 확인할 것

`paddlepaddle-gpu`나 `nvidia-*` 패키지의 CUDA/cuDNN DLL 포함 여부를
Linux 컨테이너 기준으로 추정하면 Windows에서 틀릴 수 있다.

특히 PyPI와 Paddle 전용 인덱스는 같은 패키지명이어도 제공 버전/플랫폼이 다를 수 있다.
Windows GPU 번들 작업에서는 `site-packages` 실제 파일 목록과 인덱스의 `win_amd64` wheel 존재 여부를 먼저 확인할 것.

### 로컬 개발 환경 경로 노출 금지

에러 메시지, UI 문구, 테스트 데이터에 로컬 개발 환경 절대 경로
(`C:\Users\...` 등)를 포함하면 사용자 노출과 문서/테스트 오염이 동시에 발생한다.

→ 파일 경로가 필요한 내부 진단 로그가 아니라면
오류 원문 자체에서 경로를 제거할 것.
UI 레이어에서만 가리는 것으로 끝내지 말고,
테스트 fixture와 변경 문서에도 실제 로컬 경로를 남기지 말 것.

### OCR 이미지 변환 최적화는 parity로 먼저 검증할 것

`rotate180()`처럼 겉보기엔 단순한 이미지 변환도
보간, 경계 처리, 알파 처리 차이로 OCR 결과가 달라질 수 있다.

→ `warpAffine`/`warpPerspective` 계열을 단순 치환하는 최적화는
반드시 `compare` parity를 먼저 확인하고, exact text match가 유지될 때만 채택할 것.

### 구조 변경 후 프런트 빌드 실패는 의존성 추가보다 캐시를 먼저 의심할 것

`ui/`처럼 `nodeModulesDir=auto`를 쓰는 Deno/Vite 프로젝트는
디렉토리 재배치 후 stale `node_modules` 때문에 간접 의존성 해석이 깨질 수 있다.

이 경우 `vite build` 실패를 보고 곧바로 `deno.json` import를 늘리면
구조 변경과 무관한 환경 문제를 설정 변경으로 덮게 된다.

→ 구조 변경 후 빌드가 깨지면 먼저 `node_modules` 삭제와 재생성을 확인하고,
실제 의존성 누락이 재현될 때만 패키지 선언 변경을 검토할 것.

### Windows OpenCV SDK 추출 결과는 실제 디렉토리 구조로 확인할 것

OpenCV Windows 설치 파일을 `third_party/opencv-sdk/<platform>` 아래로 옮기면서
`install/x64/vc17/lib` 같은 평탄한 배치를 바로 가정하면 틀릴 수 있다.
실제 추출 결과는 `opencv/build/...`, `opencv/sources/...`처럼
한 단계 더 중첩된 루트 구조일 수 있다.

→ 외부 SDK 경로를 canonical로 옮길 때는 먼저 추출 결과 디렉토리를 확인하고,
탐지 로직과 준비 스크립트의 기대 경로를 그 구조에 맞춰 정리할 것.

### 프런트 디렉토리 재배치 시 Tauri 창 URL 계약을 같이 확인할 것

`ui/src/pages/...`로 엔트리를 재배치해도 Tauri 설정이 여전히
`overlay.html`, `loading.html`, `popup.html`, `settings.html`를 열고 있으면
실행 시 "페이지를 찾을 수 없음" 회귀가 난다.

→ 엔트리 HTML을 이동하거나 이름을 바꿀 때는
`tauri.conf.json`의 window `url`과 dev/build 산출물 경로가 그대로 유지되는지 먼저 확인할 것.
계약을 바꾸지 않을 거면 루트 compatibility entry를 남겨 두는 편이 안전하다.

### 숨겨진 fullscreen 오버레이를 일반 창으로 되돌리지 말 것

오버레이가 이미 fullscreen 창으로 생성돼 있어도,
표시 직전에 `set_fullscreen(false)`로 일반 창 상태를 만들었다가 다시 보여 주면
최초 표시 때 창이 커지는 플래시가 눈에 보일 수 있다.

→ fullscreen overlay는 숨김 상태에서 그대로 유지하고,
표시 시점에는 `show()`만 하도록 두는 편이 안정적이다.
fullscreen 토글은 정말 상태 변경이 필요할 때만 사용할 것.
