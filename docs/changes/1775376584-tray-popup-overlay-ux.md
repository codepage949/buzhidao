# 트레이 전용 앱 + 팝업 번역 뷰어 UX 개선

## 변경 목적

- 메인 윈도우 제거: 시작 시 트레이 아이콘만 표시 (메뉴: 종료)
- OCR 영역 클릭 시 오버레이는 유지하고 근처에 마크다운 팝업 표시
- 오버레이 닫기: 닫기 버튼 추가 + 빈 곳 클릭으로 닫기 (기존 동작 유지)
- 팝업은 동시에 하나만 (단일 창 재사용)

## 변경 사항

### 제거
- `src/index.html`, `src/app.tsx` — 메인 윈도우 UI 삭제
- `tauri.conf.json` main 윈도우 항목 제거

### 추가
- `src/popup.html`, `src/popup.tsx` — 번역 결과 마크다운 팝업
- `tauri.conf.json` popup 윈도우 항목 (420×500, 장식 없음)
- Rust 시스템 트레이 (종료 메뉴)
- Rust `close_overlay` 커맨드 (오버레이 + 팝업 동시 숨김)
- `react-markdown` 의존성 추가 (`deno.json`)

### 수정
- `lib.rs` `select_text`: 오버레이 유지, 팝업 위치 지정 후 표시, popup 창에 이벤트 발송
- `lib.rs` `handle_prtsc`: 신규 캡처 시 팝업도 숨김
- `overlay.tsx`: "닫기 (ESC)" 버튼 추가, 빈 곳 클릭 → `close_overlay` 호출, OCR 클릭 시 박스 좌표 전달
- `popup.tsx`: ESC 키 → `close_overlay` 호출 (팝업이 포커스를 가져가므로 별도 핸들러 필요)
- `vite.config.ts`: main → popup 입력 교체
- `capabilities/default.json`: windows 목록 main→popup 교체
- `src-tauri/icons/icon.png`, `icon.ico`: 프로젝트 아이콘 교체 (Catppuccin Mocha 테마, "不" 한자, 멀티 사이즈 ICO)

## 이슈 & 해결

### 팝업 포커스로 인한 ESC 동작 불가
팝업 표시 시 `set_focus()`를 호출해 팝업이 포커스를 가져감 → 오버레이의 `keydown` 리스너가 더 이상 이벤트를 받지 못해 ESC가 동작하지 않음.

→ 팝업에도 `keydown` 핸들러를 추가해 ESC 시 `invoke("close_overlay")` 호출.

## 팝업 UX 흐름

```
PrtSc 누름 → 오버레이 표시 (기존 팝업 숨김)
OCR 영역 클릭 → 팝업이 박스 옆에 나타나며 번역 중 표시
번역 완료 → 팝업에 마크다운 결과 렌더링
오버레이 닫기 버튼/빈 곳 클릭 → 오버레이 + 팝업 동시 숨김
팝업 X 버튼 → 팝업만 숨김 (오버레이 유지)
```

## 팝업 위치 결정 로직 (Rust)

- 기본: OCR 박스 우측 (box_x + box_w + 12)
- 화면 오른쪽 벗어나면: 박스 좌측 (box_x - popup_w - 12)
- Y축: box_y에서 시작, 화면 아래 벗어나면 위로 올림
- 좌표: 논리 픽셀 기준 (LogicalPosition)
