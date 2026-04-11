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

/// 두 bounding box가 병합 가능한지 판단한다.
/// xGap: 같은 줄/인접 줄에서 허용할 최대 수평 간격 (픽셀)
/// yGap: 두 줄 간 허용할 최대 수직 간격 (픽셀)
function canMerge(
  group: BoundingBox,
  item: BoundingBox,
  xGap: number,
  yGap: number,
): boolean {
  const groupBottom = group.y + group.height;
  const groupRight = group.x + group.width;
  const itemRight = item.x + item.width;

  // X 범위 인접 여부: 두 박스의 X 범위가 겹치거나 xGap 이내에 있음
  // (item이 group 기준 왼쪽에 있든 오른쪽에 있든 대칭 처리)
  const xNear = item.x <= groupRight + xGap && itemRight >= group.x - xGap;

  // 같은 줄: Y 범위가 겹침
  const yOverlap = item.y < groupBottom && item.y + item.height > group.y;
  // 인접 줄: 아이템 상단이 그룹 하단 바로 아래
  const yAdjacent = item.y >= groupBottom && item.y <= groupBottom + yGap;

  return xNear && (yOverlap || yAdjacent);
}

function joinText(a: string, b: string, source: string): string {
  return source === "en" ? `${a} ${b}` : a + b;
}

export function groupDetectionsWithBounds(
  detections: RawDetection[],
  source: string,
  xGap: number,
  yGap: number,
): DetectionGroup[] {
  // 1. 소스 언어 필터 + bounds 계산
  const items = detections.flatMap(([polygon, rawText]) => {
    const text = rawText.trim();
    if (!text || !isSourceLanguage(text, source)) return [];
    return [{ text, bounds: polygonToBounds(polygon) }];
  });

  // 2. Y→X 오름차순 정렬 (읽기 순서 보장)
  items.sort(
    (a, b) => a.bounds.y - b.bounds.y || a.bounds.x - b.bounds.x,
  );

  // 3. 그리디 병합
  const groups: DetectionGroup[] = [];
  for (const item of items) {
    const idx = groups.findIndex((g) =>
      canMerge(g.bounds, item.bounds, xGap, yGap)
    );
    if (idx >= 0) {
      const g = groups[idx]!;
      groups[idx] = {
        text: joinText(g.text, item.text, source),
        bounds: mergeBounds(g.bounds, item.bounds),
      };
    } else {
      groups.push({ text: item.text, bounds: item.bounds });
    }
  }

  return groups;
}

export function groupDetections(
  detections: RawDetection[],
  source: string,
  xGap: number,
  yGap: number,
): string[] {
  return groupDetectionsWithBounds(detections, source, xGap, yGap).map(
    (g) => g.text,
  );
}
