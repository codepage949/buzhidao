import { assertEquals } from "@std/assert";
import { summarizeWarmupError } from "./index.ts";

Deno.test("로딩 - 웜업 실패 메시지는 첫 줄만 요약해서 표시한다", () => {
  assertEquals(
    summarizeWarmupError("OCR warmup 실패\ntraceback line 1\ntraceback line 2"),
    "OCR warmup 실패",
  );
});

Deno.test("로딩 - 빈 웜업 실패 메시지는 기본 문구로 대체한다", () => {
  assertEquals(summarizeWarmupError("  \n "), "알 수 없는 오류");
  assertEquals(summarizeWarmupError(undefined), "알 수 없는 오류");
});
