# current_exe import 빌드 오류 수정

## 구현 계획

- `src/lib.rs`에서 `env::current_exe()` 사용 위치와 import 상태를 확인한다.
- 파일의 기존 import 스타일에 맞춰 최소 수정으로 빌드 오류를 해결한다.
- 관련 빌드 검증으로 실제 컴파일 오류가 해소되었는지 확인한다.

## 변경 사항

- `src/lib.rs`에서 `std::env` import 누락으로 발생한 `env::current_exe()` 해석 오류를 수정한다.
- 파일 상단에 `use std::env;`를 추가해 `env::current_exe()` 호출이 정상 해석되도록 변경했다.

## 테스트 계획

- `cargo check`로 Rust 컴파일이 통과하는지 확인한다.

## 테스트 결과

- `cargo check` 통과
- Paddle 헤더/브리지 C++ 코드에서 기존 경고는 남아 있으나, 이번 수정과 직접 관련된 Rust 컴파일 오류는 재현되지 않았다.
