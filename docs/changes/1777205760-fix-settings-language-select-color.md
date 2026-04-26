# 설정 언어 선택 색상 고정

## 배경

Linux 설정창에서 번역 소스 언어 선택 콤보박스가 Windows와 다르게 흰색으로 보이는 문제가 보고되었다.
언어 선택은 네이티브 `<select>`를 사용하고 있어 WebKitGTK 기본 테마 색상이 앱의 다크 UI와 다르게 적용될 수 있다.

## 계획

1. 언어 선택 `<select>`에 전용 다크 스타일을 명시한다.
2. 드롭다운 항목 `<option>`에도 배경/글자색을 지정한다.
3. 스타일 계약을 단위 테스트로 확인한다.

## 구현

- 언어 선택 `<select>`에 `buildLanguageSelectStyle(textInputStyle)`을 적용했다.
- `buildLanguageSelectStyle`은 기존 입력 스타일을 유지하면서 `colorScheme: "dark"`와 브라우저 기본 appearance 제거를 명시한다.
- `<option>`에도 `languageSelectOptionStyle`을 적용해 드롭다운 항목 배경/글자색을 다크 UI 색상으로 고정했다.
- 새 스타일 계약 테스트를 기본 UI 테스트 태스크에 포함했다.

## 테스트

- `deno test src/pages/settings/language_select_style_test.ts` (`ui/`에서 실행)
- `deno task test` (`ui/`에서 실행)
