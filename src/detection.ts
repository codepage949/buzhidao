export type Point = [number, number];
export type DetectionPolygon = [Point, Point, Point, Point];
export type RawDetection = [DetectionPolygon, string];

export type BoundingBox = {
  x: number;
  y: number;
  width: number;
  height: number;
};

export type DetectionGroup = {
  text: string;
  bounds: BoundingBox;
};

type DetectionBucket = {
  anchor: Point;
  text: string;
  bounds: BoundingBox;
};

export function isSourceLanguage(text: string, source: string): boolean {
  return source === "en" ? /[a-zA-Z]/.test(text) : /[\u4e00-\u9fa5]/.test(text);
}

function polygonToBounds(polygon: DetectionPolygon): BoundingBox {
  const xs = polygon.map(([x]) => x);
  const ys = polygon.map(([, y]) => y);
  const x = Math.min(...xs);
  const y = Math.min(...ys);
  return { x, y, width: Math.max(...xs) - x, height: Math.max(...ys) - y };
}

function mergeBounds(a: BoundingBox, b: BoundingBox): BoundingBox {
  const x = Math.min(a.x, b.x);
  const y = Math.min(a.y, b.y);
  return {
    x,
    y,
    width: Math.max(a.x + a.width, b.x + b.width) - x,
    height: Math.max(a.y + a.height, b.y + b.height) - y,
  };
}

export function groupDetectionsWithBounds(
  detections: RawDetection[],
  source: string,
  xDelta: number,
  yDelta: number,
): DetectionGroup[] {
  const buckets: DetectionBucket[] = [];

  for (const detection of detections) {
    const [polygon, rawText] = detection;
    const [leftUpper, , , leftBottom] = polygon;
    const text = rawText.trim();

    if (!text || !isSourceLanguage(text, source)) {
      continue;
    }

    const detBounds = polygonToBounds(polygon);

    const nearIndex = buckets.findIndex(({ anchor: [x1, y1] }) => {
      const [x2, y2] = leftUpper;
      return (x2 - x1) ** 2 <= xDelta && (y2 - y1) ** 2 <= yDelta;
    });

    if (nearIndex >= 0) {
      const bucket = buckets[nearIndex]!;
      buckets[nearIndex] = {
        anchor: leftBottom,
        text: bucket.text + text,
        bounds: mergeBounds(bucket.bounds, detBounds),
      };
      continue;
    }

    buckets.push({ anchor: leftBottom, text, bounds: detBounds });
  }

  return buckets.map(({ text, bounds }) => ({ text, bounds }));
}

export function groupDetections(
  detections: RawDetection[],
  source: string,
  xDelta: number,
  yDelta: number,
): string[] {
  return groupDetectionsWithBounds(detections, source, xDelta, yDelta).map(
    (g) => g.text,
  );
}
