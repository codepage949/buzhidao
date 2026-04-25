import { assertEquals } from "@std/assert";
import {
  canSaveSettings,
  getSettingsFooterMessage,
  OCR_BUSY_MESSAGE,
} from "./save_state.ts";

Deno.test("설정 저장 상태 - OCR 진행 중이면 저장할 수 없다", () => {
  assertEquals(canSaveSettings(false, true), false);
});

Deno.test("설정 저장 상태 - 저장 중이면 OCR 유휴 상태여도 저장할 수 없다", () => {
  assertEquals(canSaveSettings(true, false), false);
});

Deno.test("설정 저장 상태 - 유휴 상태이고 저장 중이 아니면 저장할 수 있다", () => {
  assertEquals(canSaveSettings(false, false), true);
});

Deno.test("설정 저장 상태 - OCR 진행 중에는 안내 문구를 표시한다", () => {
  assertEquals(getSettingsFooterMessage("", "", true), OCR_BUSY_MESSAGE);
});

Deno.test("설정 저장 상태 - 오류가 있으면 OCR 안내보다 오류를 우선 표시한다", () => {
  assertEquals(
    getSettingsFooterMessage("필수 항목을 입력하세요.", "저장 중입니다.", true),
    "필수 항목을 입력하세요.",
  );
});
