import { assertEquals } from "jsr:@std/assert";
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

Deno.test("isSourceLanguage matches configured source", () => {
  assertEquals(isSourceLanguage("hello", "en"), true);
  assertEquals(isSourceLanguage("你好", "en"), false);
  assertEquals(isSourceLanguage("你好", "ch"), true);
});

Deno.test("groupDetections merges nearby source-language detections", () => {
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

Deno.test("groupDetections filters non-source language text and empties", () => {
  const detections: RawDetection[] = [
    detection([10, 10], [10, 20], "  "),
    detection([20, 20], [20, 30], "你好"),
    detection([100, 100], [100, 110], "Answer"),
  ];

  assertEquals(groupDetections(detections, "en", 25, 25), ["Answer"]);
});
