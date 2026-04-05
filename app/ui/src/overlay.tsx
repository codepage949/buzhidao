import React, { useState, useCallback } from "react";
import { createRoot } from "react-dom/client";
import { listen } from "@tauri-apps/api/event";
import { invoke } from "@tauri-apps/api/core";
import {
  groupDetectionsWithBounds,
  type DetectionGroup,
  type RawDetection,
} from "./detection";
import { useListenerCleanup, useWindowKeydown } from "./app-hooks";

type OcrResultPayload = {
  detections: RawDetection[];
  scale: number;
  orig_width: number;
  orig_height: number;
  source: string;
  x_delta: number;
  y_delta: number;
};

type State =
  | { kind: "hidden" }
  | { kind: "loading" }
  | { kind: "ready"; ocr: OcrResultPayload }
  | { kind: "error"; message: string };

function OverlayApp() {
  const [state, setState] = useState<State>({ kind: "hidden" });
  const [hoveredIdx, setHoveredIdx] = useState<number | null>(null);

  useListenerCleanup(() => [
      listen("overlay_show", () => {
        setState({ kind: "loading" });
        setHoveredIdx(null);
      }),
      listen<OcrResultPayload>("ocr_result", (e) => {
        setState({ kind: "ready", ocr: e.payload });
      }),
      listen<string>("ocr_error", (e) => {
        setState({ kind: "error", message: e.payload });
      }),
    ], []);

  // close_overlay: 오버레이 + 팝업 동시 숨김 (Rust에서 처리)
  const close = useCallback(async () => {
    setState({ kind: "hidden" });
    await invoke("close_overlay");
  }, []);

  useWindowKeydown((e) => {
    if (e.key === "Escape") close();
  }, [close]);

  if (state.kind === "hidden") return null;

  const groups =
    state.kind === "ready"
      ? groupDetectionsWithBounds(
          state.ocr.detections,
          state.ocr.source,
          state.ocr.x_delta,
          state.ocr.y_delta,
        )
      : [];

  const cssScaleX =
    state.kind === "ready" ? window.innerWidth / state.ocr.orig_width : 1;
  const cssScaleY =
    state.kind === "ready" ? window.innerHeight / state.ocr.orig_height : 1;
  const scale = state.kind === "ready" ? state.ocr.scale : 1;

  return (
    // rgba(0,0,0,0.01): 투명 WebView2에서 배경이 없으면 클릭이 창 아래로 통과함
    <div
      onClick={close}
      style={{
        position: "fixed",
        inset: 0,
        background: "rgba(0, 0, 0, 0.01)",
        cursor: "default",
        userSelect: "none",
      }}
    >
      {/* 반투명 어둠 레이어 */}
      <div
        style={{
          position: "absolute",
          inset: 0,
          background: "rgba(20, 20, 20, 0.45)",
          pointerEvents: "none",
        }}
      />

      {/* 닫기 버튼 */}
      <button
        onClick={(e) => {
          e.stopPropagation();
          close();
        }}
        style={{
          position: "absolute",
          top: "16px",
          right: "16px",
          zIndex: 100,
          background: "rgba(30, 30, 46, 0.85)",
          border: "1px solid #45475a",
          color: "#cdd6f4",
          borderRadius: "6px",
          padding: "4px 12px",
          cursor: "pointer",
          fontSize: "13px",
          lineHeight: "1.6",
        }}
      >
        닫기 (ESC)
      </button>

      {/* 로딩 */}
      {state.kind === "loading" && (
        <div
          style={{
            position: "absolute",
            inset: 0,
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            pointerEvents: "none",
          }}
        >
          <div
            style={{
              color: "white",
              fontSize: "20px",
              background: "rgba(0, 0, 0, 0.6)",
              padding: "14px 28px",
              borderRadius: "8px",
            }}
          >
            텍스트 인식 중…
          </div>
        </div>
      )}

      {/* 오류 */}
      {state.kind === "error" && (
        <div
          style={{
            position: "absolute",
            inset: 0,
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            pointerEvents: "none",
          }}
        >
          <div
            style={{
              color: "#ff6b6b",
              fontSize: "18px",
              background: "rgba(0, 0, 0, 0.7)",
              padding: "14px 28px",
              borderRadius: "8px",
            }}
          >
            OCR 오류: {state.message}
          </div>
        </div>
      )}

      {/* OCR 감지 박스 */}
      {groups.map((group: DetectionGroup, i: number) => {
        const cssX = group.bounds.x * scale * cssScaleX;
        const cssY = group.bounds.y * scale * cssScaleY;
        const cssW = group.bounds.width * scale * cssScaleX;
        const cssH = group.bounds.height * scale * cssScaleY;
        const isHovered = hoveredIdx === i;
        return (
          <div
            key={i}
            onClick={(e) => {
              e.stopPropagation();
              // 오버레이는 유지, Rust에서 팝업 위치 지정 + 번역 처리
              invoke("select_text", {
                text: group.text,
                boxX: cssX,
                boxY: cssY,
                boxW: cssW,
                boxH: cssH,
              });
            }}
            onMouseEnter={() => setHoveredIdx(i)}
            onMouseLeave={() => setHoveredIdx(null)}
            title={group.text}
            style={{
              position: "absolute",
              left: `${cssX}px`,
              top: `${cssY}px`,
              width: `${cssW}px`,
              height: `${cssH}px`,
              border: `2px solid ${isHovered ? "#ffff00" : "#00e5ff"}`,
              background: isHovered
                ? "rgba(255, 255, 0, 0.25)"
                : "rgba(0, 229, 255, 0.15)",
              cursor: "pointer",
              transition: "border-color 0.1s, background 0.1s",
            }}
          />
        );
      })}
    </div>
  );
}

createRoot(document.getElementById("root")!).render(<OverlayApp />);
