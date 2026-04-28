export type BoundingBox = {
  x: number;
  y: number;
  width: number;
  height: number;
};

export type RawDetection = [BoundingBox, string];

export type DetectionGroup = {
  text: string;
  bounds: BoundingBox;
};

export type DetectionItem = DetectionGroup;
export type DetectionTraceGroup = DetectionGroup & {
  members: DetectionItem[];
};
