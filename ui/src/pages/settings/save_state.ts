export const OCR_BUSY_MESSAGE = "OCR 진행 중에는 설정을 저장할 수 없습니다.";

export function canSaveSettings(saving: boolean, ocrBusy: boolean): boolean {
  return !saving && !ocrBusy;
}

export function getSettingsFooterMessage(
  error: string,
  status: string,
  ocrBusy: boolean,
): string {
  if (error) {
    return error;
  }
  if (ocrBusy) {
    return OCR_BUSY_MESSAGE;
  }
  return status;
}
