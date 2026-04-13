export type SelectionRect = {
  x: number;
  y: number;
  width: number;
  height: number;
};

export type SelectionOutcome = "close" | "resume" | "submit";

export function selectionOutcome(
  rect: SelectionRect | null,
): SelectionOutcome {
  if (!rect) {
    return "resume";
  }
  if (rect.width === 0 && rect.height === 0) {
    return "close";
  }
  if (rect.width < 8 || rect.height < 8) {
    return "resume";
  }
  return "submit";
}
