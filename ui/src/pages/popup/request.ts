export function shouldApplyPopupTranslationEvent(
  activeRequestId: number | null,
  eventRequestId: number,
): boolean {
  return activeRequestId !== null && activeRequestId === eventRequestId;
}
