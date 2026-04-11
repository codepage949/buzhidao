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

export type DetectionItem = DetectionGroup;
export type DetectionTraceGroup = DetectionGroup & {
  members: DetectionItem[];
};

function sortItemsByReadingOrder(items: DetectionItem[]): DetectionItem[] {
  return [...items].sort((a, b) => a.bounds.y - b.bounds.y || a.bounds.x - b.bounds.x);
}

export function polygonToBounds(polygon: DetectionPolygon): BoundingBox {
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

function intersectionArea(a: BoundingBox, b: BoundingBox): number {
  const x0 = Math.max(a.x, b.x);
  const y0 = Math.max(a.y, b.y);
  const x1 = Math.min(a.x + a.width, b.x + b.width);
  const y1 = Math.min(a.y + a.height, b.y + b.height);
  if (x1 <= x0 || y1 <= y0) return 0;
  return (x1 - x0) * (y1 - y0);
}

function area(box: BoundingBox): number {
  return box.width * box.height;
}

function overlapRatioOfSmaller(a: BoundingBox, b: BoundingBox): number {
  const inter = intersectionArea(a, b);
  if (inter <= 0) return 0;
  return inter / Math.max(1, Math.min(area(a), area(b)));
}

function horizontalGap(a: BoundingBox, b: BoundingBox): number {
  const aRight = a.x + a.width;
  const bRight = b.x + b.width;
  if (aRight < b.x) return b.x - aRight;
  if (bRight < a.x) return a.x - bRight;
  return 0;
}

function verticalCenterDistance(a: BoundingBox, b: BoundingBox): number {
  return Math.abs(a.y + a.height / 2 - (b.y + b.height / 2));
}

function mergeCandidateScore(group: BoundingBox, item: BoundingBox): number {
  return verticalCenterDistance(group, item) * 10000 + horizontalGap(group, item);
}

function deduplicateGroups(groups: DetectionTraceGroup[]): DetectionTraceGroup[] {
  const sorted = [...groups].sort((a, b) => area(b.bounds) - area(a.bounds));
  const result: DetectionTraceGroup[] = [];

  for (const group of sorted) {
    const duplicated = result.some((kept) => {
      const overlap = overlapRatioOfSmaller(group.bounds, kept.bounds);
      if (overlap < 0.9) return false;
      return kept.text.includes(group.text) || group.text.includes(kept.text);
    });
    if (!duplicated) {
      result.push(group);
    }
  }

  return result.sort((a, b) => a.bounds.y - b.bounds.y || a.bounds.x - b.bounds.x);
}

function buildGroupTextFromSortedMembers(members: DetectionItem[], source: string): string {
  if (members.length === 0) return "";

  let text = members[0]!.text;
  for (let i = 1; i < members.length; i++) {
    text = joinText(text, members[i]!.text, source);
  }
  return text;
}

function isNestedDuplicateItem(group: DetectionTraceGroup, item: DetectionItem): boolean {
  return group.members.some((member) => {
    const overlap = overlapRatioOfSmaller(member.bounds, item.bounds);
    if (overlap < 0.9) return false;
    return member.text.includes(item.text) || item.text.includes(member.text);
  });
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
  const maxHeight = Math.max(group.height, item.height);
  const adaptiveXGap = Math.max(xGap, Math.round(Math.min(group.height, item.height) * 1.2));

  // X 범위 인접 여부: 두 박스의 X 범위가 겹치거나 xGap 이내에 있음
  // (item이 group 기준 왼쪽에 있든 오른쪽에 있든 대칭 처리)
  const xNear = item.x <= groupRight + adaptiveXGap &&
    itemRight >= group.x - adaptiveXGap;

  // 같은 줄: Y 범위가 겹침
  const yOverlap = item.y < groupBottom && item.y + item.height > group.y;
  const sameLineByCenter = verticalCenterDistance(group, item) <= maxHeight * 0.6;
  // 인접 줄: 아이템 상단이 그룹 하단 바로 아래
  const yAdjacent = item.y >= groupBottom && item.y <= groupBottom + yGap;
  const sameLineGapMerge = sameLineByCenter && horizontalGap(group, item) <= adaptiveXGap;

  return (xNear && (yOverlap || yAdjacent)) || sameLineGapMerge;
}

function joinText(a: string, b: string, source: string): string {
  if (a === b) return a;
  return source === "en" ? `${a} ${b}` : a + b;
}

export function groupDetectionsWithBounds(
  detections: RawDetection[],
  source: string,
  xGap: number,
  yGap: number,
): DetectionGroup[] {
  return groupDetectionsTraceWithBounds(detections, source, xGap, yGap).map(
    ({ members: _members, ...group }) => group,
  );
}

export function groupDetectionsTraceWithBounds(
  detections: RawDetection[],
  source: string,
  xGap: number,
  yGap: number,
): DetectionTraceGroup[] {
  // 1. OCR 통과 결과는 언어 기준으로 다시 버리지 않고 모두 그룹핑한다.
  // source는 joinText의 공백 규칙에만 사용한다.
  const items = rawDetectionsWithBounds(detections);

  // 2. Y→X 오름차순 정렬 (읽기 순서 보장)
  items.sort((a, b) => a.bounds.y - b.bounds.y || a.bounds.x - b.bounds.x);

  // 3. 그리디 병합
  const groups: DetectionTraceGroup[] = [];
  for (const item of items) {
    let idx = -1;
    let bestScore = Number.POSITIVE_INFINITY;
    for (let i = 0; i < groups.length; i++) {
      const group = groups[i]!;
      if (!canMerge(group.bounds, item.bounds, xGap, yGap)) continue;
      if (isNestedDuplicateItem(group, item)) {
        idx = i;
        bestScore = Number.NEGATIVE_INFINITY;
        break;
      }
      const score = mergeCandidateScore(group.bounds, item.bounds);
      if (score < bestScore) {
        bestScore = score;
        idx = i;
      }
    }
    if (bestScore === Number.NEGATIVE_INFINITY) {
      continue;
    }
    if (idx >= 0) {
      const g = groups[idx]!;
      groups[idx] = {
        text: joinText(g.text, item.text, source),
        bounds: mergeBounds(g.bounds, item.bounds),
        members: [...g.members, item],
      };
    } else {
      groups.push({ text: item.text, bounds: item.bounds, members: [item] });
    }
  }

  return deduplicateGroups(groups).map((group) => {
    const members = sortItemsByReadingOrder(group.members);
    return {
      ...group,
      members,
      text: buildGroupTextFromSortedMembers(members, source),
    };
  });
}

export function rawDetectionsWithBounds(
  detections: RawDetection[],
): DetectionItem[] {
  return detections.flatMap(([polygon, rawText]) => {
    const text = rawText.trim();
    if (!text) return [];
    return [{ text, bounds: polygonToBounds(polygon) }];
  });
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
