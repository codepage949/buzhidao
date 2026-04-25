import { assertEquals } from "@std/assert";
import { measureOverlayOcrElapsedMs } from "./timing.ts";

Deno.test("로딩_시작_시각이_없으면_경과시간도_null이다", () => {
  assertEquals(measureOverlayOcrElapsedMs(null, 1000), null);
});

Deno.test("유효한_시각차를_반올림한_ms로_반환한다", () => {
  assertEquals(measureOverlayOcrElapsedMs(1000, 1234.6), 235);
});

Deno.test("음수_경과시간은_null로_버린다", () => {
  assertEquals(measureOverlayOcrElapsedMs(1500, 1200), null);
});
