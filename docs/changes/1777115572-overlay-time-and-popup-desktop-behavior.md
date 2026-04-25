# 오버레이 시간 표시와 팝업 데스크톱 동작 조정

## 배경

- 오버레이 좌측 상단의 `OCR + 결과 ...ms` 표시가 현재 어두운 배경, 뚜렷한 테두리, 그림자 때문에 OCR 결과보다 시선을 끈다.
- 이 표시는 디버깅과 체감 시간 확인에는 필요하지만, 기본 OCR 사용 흐름에서는 보조 정보로 보여야 한다.
- 번역 팝업이 Windows 가상 데스크톱 전환 시 현재 데스크톱으로 따라오는 문제가 있다.
- 과거 오버레이에서도 같은 증상이 있었고, 실제 원인은 `alwaysOnTop`이 아니라 `skipTaskbar`가 만드는 tool window 성격이었다.

## 구현 계획

1. 오버레이 시간 표시를 배지가 아닌 낮은 대비의 텍스트로 조정한다.
2. 포인터 이벤트 차단, 위치, 표시 조건은 그대로 유지한다.
3. 팝업의 `alwaysOnTop`은 유지하고 `skipTaskbar`만 끈다.
4. 시각 조정과 Tauri 창 설정 변경이므로 별도 단위 테스트는 추가하지 않고 프런트 빌드와 Rust check로 확인한다.

## 구현 내용

- `ui/src/pages/overlay/index.tsx`의 elapsed label inline style을 수정했다.
- 배경, 테두리, border radius, padding, 그림자를 제거해 시간 텍스트만 보이게 변경했다.
- 글자색을 반투명하게 유지해 보조 정보처럼 보이게 조정했다.
- 표시 문구를 `OCR + 결과 ...ms`에서 `... ms`로 줄였다.
- 클릭 방해를 막는 `pointerEvents: "none"`과 좌측 상단 위치는 유지했다.
- `tauri.conf.json`의 popup 창에서 `skipTaskbar`를 `false`로 변경했다.
- popup의 `alwaysOnTop`은 유지해 오버레이/다른 창 위에 뜨는 기존 동작은 보존했다.

## 검증

- `deno task build`
  - 통과
- `cargo check --lib`
  - 통과
  - 실행 중인 프로세스가 일부 런타임 DLL을 점유해 DLL 복사 warning은 발생했지만 컴파일은 성공했다.
