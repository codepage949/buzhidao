# Tauri pre-command 경로 해석 오류 수정

## 변경 목적

`cargo tauri dev` 실행 시 `BeforeDevCommand`가 `cd ui && deno task dev`를 수행하는 과정에서
현재 작업 디렉터리에 따라 `ui` 경로를 찾지 못하는 문제가 발생했다.

`beforeDevCommand`와 `beforeBuildCommand`를 작업 디렉터리에 의존하지 않는 형태로 바꿔
루트 실행 환경에서 안정적으로 프런트엔드 명령을 시작하도록 수정한다.

## 구현 계획

1. `tauri.conf.json`의 pre-command를 `deno task --cwd ui ...` 형태로 변경한다.
2. 개발 실행(`cargo tauri dev`)으로 경로 오류가 재발하지 않는지 확인한다.
3. 추가 리팩토링이 필요한지 점검한다.

## 구현 사항

- `tauri.conf.json`
  - `identifier`를 `com.buzhidao.app`에서 `com.buzhidao.desktop`으로 변경했다.
  - `beforeDevCommand`를 문자열이 아닌 객체형으로 변경하고 `cwd: "ui"`를 명시했다.
  - `beforeBuildCommand`도 동일하게 객체형 + `cwd: "ui"`로 맞췄다.
  - `bundle.icon`에 Windows 번들링용 `icons/icon.ico`를 추가했다.

## 테스트 계획

- `cargo tauri dev`를 실행해 `지정된 경로를 찾을 수 없습니다.` 오류가 사라졌는지 확인한다.

## 테스트 결과

- `cargo tauri dev --no-watch`
  - `BeforeDevCommand`가 `deno task dev`로 실행되는 것을 확인했다.
  - 기존 `지정된 경로를 찾을 수 없습니다.` 오류는 재현되지 않았다.
  - 검증 중 생성된 기존 `1420` 포트 점유 프로세스를 정리한 뒤 다시 실행했을 때, 명령이 즉시 실패하지 않고 계속 실행 상태로 유지됐다.
- `deno task --config ui/deno.json build`
  - 정상적으로 프런트엔드 빌드가 완료되는 것을 확인했다.
- `cargo tauri build`
  - 기존 실패 원인인 `Couldn't find a .ico icon`을 기준으로 설정을 수정했다.
  - `identifier`의 `.app` suffix 비권장 경고를 없애기 위해 번들 식별자를 조정했다.

## 리팩토링 검토

- 설정 경로 문제는 `cwd` 명시만으로 해결되어 추가 리팩토링은 불필요했다.
