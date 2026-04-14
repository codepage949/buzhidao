# onedir fallback 테스트의 CWD 의존성 제거

## 배경

`tests::번들_리소스에_onedir_폴더가_있으면_그_안의_ocr_server를_사용한다`가 로컬에서 실패했다.

테스트 의도: `configured` 경로가 존재하지 않을 때, `resource_dir/<parent_name>/<file_name>` 후보로 fallback 되는지 확인.

하지만 테스트가 사용한 `"../ocr_server/dist/ocr_server/ocr_server.exe"`가
cargo test CWD(`app/`) 기준으로 실제 파일 시스템에 존재(`buzhidao/ocr_server/dist/ocr_server/ocr_server.exe`)해
`configured_path.exists()` 분기에서 early return 되어 fallback 경로가 한 번도 테스트되지 않았다.

## 변경

테스트의 `configured` 인자를 로컬 파일 시스템과 무관한 가짜 경로로 교체.

```
"missing-nonexistent/ocr_server/ocr_server.exe"
```

`parent_name`은 여전히 `"ocr_server"`라 `resolve_ocr_server_executable`의
두 번째 후보 계산 로직을 그대로 검증한다. 프로덕션 코드(`resolve_ocr_server_executable`)는 변경하지 않음.

## 검증

- `cargo test --lib` 전 테스트 통과
