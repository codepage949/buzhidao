import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

const LOADING_LABEL = "OCR 모델 로딩 중…";
const LOADING_SUB = "잠시만 기다려 주세요";
const FAILED_LABEL = "OCR 모델 로딩 실패";

type LoadingElements = {
  spinner: HTMLElement;
  errorIcon: HTMLElement;
  label: HTMLElement;
  sub: HTMLElement;
  quitButton: HTMLButtonElement;
};

type LoadingStatusPayload = {
  kind: "loading" | "failed" | string;
  message?: string | null;
};

export function summarizeWarmupError(
  message: string | null | undefined,
): string {
  const normalized = (message ?? "")
    .split(/\r?\n/)
    .map((line) => line.trim())
    .find((line) => line.length > 0);
  if (!normalized) {
    return "알 수 없는 오류";
  }
  if (normalized.startsWith("OCR server 실행 파일을 찾을 수 없습니다:")) {
    return "OCR server 실행 파일을 찾을 수 없습니다";
  }
  return normalized;
}

export function applyWarmupLoadingState(elements: LoadingElements): void {
  elements.spinner.classList.remove("hidden");
  elements.errorIcon.classList.add("hidden");
  elements.label.textContent = LOADING_LABEL;
  elements.sub.textContent = LOADING_SUB;
  elements.quitButton.classList.add("hidden");
}

export function applyWarmupFailedState(
  elements: LoadingElements,
  message: string | null | undefined,
): void {
  elements.spinner.classList.add("hidden");
  elements.errorIcon.classList.remove("hidden");
  elements.label.textContent = FAILED_LABEL;
  elements.sub.textContent = summarizeWarmupError(message);
  elements.quitButton.classList.remove("hidden");
}

function getLoadingElements(): LoadingElements {
  return {
    spinner: document.getElementById("spinner") as HTMLElement,
    errorIcon: document.getElementById("error-icon") as HTMLElement,
    label: document.getElementById("label") as HTMLElement,
    sub: document.getElementById("sub") as HTMLElement,
    quitButton: document.getElementById("quit-btn") as HTMLButtonElement,
  };
}

if (typeof document !== "undefined") {
  const elements = getLoadingElements();

  applyWarmupLoadingState(elements);

  listen<string>("warmup_failed", (event) => {
    applyWarmupFailedState(elements, event.payload);
  });

  listen("warmup_loading", () => {
    applyWarmupLoadingState(elements);
  });

  elements.quitButton.addEventListener("click", () => {
    void invoke("exit_app");
  });

  void invoke<LoadingStatusPayload>("get_loading_status").then((status) => {
    if (status.kind === "failed") {
      applyWarmupFailedState(elements, status.message);
      return;
    }
    applyWarmupLoadingState(elements);
  });
}
