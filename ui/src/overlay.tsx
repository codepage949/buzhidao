import React, { useState, useCallback, useEffect, useMemo, useRef } from "react";
import { createRoot } from "react-dom/client";
import { listen } from "@tauri-apps/api/event";
import { invoke } from "@tauri-apps/api/core";
import {
  groupDetectionsTraceWithBounds,
  type DetectionTraceGroup,
  type RawDetection,
} from "./detection";
import { nextCloseSuppressed } from "./overlay_close";
import { selectionOutcome, type SelectionRect } from "./overlay_selection";
import { useListenerCleanup, useWindowKeydown } from "./app-hooks";

type OcrResultPayload = {
  detections: RawDetection[];
  debug_detections: [RawDetection[0], string, number, boolean][];
  orig_width: number;
  orig_height: number;
  source: string;
  word_gap: number;
  line_gap: number;
  debug_trace: boolean;
};

type State =
  | { kind: "hidden" }
  | { kind: "loading" }
  | { kind: "selecting" }
  | { kind: "ready"; ocr: OcrResultPayload }
  | { kind: "error"; message: string };

function OverlayApp() {
  const [state, setState] = useState<State>({ kind: "hidden" });
  const [hoveredIdx, setHoveredIdx] = useState<number | null>(null);
  const [selectionStart, setSelectionStart] = useState<[number, number] | null>(null);
  const [selectionRect, setSelectionRect] = useState<SelectionRect | null>(null);
  const suppressNextCloseRef = useRef(false);
  const selectionResumeStateRef = useRef<State | null>(null);

  useListenerCleanup(() => [
      listen("overlay_show", () => {
        setState({ kind: "loading" });
        setHoveredIdx(null);
        setSelectionStart(null);
        setSelectionRect(null);
        selectionResumeStateRef.current = null;
        suppressNextCloseRef.current = nextCloseSuppressed(
          suppressNextCloseRef.current,
          "overlay_show",
        );
      }),
      listen("overlay_select_region", () => {
        setState({ kind: "selecting" });
        setHoveredIdx(null);
        setSelectionStart(null);
        setSelectionRect(null);
        selectionResumeStateRef.current = null;
        suppressNextCloseRef.current = nextCloseSuppressed(
          suppressNextCloseRef.current,
          "overlay_select_region",
        );
      }),
      listen<OcrResultPayload>("ocr_result", (e) => {
        setState({ kind: "ready", ocr: e.payload });
        setSelectionStart(null);
        setSelectionRect(null);
        selectionResumeStateRef.current = null;
        suppressNextCloseRef.current = nextCloseSuppressed(
          suppressNextCloseRef.current,
          "ocr_result",
        );
      }),
      listen<string>("ocr_error", (e) => {
        setState({ kind: "error", message: e.payload });
        setSelectionStart(null);
        setSelectionRect(null);
        selectionResumeStateRef.current = null;
        suppressNextCloseRef.current = nextCloseSuppressed(
          suppressNextCloseRef.current,
          "ocr_error",
        );
      }),
    ], []);

  // close_overlay: 오버레이 + 팝업 동시 숨김 (Rust에서 처리)
  const close = useCallback(async () => {
    setState({ kind: "hidden" });
    await invoke("close_overlay");
  }, []);

  const handleRootClick = useCallback(async () => {
    if (suppressNextCloseRef.current) {
      suppressNextCloseRef.current = nextCloseSuppressed(
        suppressNextCloseRef.current,
        "root_click_consumed",
      );
      return;
    }
    if (state.kind === "selecting") return;
    await close();
  }, [close, state.kind]);

  useWindowKeydown((e) => {
    if (e.key === "Escape") close();
  }, [close]);

  const beginSelection = useCallback((e: React.MouseEvent<HTMLDivElement>) => {
    if (
      state.kind !== "selecting" &&
      state.kind !== "ready" &&
      state.kind !== "error"
    ) {
      return;
    }
    e.stopPropagation();
    const x = e.clientX;
    const y = e.clientY;
    selectionResumeStateRef.current =
      state.kind === "ready" || state.kind === "error" ? state : null;
    if (state.kind !== "selecting") {
      setState({ kind: "selecting" });
    }
    setSelectionStart([x, y]);
    setSelectionRect({ x, y, width: 0, height: 0 });
    suppressNextCloseRef.current = nextCloseSuppressed(
      suppressNextCloseRef.current,
      "selection_started",
    );
  }, [state]);

  const updateSelection = useCallback((e: React.MouseEvent<HTMLDivElement>) => {
    if (state.kind !== "selecting" || !selectionStart) return;
    const [startX, startY] = selectionStart;
    const x1 = Math.min(startX, e.clientX);
    const y1 = Math.min(startY, e.clientY);
    const x2 = Math.max(startX, e.clientX);
    const y2 = Math.max(startY, e.clientY);
    setSelectionRect({
      x: x1,
      y: y1,
      width: x2 - x1,
      height: y2 - y1,
    });
  }, [selectionStart, state.kind]);

  const finishSelection = useCallback(async (e: React.MouseEvent<HTMLDivElement>) => {
    if (state.kind !== "selecting") return;
    e.stopPropagation();
    const rect = selectionRect;
    setSelectionStart(null);
    switch (selectionOutcome(rect)) {
      case "close":
        selectionResumeStateRef.current = null;
        setSelectionRect(null);
        await close();
        return;
      case "resume": {
        setSelectionRect(null);
        const resumeState = selectionResumeStateRef.current;
        if (resumeState) {
          setState(resumeState);
          selectionResumeStateRef.current = null;
        }
        return;
      }
      case "submit":
        break;
    }

    selectionResumeStateRef.current = null;
    suppressNextCloseRef.current = nextCloseSuppressed(
      suppressNextCloseRef.current,
      "selection_submitted",
    );
    setState({ kind: "loading" });
    try {
      await invoke("run_region_ocr", {
        rectX: rect.x,
        rectY: rect.y,
        rectW: rect.width,
        rectH: rect.height,
        viewportW: window.innerWidth,
        viewportH: window.innerHeight,
      });
    } catch (err) {
      setState({ kind: "error", message: String(err) });
    }
  }, [close, selectionRect, state.kind]);

  const groups = useMemo(
    () =>
      state.kind === "ready"
        ? groupDetectionsTraceWithBounds(
            state.ocr.detections,
            state.ocr.source,
            state.ocr.word_gap,
            state.ocr.line_gap,
          )
        : [],
    [state],
  );
  const rawItems = useMemo(
    () =>
      state.kind === "ready"
        ? state.ocr.debug_detections.map(([polygon, text, score, accepted]) => {
            let minX = Number.POSITIVE_INFINITY;
            let minY = Number.POSITIVE_INFINITY;
            let maxX = Number.NEGATIVE_INFINITY;
            let maxY = Number.NEGATIVE_INFINITY;
            for (const [x, y] of polygon) {
              if (x < minX) minX = x;
              if (y < minY) minY = y;
              if (x > maxX) maxX = x;
              if (y > maxY) maxY = y;
            }
            return {
              text: `${accepted ? "ok" : "ng"} ${score.toFixed(3)} ${text}`,
              bounds: {
                x: minX,
                y: minY,
                width: maxX - minX,
                height: maxY - minY,
              },
            };
          })
        : [],
    [state],
  );

  useEffect(() => {
    if (state.kind !== "ready" || !state.ocr.debug_trace) return;
    console.log("[OCR][overlay] raw items", rawItems);
    console.log("[OCR][overlay] grouped items", groups);
  }, [groups, rawItems, state]);

  if (state.kind === "hidden") return null;

  const cssScaleX =
    state.kind === "ready" ? window.innerWidth / state.ocr.orig_width : 1;
  const cssScaleY =
    state.kind === "ready" ? window.innerHeight / state.ocr.orig_height : 1;

  return (
    // rgba(0,0,0,0.01): 투명 WebView2에서 배경이 없으면 클릭이 창 아래로 통과함
    <div
      onClick={handleRootClick}
      onMouseDown={beginSelection}
      onMouseMove={updateSelection}
      onMouseUp={finishSelection}
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
        onMouseDown={(e) => e.stopPropagation()}
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

      {state.kind === "selecting" && (
        <div
          style={{
            position: "absolute",
            inset: 0,
            pointerEvents: "none",
          }}
        >
          <div
            style={{
              position: "absolute",
              top: "24px",
              left: "50%",
              transform: "translateX(-50%)",
              color: "#ffffff",
              fontSize: "18px",
              background: "rgba(0, 0, 0, 0.68)",
              padding: "12px 18px",
              borderRadius: "8px",
              border: "1px solid rgba(255,255,255,0.18)",
            }}
          >
            OCR할 영역을 드래그해서 선택하세요
          </div>
          {selectionRect && (
            <div
              style={{
                position: "absolute",
                left: `${selectionRect.x}px`,
                top: `${selectionRect.y}px`,
                width: `${selectionRect.width}px`,
                height: `${selectionRect.height}px`,
                border: "2px solid #00e5ff",
                background: "rgba(0, 229, 255, 0.12)",
                boxShadow: "0 0 0 99999px rgba(0, 0, 0, 0.35)",
                boxSizing: "border-box",
              }}
            />
          )}
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
      {state.kind === "ready" && state.ocr.debug_trace &&
        rawItems.map((item, i) => {
          const cssX = item.bounds.x * cssScaleX;
          const cssY = item.bounds.y * cssScaleY;
          const cssW = item.bounds.width * cssScaleX;
          const cssH = item.bounds.height * cssScaleY;
          return (
            <div
              key={`raw-${i}`}
            >
              <div
                title={`raw: ${item.text}`}
                style={{
                  position: "absolute",
                  left: `${cssX}px`,
                  top: `${cssY}px`,
                  width: `${cssW}px`,
                  height: `${cssH}px`,
                  border: "1px dashed rgba(255, 64, 128, 0.95)",
                  background: "rgba(255, 64, 128, 0.08)",
                  pointerEvents: "none",
                  boxSizing: "border-box",
                }}
              />
              <div
                style={{
                  position: "absolute",
                  left: `${cssX}px`,
                  top: `${Math.max(cssY - 18, 0)}px`,
                  maxWidth: "220px",
                  padding: "1px 4px",
                  fontSize: "11px",
                  lineHeight: "1.2",
                  color: "#ffffff",
                  background: "rgba(160, 30, 90, 0.92)",
                  border: "1px solid rgba(255, 64, 128, 0.95)",
                  borderRadius: "3px",
                  pointerEvents: "none",
                  whiteSpace: "normal",
                  wordBreak: "break-all",
                  boxSizing: "border-box",
                }}
                title={item.text}
              >
                {item.text}
              </div>
            </div>
          );
        })}
      {groups.map((group: DetectionTraceGroup, i: number) => {
        const cssX = group.bounds.x * cssScaleX;
        const cssY = group.bounds.y * cssScaleY;
        const cssW = group.bounds.width * cssScaleX;
        const cssH = group.bounds.height * cssScaleY;
        const isHovered = hoveredIdx === i;
        return (
          <div key={i}>
            <div
              onClick={(e) => {
                e.stopPropagation();
                // 오버레이는 유지, Rust에서 팝업 위치 지정 + 번역 처리
                invoke("select_text", {
                  text: group.text,
                  boxX: cssX,
                  boxY: cssY,
                  boxW: cssW,
                });
              }}
              onMouseEnter={() => setHoveredIdx(i)}
              onMouseLeave={() => setHoveredIdx(null)}
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
            {isHovered && (
              <div
                style={{
                  position: "absolute",
                  left: `${cssX}px`,
                  top: `${Math.max(cssY - 56, 0)}px`,
                  maxWidth: "480px",
                  padding: "6px 8px",
                  fontSize: "12px",
                  lineHeight: "1.35",
                  color: "#ffffff",
                  background: "rgba(15, 23, 42, 0.96)",
                  border: "1px solid rgba(148, 163, 184, 0.9)",
                  borderRadius: "6px",
                  boxShadow: "0 8px 24px rgba(0, 0, 0, 0.35)",
                  pointerEvents: "none",
                  whiteSpace: "normal",
                  wordBreak: "break-word",
                  boxSizing: "border-box",
                  zIndex: 50,
                }}
              >
                {group.text}
              </div>
            )}
            {state.kind === "ready" && state.ocr.debug_trace && (
              <div
                style={{
                  position: "absolute",
                  left: `${cssX}px`,
                  top: `${Math.max(cssY - 18, 0)}px`,
                  maxWidth: "360px",
                  padding: "1px 4px",
                  fontSize: "11px",
                  lineHeight: "1.2",
                  color: "#ffffff",
                  background: "rgba(0, 120, 150, 0.9)",
                  border: "1px solid rgba(0, 229, 255, 0.9)",
                  borderRadius: "3px",
                  pointerEvents: "none",
                  whiteSpace: "normal",
                  wordBreak: "break-all",
                  boxSizing: "border-box",
                }}
                title={group.text}
              >
                {group.text}
                {"\n"}
                {group.members.map((member) => member.text).join(" | ")}
              </div>
            )}
          </div>
        );
      })}
    </div>
  );
}

createRoot(document.getElementById("root")!).render(<OverlayApp />);
