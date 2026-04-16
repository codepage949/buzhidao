# 지원 언어 목록 단일 소스화

## 배경

PaddleOCR 지원 언어 목록이 Rust(`settings.rs`), Python(`ocr_server.py`),
TypeScript(`settings.tsx`)에 3벌 복사되어 있다. 언어 추가/제거 시
동기화를 놓치면 한쪽은 지원하고 다른 쪽은 거부하는 불일치가 생긴다.

## 변경

- `shared/langs.json` 생성: `[{code, label}, ...]` 형태의 단일 소스.
- **Rust** (`settings.rs`): `include_str!("../../shared/langs.json")`로
  컴파일 타임 임베드. `SUPPORTED_LANGS` 상수를 `LazyLock`으로 교체하여
  JSON에서 코드 목록을 파싱.
- **TypeScript** (`settings.tsx`): Vite JSON import로 빌드 타임 번들링.
  하드코딩된 `SUPPORTED_LANGS` 배열 제거.
- **Python** (`ocr_server.py`): `__file__` 기준 상대경로로 JSON 로드,
  frozen 빌드 시 `sys._MEIPASS` 폴백. 하드코딩된 `LANGS` 튜플 제거.
- Vite `vite.config.ts`에 `shared/` 디렉토리 alias 추가 (필요 시).

## 테스트

- Rust: `normalize_source`가 JSON에서 로드된 화이트리스트로 정상
  동작하는지 기존 단위 테스트로 검증.
- Python: 구문 검증 + JSON 로드 경로 확인.
- TypeScript: UI 빌드 성공 확인.

## 배포 영향

- 세 곳 모두 빌드 타임에 JSON을 흡수하므로 릴리즈에 별도 파일 없음.
- Python PyInstaller spec에 `langs.json` data file 추가 필요.
