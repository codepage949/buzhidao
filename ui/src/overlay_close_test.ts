import { assertEquals } from "@std/assert";
import { nextCloseSuppressed } from "./overlay_close.ts";

Deno.test("overlay close - 영역 선택 제출 직후 한 번만 억제한다", () => {
  assertEquals(nextCloseSuppressed(false, "selection_submitted"), true);
  assertEquals(nextCloseSuppressed(true, "root_click_consumed"), false);
});

Deno.test("overlay close - OCR 결과 수신 후에는 첫 빈 클릭을 막지 않는다", () => {
  assertEquals(nextCloseSuppressed(true, "ocr_result"), false);
  assertEquals(nextCloseSuppressed(true, "ocr_error"), false);
});

Deno.test("overlay close - 오버레이 재표시나 영역 선택 진입 시 억제를 초기화한다", () => {
  assertEquals(nextCloseSuppressed(true, "overlay_show"), false);
  assertEquals(nextCloseSuppressed(true, "overlay_select_region"), false);
});
