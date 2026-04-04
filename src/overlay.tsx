import React, { useState, useEffect, useCallback } from "react";
import { createRoot } from "react-dom/client";
import { listen } from "@tauri-apps/api/event";
import { invoke } from "@tauri-apps/api/core";
import { getCurrentWindow } from "@tauri-apps/api/window";
import {
  groupDetectionsWithBounds,
  type DetectionGroup,
  type RawDetection,
} from "./detection";

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

  useEffect(() => {
    const unlistens = [
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
    ];
    return () => {
      unlistens.forEach((p) => p.then((f) => f()));
    };
  }, []);

  const close = useCallback(async () => {
    await getCurrentWindow().hide();
    setState({ kind: "hidden" });
  }, []);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") close();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [close]);

  const handleGroupClick = useCallback(
    (group: DetectionGroup, e: React.MouseEvent) => {
      e.stopPropagation();
      // Rust 측에서 오버레이 숨기기 + 번역을 한 번에 처리.
      // JS에서 hide() 후 invoke()를 하면 WebView2가 서스펜드되어 invoke가 실행 안 됨.
      invoke("select_text", { text: group.text });
    },
    [],
  );

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
      {groups.map((group, i) => {
        const { x, y, width, height } = group.bounds;
        const isHovered = hoveredIdx === i;
        return (
          <div
            key={i}
            onClick={(e) => handleGroupClick(group, e)}
            onMouseEnter={() => setHoveredIdx(i)}
            onMouseLeave={() => setHoveredIdx(null)}
            title={group.text}
            style={{
              position: "absolute",
              left: `${x * scale * cssScaleX}px`,
              top: `${y * scale * cssScaleY}px`,
              width: `${width * scale * cssScaleX}px`,
              height: `${height * scale * cssScaleY}px`,
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
