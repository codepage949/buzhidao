import { assertEquals } from "@std/assert";
import { shouldApplyPopupTranslationEvent } from "./request.ts";

Deno.test("팝업 번역 이벤트 - 활성 요청만 반영한다", () => {
  assertEquals(shouldApplyPopupTranslationEvent(3, 3), true);
  assertEquals(shouldApplyPopupTranslationEvent(3, 2), false);
  assertEquals(shouldApplyPopupTranslationEvent(null, 1), false);
});
