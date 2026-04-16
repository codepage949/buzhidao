# 웜업/초기화 실패 시 로딩 창에 에러 표시 및 종료 버튼

## 배경

OCR 엔진 초기화 또는 웜업이 실패하면 패닉 또는 `eprintln`만 남고 로딩 창이
닫혔다. 사용자는 성공한 줄 알고 OCR을 시도하거나, 아예 원인 없이 앱이
종료된 것처럼 보이게 된다.

## 변경

- `app/ui/src/loading.html`, `loading.ts`:
  Tauri 이벤트(`warmup_loading`, `warmup_failed`)를 받아
  로딩/실패 상태를 전환한다.
  실패 시 스피너를 에러 아이콘으로 교체하고,
  첫 줄 기준 에러 메시지 요약 + "종료" 버튼을 표시한다.
  종료 버튼은 Rust 커맨드 `exit_app`을 호출한다.
- `app/src/ocr/python_sidecar.rs`:
  실행 파일 누락/실행 실패 오류 메시지 자체에 경로를 포함하지 않도록 정리한다.
- `app/src/lib.rs`:
  OCR 엔진 생성 결과를 상태로 보관하고,
  초기화 실패·초기 웜업 실패·언어 변경 재웜업 실패 모두
  `warmup_failed`로 로딩 창에 표시한다.
  시작/재시작 시에는 `warmup_loading`을 emit하고,
  실패하면 로딩 창을 숨기지 않고 에러 상태를 유지한다.
  또한 로딩 상태를 별도 상태값으로 보관해,
  로딩 창 JS가 이벤트 리스너를 늦게 등록해도 현재 실패 상태를 복구할 수 있게 한다.
- `app/ui/src/loading.ts`:
  이벤트 수신 외에도 시작 시 `get_loading_status`를 호출해
  현재 로딩/실패 상태를 즉시 반영한다.
- `app/capabilities/default.json`:
  로딩 창 이벤트 수신을 위해 `core:event:default` 권한을 명시한다.

## 테스트

- 자동 테스트:
  `deno task test`의 `loading_test.ts`로 에러 메시지 요약 로직 검증
- 육안 검증:
  OCR 서버 경로를 잘못 지정하거나 실행 파일을 잠시 이동해
  초기화/웜업 실패와 초기 이벤트 레이스 상황을 재현하고
  로딩 창이 에러 상태로 전환되는지 확인
