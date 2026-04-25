export function measureOverlayOcrElapsedMs(
  startedAtMs: number | null,
  nowMs: number,
): number | null {
  if (startedAtMs === null) {
    return null;
  }
  const elapsedMs = nowMs - startedAtMs;
  if (!Number.isFinite(elapsedMs) || elapsedMs < 0) {
    return null;
  }
  return Math.round(elapsedMs);
}
