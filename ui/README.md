# Buzhidao UI

`ui/`는 Tauri 데스크톱 앱의 프런트엔드 엔트리들을 관리합니다. 오버레이, 팝업, 로딩, 설정 창이 각각 별도 진입점으로 빌드됩니다.

## 기술 스택

- React 19
- TypeScript
- Vite 8
- Deno
- `@tauri-apps/api`

## 프로젝트 구조

```text
ui/
├── src/
│   ├── overlay.html            # Tauri가 여는 루트 오버레이 엔트리
│   ├── popup.html              # Tauri가 여는 루트 팝업 엔트리
│   ├── loading.html            # Tauri가 여는 루트 로딩 엔트리
│   ├── settings.html           # Tauri가 여는 루트 설정 엔트리
│   ├── lib/
│   │   ├── app-hooks.ts         # 윈도우 이벤트/리스너 훅
│   │   └── detection.ts         # OCR 박스 그룹핑 로직
│   └── pages/
│       ├── overlay/             # 오버레이 로직과 테스트
│       ├── popup/               # 팝업 로직
│       ├── loading/             # 로딩 로직과 테스트
│       └── settings/            # 설정 로직과 저장 상태 테스트
├── deno.json
└── vite.config.ts
```

## 개발 및 빌드

개발 서버:

```bash
deno task dev
```

프로덕션 빌드:

```bash
deno task build
```

테스트:

```bash
deno task test
```

## 실행 경로

### overlay

- Rust가 `overlay_show` 이벤트를 보내면 로딩 상태로 전환합니다.
- OCR 결과를 받으면 박스 그룹을 렌더링하고, 클릭 시 `select_text`를 호출합니다.
- 영역 재선택 관련 코드와 테스트 파일은 남아 있지만, 현재 제품에서는 사용자 기능으로 노출하지 않습니다.
- ESC 또는 배경 클릭 흐름은 `close_overlay`로 연결됩니다.

### popup

- 선택된 텍스트의 번역 결과를 표시합니다.
- 닫기 동작은 `close_popup`을 호출해 팝업만 닫고 오버레이 포커스를 복구합니다.

### loading

- 앱 시작 직후 OCR warmup이 끝날 때까지 표시됩니다.
- warmup 완료 후 Rust가 창을 닫습니다.
- 초기화나 warmup 실패 시 실패 상태로 전환되며, 종료 버튼을 표시합니다.
- 이벤트를 놓친 경우에도 `get_loading_status`로 현재 상태를 다시 조회해 복구합니다.

### settings

- `get_user_settings`로 현재 설정과 초기 notice를 불러옵니다.
- `get_ocr_busy`와 `ocr_busy_changed`로 OCR busy 상태를 조회/구독합니다.
- OCR 진행 중에는 저장 버튼이 비활성화되고 `OCR 진행 중에는 설정을 저장할 수 없습니다.` 문구를 표시합니다.
- 저장 시 `save_user_settings`를 호출합니다.

## 특이 사항

- Tauri 창 URL은 `overlay.html`, `popup.html`, `loading.html`, `settings.html`를 사용합니다.
- `vite.config.ts`는 위 루트 HTML 엔트리를 multi-entry로 빌드합니다.
- 테스트는 그룹핑, 로딩 상태 요약, 오버레이 닫기 억제, 영역 선택 결과, 설정 저장 가능 상태 같은 핵심 UI 로직만 포함합니다.
