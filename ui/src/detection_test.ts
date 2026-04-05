import { assertEquals } from "@std/assert";
import {
  groupDetections,
  isSourceLanguage,
  type RawDetection,
} from "./detection.ts";

function detection(
  leftUpper: [number, number],
  leftBottom: [number, number],
  text: string,
): RawDetection {
  return [
    [leftUpper, [0, 0], [0, 0], leftBottom],
    text,
  ];
}

// isSourceLanguage

Deno.test("isSourceLanguage - 설정된 소스 언어와 일치하면 true", () => {
  assertEquals(isSourceLanguage("hello", "en"), true);
  assertEquals(isSourceLanguage("你好", "en"), false);
  assertEquals(isSourceLanguage("你好", "ch"), true);
});

Deno.test("isSourceLanguage - 빈 문자열은 false", () => {
  assertEquals(isSourceLanguage("", "en"), false);
  assertEquals(isSourceLanguage("", "ch"), false);
});

Deno.test("isSourceLanguage - 숫자·기호만 있으면 false", () => {
  assertEquals(isSourceLanguage("123!@#", "en"), false);
  assertEquals(isSourceLanguage("123!@#", "ch"), false);
});

// groupDetections

Deno.test("groupDetections - 빈 배열이면 빈 결과 반환", () => {
  assertEquals(groupDetections([], "en", 10, 10), []);
});

Deno.test("groupDetections - 단일 탐지는 그대로 반환", () => {
  const detections: RawDetection[] = [
    detection([10, 10], [10, 20], "Hello"),
  ];
  assertEquals(groupDetections(detections, "en", 10, 10), ["Hello"]);
});

Deno.test("groupDetections - 인접한 소스 언어 탐지를 병합", () => {
  const detections: RawDetection[] = [
    detection([10, 10], [10, 20], "Hello"),
    detection([11, 22], [11, 32], "World"),
    detection([200, 200], [200, 210], "Ignored"),
  ];

  assertEquals(groupDetections(detections, "en", 10, 10), [
    "HelloWorld",
    "Ignored",
  ]);
});

Deno.test("groupDetections - 비소스 언어 텍스트와 빈 문자열 필터링", () => {
  const detections: RawDetection[] = [
    detection([10, 10], [10, 20], "  "),
    detection([20, 20], [20, 30], "你好"),
    detection([100, 100], [100, 110], "Answer"),
  ];

  assertEquals(groupDetections(detections, "en", 25, 25), ["Answer"]);
});

Deno.test("groupDetections - 임계값 경계 안쪽은 병합", () => {
  // (13-10)²=9 ≤ 10, (23-20)²=9 ≤ 10 → 병합
  const detections: RawDetection[] = [
    detection([10, 10], [10, 20], "A"),
    detection([13, 23], [13, 33], "B"),
  ];
  assertEquals(groupDetections(detections, "en", 10, 10), ["AB"]);
});

Deno.test("groupDetections - 임계값 경계 바깥쪽은 별도 그룹", () => {
  // (14-10)²=16 > 10 → 별도 그룹
  const detections: RawDetection[] = [
    detection([10, 10], [10, 20], "A"),
    detection([14, 24], [14, 34], "B"),
  ];
  assertEquals(groupDetections(detections, "en", 10, 10), ["A", "B"]);
});

Deno.test("groupDetections - 여러 독립 그룹 생성", () => {
  const detections: RawDetection[] = [
    detection([0, 0], [0, 10], "One"),
    detection([100, 100], [100, 110], "Two"),
    detection([200, 200], [200, 210], "Three"),
  ];
  assertEquals(groupDetections(detections, "en", 10, 10), [
    "One",
    "Two",
    "Three",
  ]);
});

Deno.test("groupDetections - 중국어 소스에서 한자만 통과", () => {
  const detections: RawDetection[] = [
    detection([10, 10], [10, 20], "hello"),
    detection([20, 20], [20, 30], "你好"),
    detection([21, 32], [21, 42], "世界"),
  ];
  assertEquals(groupDetections(detections, "ch", 10, 10), ["你好世界"]);
});
