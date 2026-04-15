# 릴리즈 워크플로우에 Tauri CLI 설치 단계 추가

## 목적

- GitHub Actions 릴리즈 워크플로우에서 `cargo tauri build --no-bundle`가 실패하지 않게 한다.
- CI 러너에 없는 `cargo tauri` 서브커맨드를 명시적으로 설치한다.

## 구현 계획

1. 릴리즈 워크플로우에서 데스크톱 앱 빌드 전에 `tauri-cli`를 설치한다.
2. 문서에 왜 이 단계가 필요한지 기록한다.
3. YAML 파싱으로 워크플로우 문법을 확인한다.

## 구현 내용

### Tauri CLI 설치 단계 추가

- `cargo tauri build --no-bundle`는 Cargo 기본 명령이 아니라 `cargo-tauri` 외부 서브커맨드를 필요로 한다.
- GitHub Actions 러너에는 기본적으로 `cargo tauri`가 없어서 `error: no such command: tauri`가 발생했다.
- 릴리즈 워크플로우에 `cargo install tauri-cli --version '^2' --locked` 단계를 추가해 빌드 전에 CLI를 설치하도록 수정했다.

## 테스트

- `deno eval "import { parse } from 'jsr:@std/yaml'; parse(await Deno.readTextFile('.github/workflows/release.yml')); console.log('ok');"`

## 리팩토링 검토

- 변경 범위가 워크플로우 한 단계 추가로 작고 명확해 추가 리팩토링은 필요하지 않았다.
