# 시작 시 OCR 모델 로딩 창과 warmup 중 핫키 차단

## 배경

`OcrBackend`는 첫 PrtSc 시점에 사이드카를 지연 시작했다. Paddle 모델 로딩 + warmup이
수 초~수십 초 걸리므로 사용자는 첫 캡처에서 긴 지연을 경험했고,
그 사이 추가 PrtSc 입력이 중복 요청으로 쌓일 위험이 있었다.

## 변경

- `loading` 윈도우 추가 (300×90, center, alwaysOnTop, skipTaskbar, `visible: true`).
  `loading.html`은 순수 HTML/CSS 스피너로 React 번들 없이 빠르게 뜬다.
- `OcrBackend::warmup()` 추가 — 사이드카를 선행 시작해 Python `warmup_models` 완료
  (=`ready` 수신) 시점까지 블로킹.
- `setup()` 내 백그라운드 태스크에서 `spawn_blocking(warmup)` 수행 →
  완료 시 loading 창 `close()` + `busy = false`.
- `busy`를 `AtomicBool::new(true)`로 초기화해 warmup 중 들어오는 PrtSc를
  `handle_prtsc` 진입부 `busy.swap(true)` 분기에서 전부 무시.
- `tauri.conf.json` windows 정의, `capabilities/default.json` windows 목록,
  `vite.config.ts` 엔트리에 `loading` 추가.

## 핫키 차단 검증

`handle_prtsc`는 진입 직후 `if busy.swap(true) { return; }`로 빠져나가며
busy를 되돌리지 않는다. 초기 busy=true면 warmup 완료까지 모든 PrtSc가
무시되고 busy=true 상태가 유지된다. warmup 완료 시에만 false로 내려가
이후 정상 동작한다.

## 검증

- `cargo check` 통과
- `cargo test --lib` 26/27 통과 (실패 1건 `tests::번들_리소스에_onedir_폴더가...`은
  이 변경과 무관한 기존 이슈 — 로컬 CWD에 `../ocr_server/dist/ocr_server/ocr_server.exe`가
  실제로 존재해 fallback 탐색을 건너뜀)
- 로딩창 가시성·스피너 애니메이션·warmup 완료 후 창 닫힘은 육안 확인 영역
