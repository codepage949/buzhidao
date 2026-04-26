import { assertEquals } from "@std/assert";
import {
  buildLanguageSelectStyle,
  languageSelectOptionStyle,
} from "./language_select_style.ts";

Deno.test("설정 언어 선택 - 다크 색상 계약을 명시한다", () => {
  const style = buildLanguageSelectStyle({
    background: "#181825",
    color: "#cdd6f4",
  });

  assertEquals(style.background, "#181825");
  assertEquals(style.color, "#cdd6f4");
  assertEquals(style.colorScheme, "dark");
  assertEquals(style.appearance, "none");
  assertEquals(style.WebkitAppearance, "none");
  assertEquals(languageSelectOptionStyle.background, "#181825");
  assertEquals(languageSelectOptionStyle.color, "#cdd6f4");
});
