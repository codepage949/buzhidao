# GPU 번들 리소스를 별도 설정 파일로 분리

## 문제

`tauri.conf.json`의 `bundle.resources`에 `"cuda/*"`가 항상 포함되어 있어,
`cuda/` 디렉토리가 없는 환경(GPU feature 미사용)에서 `cargo tauri dev` 실행 시
아래 오류가 발생했다.

```
glob pattern cuda/* path not found or didn't match any files.
```

## 변경 사항

- `tauri.conf.json`: `bundle.resources`에서 `"cuda/*"` 제거
- `tauri.gpu.conf.json` 신규 생성: `cuda/*`를 포함한 GPU 빌드 전용 리소스 오버라이드

## 사용법

| 상황 | 명령 |
|------|------|
| 일반 개발 | `cargo tauri dev` |
| 일반 빌드 | `cargo tauri build` |
| GPU 빌드 | `cargo tauri build --features gpu --config tauri.gpu.conf.json` |

Tauri 2의 `--config` 옵션은 기본 설정에 deep merge되므로,
`tauri.gpu.conf.json`에는 변경이 필요한 항목(`bundle.resources`)만 정의하면 된다.
