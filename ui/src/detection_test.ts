import { assertEquals } from "@std/assert";
import {
  groupDetections,
  groupDetectionsWithBounds,
  type RawDetection,
} from "./detection.ts";

/// x, y, w, h로 정확한 직사각형 폴리곤을 생성한다.
function det(
  x: number,
  y: number,
  w: number,
  h: number,
  text: string,
): RawDetection {
  return [
    [
      [x, y],
      [x + w, y],
      [x + w, y + h],
      [x, y + h],
    ],
    text,
  ];
}

// ── groupDetections ───────────────────────────────────────────────────────────

Deno.test("groupDetections - 빈 배열이면 빈 결과 반환", () => {
  assertEquals(groupDetections([], "en", 20, 15), []);
});

Deno.test("groupDetections - 단일 탐지는 그대로 반환", () => {
  assertEquals(
    groupDetections([det(10, 10, 80, 20, "Hello")], "en", 20, 15),
    ["Hello"],
  );
});

Deno.test("groupDetections - 같은 줄, X 간격 ≤ xGap → 병합", () => {
  // 두 박스가 같은 Y 범위에 있고 X 간격이 20px 이하
  const ds: RawDetection[] = [
    det(0, 0, 50, 20, "Hello"),
    det(60, 0, 50, 20, "World"), // X 간격 = 60 - 50 = 10 ≤ 20
  ];
  assertEquals(groupDetections(ds, "en", 20, 15), ["Hello World"]);
});

Deno.test("groupDetections - 같은 줄, X 간격 > xGap → 별도 그룹", () => {
  const ds: RawDetection[] = [
    det(0, 0, 50, 20, "Left"),
    det(100, 0, 50, 20, "Right"), // X 간격 = 100 - 50 = 50 > 20
  ];
  assertEquals(groupDetections(ds, "en", 20, 15), ["Left", "Right"]);
});

Deno.test("groupDetections - 같은 줄이면 약간 더 큰 간격도 병합", () => {
  const ds: RawDetection[] = [
    det(0, 0, 60, 22, "Alpha"),
    det(84, 2, 70, 22, "Beta"), // gap=24, height 기반 adaptive gap으로 병합
  ];
  assertEquals(groupDetections(ds, "en", 20, 15), ["Alpha Beta"]);
});

Deno.test("groupDetections - 겹치는 조각도 같은 줄이면 텍스트를 유지", () => {
  const ds: RawDetection[] = [
    det(0, 0, 120, 22, "The quick"),
    det(70, 0, 100, 22, "quick brown"),
    det(150, 0, 80, 22, "fox"),
  ];
  assertEquals(groupDetections(ds, "en", 20, 15), ["The quick quick brown fox"]);
});

Deno.test("groupDetections - 부분 문자열이어도 별도 조각이면 유지", () => {
  const ds: RawDetection[] = [
    det(0, 0, 80, 22, "2026-04"),
    det(84, 0, 40, 22, "04"),
  ];
  assertEquals(groupDetections(ds, "ch", 20, 15), ["2026-0404"]);
});

Deno.test("groupDetections - 인접 줄, Y 간격 ≤ yGap → 병합", () => {
  // 첫 줄 y=0~20, 둘째 줄 y=25~45 → Y 간격 = 25 - 20 = 5 ≤ 15
  const ds: RawDetection[] = [
    det(0, 0, 100, 20, "Line"),
    det(0, 25, 100, 20, "two"),
  ];
  assertEquals(groupDetections(ds, "en", 20, 15), ["Line two"]);
});

Deno.test("groupDetections - 인접 줄, Y 간격 > yGap → 별도 그룹", () => {
  // Y 간격 = 40 - 20 = 20 > 15
  const ds: RawDetection[] = [
    det(0, 0, 100, 20, "Para"),
    det(0, 40, 100, 20, "Two"),
  ];
  assertEquals(groupDetections(ds, "en", 20, 15), ["Para", "Two"]);
});

Deno.test("groupDetections - 빈 문자열만 제외하고 OCR 결과는 유지", () => {
  const ds: RawDetection[] = [
    det(0, 0, 50, 20, "  "),
    det(0, 30, 50, 20, "你好"),
    det(0, 60, 80, 20, "Answer"),
  ];
  assertEquals(groupDetections(ds, "en", 20, 15), ["你好 Answer"]);
});

