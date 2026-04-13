export type OverlayCloseEvent =
  | "overlay_show"
  | "overlay_select_region"
  | "selection_submitted"
  | "ocr_result"
  | "ocr_error"
  | "root_click_consumed";

export function nextCloseSuppressed(
  current: boolean,
  event: OverlayCloseEvent,
): boolean {
  switch (event) {
    case "overlay_show":
    case "overlay_select_region":
    case "ocr_result":
    case "ocr_error":
    case "root_click_consumed":
      return false;
    case "selection_submitted":
      return true;
    default:
      return current;
  }
}
