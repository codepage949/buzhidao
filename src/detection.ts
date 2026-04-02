export type Point = [number, number];
export type DetectionPolygon = [Point, Point, Point, Point];
export type RawDetection = [DetectionPolygon, string];

type DetectionBucket = {
  anchor: Point;
  text: string;
};

export function isSourceLanguage(text: string, source: string): boolean {
  return source === "en" ? /[a-zA-Z]/.test(text) : /[\u4e00-\u9fa5]/.test(text);
}

export function groupDetections(
  detections: RawDetection[],
  source: string,
  xDelta: number,
  yDelta: number,
): string[] {
  const buckets: DetectionBucket[] = [];

  for (const detection of detections) {
    const [polygon, rawText] = detection;
    const [leftUpper, , , leftBottom] = polygon;
    const text = rawText.trim();

    if (!text || !isSourceLanguage(text, source)) {
      continue;
    }

    const nearIndex = buckets.findIndex(({ anchor: [x1, y1] }) => {
      const [x2, y2] = leftUpper;

      return (x2 - x1) ** 2 <= xDelta && (y2 - y1) ** 2 <= yDelta;
    });

    if (nearIndex >= 0) {
      const bucket = buckets[nearIndex];
      buckets[nearIndex] = {
        anchor: leftBottom,
        text: bucket.text + text,
      };
      continue;
    }

    buckets.push({ anchor: leftBottom, text });
  }

  return buckets.map(({ text }) => text);
}
