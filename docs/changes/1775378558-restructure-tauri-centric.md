# 프로젝트 구조 Tauri 중심으로 재편

## 변경 목적

기존에는 루트에 `deno.json`, `vite.config.ts`, `tsconfig.json` 등 프론트엔드
도구 설정이 나열되어 프로젝트가 "Tauri를 쓰는 프론트엔드 앱"처럼 보였음.
Tauri 앱을 중심 프로젝트로 하고, 프론트엔드·OCR 서버를 하위 모듈로 배치.

## 변경 내용

### 디렉토리 이름 변경 / 이동

| 이전 | 이후 | 비고 |
|------|------|------|
| `src/` | `ui/src/` | 프론트엔드 소스 |
| `deno.json` (루트) | `ui/deno.json` | |
| `deno.lock` (루트) | `ui/deno.lock` | |
| `vite.config.ts` (루트) | `ui/vite.config.ts` | |
| `tsconfig.json` (루트) | `ui/tsconfig.json` | |
| `server/` | `ocr/` | OCR 서버 |

### 설정 파일 수정

- `src-tauri/tauri.conf.json`
  - `beforeDevCommand`: `"cd ui && deno task dev"`
  - `beforeBuildCommand`: `"cd ui && deno task build"`
  - `frontendDist`: `"../ui/dist"` (구 `"../dist"`)

## 최종 구조

```
buzhidao/
├── src/               # Rust 소스 (표준 Cargo 구조)
│   ├── lib.rs
│   └── main.rs
├── Cargo.toml
├── Cargo.lock
├── build.rs
├── tauri.conf.json
├── capabilities/
├── gen/
├── icons/
├── ui/                # 프론트엔드 서브 프로젝트
│   ├── src/
│   ├── deno.json
│   ├── deno.lock
│   ├── vite.config.ts
│   └── tsconfig.json
├── ocr/               # OCR 서버 (Python/FastAPI)
├── .env
├── CLAUDE.md
└── docs/
```
