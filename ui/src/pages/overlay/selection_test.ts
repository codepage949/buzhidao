import { assertEquals } from "@std/assert";
import { selectionOutcome } from "./selection.ts";

Deno.test("선택 결과 - 선택이 없으면 결과 상태로 복귀한다", () => {
  assertEquals(selectionOutcome(null), "resume");
});

Deno.test("선택 결과 - 한 점 클릭이면 오버레이를 닫는다", () => {
  assertEquals(
    selectionOutcome({ x: 10, y: 20, width: 0, height: 0 }),
    "close",
  );
});

Deno.test("선택 결과 - 작은 사각형이면 결과 상태로 복귀한다", () => {
  assertEquals(
    selectionOutcome({ x: 10, y: 20, width: 7, height: 12 }),
    "resume",
  );
  assertEquals(
    selectionOutcome({ x: 10, y: 20, width: 12, height: 7 }),
    "resume",
  );
});

Deno.test("선택 결과 - 충분한 사각형이면 OCR을 실행한다", () => {
  assertEquals(
    selectionOutcome({ x: 10, y: 20, width: 8, height: 8 }),
    "submit",
  );
});
