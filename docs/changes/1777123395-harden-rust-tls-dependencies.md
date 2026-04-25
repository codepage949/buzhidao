# Rust TLS 의존성 취약점 대응

## 배경

Dependabot이 `Cargo.lock`에서 다음 취약 전이 의존성을 보고했다.

- `openssl 0.10.76`: `rust-openssl` 계열 high 취약점 다수. 권고 버전은 `0.10.78` 이상.
- `rustls-webpki 0.103.10`: malformed CRL BIT STRING 처리 중 panic 가능성. 권고 버전은 `0.103.13` 이상.

`openssl`은 앱 코드가 직접 사용하지 않고 `reqwest`의 기본 TLS feature가 `native-tls`를 켜면서 들어온다.

## 결정

- 앱 HTTP 클라이언트는 OS native TLS가 필요하지 않으므로 `reqwest` 기본 feature를 끄고 `rustls-tls-webpki-roots`만 사용한다.
- 이 변경으로 `native-tls`, `hyper-tls`, `openssl` 전이 의존성을 앱 의존성 트리에서 제거한다.
- `rustls-webpki`는 `rustls` 계열 전이 의존성이므로 lockfile을 권고 버전 이상으로 갱신한다.

## 구현

- `Cargo.toml`
  - `reqwest = { version = "0.12", default-features = false, features = ["json", "rustls-tls-webpki-roots"] }`로 변경.
- `Cargo.lock`
  - `cargo update`로 TLS 관련 전이 의존성을 재해석한다.

## 검증 결과

- `cargo tree --target all -i openssl`
  - 결과: `package ID specification 'openssl' did not match any packages`
  - 의미: `openssl`이 lockfile/의존성 트리에서 제거됨.
- `cargo tree --target all -i native-tls`
  - 결과: `package ID specification 'native-tls' did not match any packages`
  - 의미: `native-tls` 경로가 제거됨.
- `cargo tree --target all -i rustls-webpki`
  - 결과: `rustls-webpki v0.103.13`
- `cargo test --lib`
  - 결과: 90개 테스트 통과.
- `cargo test --features gpu --lib`
  - 결과: 90개 테스트 통과.

## 범위 밖

- `glib` 취약점은 Tauri/Linux GUI 스택의 전이 의존성이다.
  앱 코드에서 직접 사용하는 패키지가 아니므로 자체 코드 변경만으로 제거하기 어렵고,
  Tauri/Wry/GTK 계열 의존성 갱신 또는 upstream 대응이 필요하다.