Deno.test("groupDetections - 중국어는 공백 없이 병합", () => {
  const ds: RawDetection[] = [
    det(0, 0, 40, 20, "你好"),
    det(50, 0, 40, 20, "世界"),
  ];
  assertEquals(groupDetections(ds, "ch", 20, 15), ["你好世界"]);
});

Deno.test("groupDetections - 영어는 단어 사이 공백 삽입", () => {
  const ds: RawDetection[] = [
    det(0, 0, 40, 20, "Hello"),
    det(50, 0, 40, 20, "World"),
  ];
  assertEquals(groupDetections(ds, "en", 20, 15), ["Hello World"]);
});

Deno.test("groupDetections - 입력 순서가 달라도 Y→X 정렬 후 병합", () => {
  // 역순으로 입력 — 정렬 후 올바르게 그루핑되어야 한다
  const ds: RawDetection[] = [
    det(0, 30, 100, 20, "Two"), // 아래 줄
    det(0, 0, 100, 20, "One"), // 위 줄
  ];
  assertEquals(groupDetections(ds, "en", 20, 15), ["One Two"]);
});

Deno.test("groupDetections - 여러 독립 그룹", () => {
  const ds: RawDetection[] = [
    det(0, 0, 50, 20, "Alpha"),
    det(0, 100, 50, 20, "Beta"),
    det(0, 200, 50, 20, "Gamma"),
  ];
  assertEquals(groupDetections(ds, "en", 20, 15), [
    "Alpha",
    "Beta",
    "Gamma",
  ]);
});

Deno.test("groupDetections - 두 컬럼은 별도 그룹 유지", () => {
  // 왼쪽 컬럼 x=0, 오른쪽 컬럼 x=500 — X 간격이 너무 크므로 합치지 않는다
  const ds: RawDetection[] = [
    det(0, 0, 80, 20, "Left"),
    det(500, 0, 80, 20, "Right"),
    det(0, 25, 80, 20, "line"),
    det(500, 25, 80, 20, "two"),
  ];
  assertEquals(groupDetections(ds, "en", 20, 15), [
    "Left line",
    "Right two",
  ]);
});

Deno.test("groupDetections - 오른쪽 컬럼 1행 + 왼쪽 컬럼 2행은 병합하지 않음", () => {
  // 1행에 오른쪽 컬럼만 있고, 2행에 왼쪽 컬럼만 있을 때 잘못 합치지 않아야 한다
  const ds: RawDetection[] = [
    det(500, 0, 80, 20, "Right"),   // x 범위: 500-580
    det(0, 25, 80, 20, "left"),     // x 범위: 0-80 → 범위 안 겹침 → 별도 그룹
  ];
  assertEquals(groupDetections(ds, "en", 20, 15), ["Right", "left"]);
});

// ── groupDetectionsWithBounds ─────────────────────────────────────────────────

Deno.test("groupDetectionsWithBounds - 병합된 bounds는 두 박스를 감싼다", () => {
  const ds: RawDetection[] = [
    det(10, 5, 50, 20, "Hello"),
    det(70, 5, 50, 20, "World"),
  ];
  const [group] = groupDetectionsWithBounds(ds, "en", 20, 15);
  assertEquals(group!.bounds, { x: 10, y: 5, width: 110, height: 20 });
});

Deno.test("groupDetectionsWithBounds - 큰 그룹 안의 중복 부분 그룹은 제거", () => {
  const ds: RawDetection[] = [
    det(10, 10, 220, 24, "The quick brown fox"),
    det(40, 10, 100, 24, "quick brown"),
  ];
  assertEquals(groupDetectionsWithBounds(ds, "en", 20, 15), [
    {
      text: "The quick brown fox",
      bounds: { x: 10, y: 10, width: 220, height: 24 },
    },
  ]);
});

Deno.test("groupDetectionsWithBounds - 텍스트가 달라도 중첩 그룹은 제거", () => {
  const ds: RawDetection[] = [
    det(10, 10, 220, 24, "ABCDE"),
    det(20, 10, 60, 24, "XYZ"),
  ];
  assertEquals(groupDetectionsWithBounds(ds, "ch", 20, 15), [
    {
      text: "ABCDEXYZ",
      bounds: { x: 10, y: 10, width: 220, height: 24 },
    },
  ]);
});
