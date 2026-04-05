import React, { useState, useEffect } from "react";
import { createRoot } from "react-dom/client";
import { listen } from "@tauri-apps/api/event";
import { invoke } from "@tauri-apps/api/core";
import ReactMarkdown from "react-markdown";

type Status = "idle" | "translating" | "done" | "error";

function PopupApp() {
  const [status, setStatus] = useState<Status>("idle");
  const [content, setContent] = useState<string>("");

  useEffect(() => {
    const unlistens = [
      listen("translating", () => {
        setStatus("translating");
        setContent("");
      }),
      listen<string>("translation_result", (e) => {
        setStatus("done");
        setContent(e.payload);
      }),
      listen<string>("translation_error", (e) => {
        setStatus("error");
        setContent(e.payload);
      }),
    ];
    return () => {
      unlistens.forEach((p) => p.then((f) => f()));
    };
  }, []);

  const close = () => invoke("close_popup");

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") close();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  return (
    <div
      style={{
        width: "100%",
        height: "100%",
        display: "flex",
        flexDirection: "column",
        background: "#1e1e2e",
        color: "#cdd6f4",
        fontFamily:
          "'Segoe UI', 'Noto Sans KR', sans-serif",
        border: "1px solid #45475a",
        borderRadius: "8px",
        overflow: "hidden",
      }}
    >
      {/* 헤더 (닫기 버튼) */}
      <div
        style={{
          display: "flex",
          alignItems: "center",
          justifyContent: "space-between",
          padding: "6px 10px",
          background: "#181825",
          flexShrink: 0,
          userSelect: "none",
        }}
      >
        <span
          style={{ fontSize: "12px", color: "#6c7086" }}
        >
          번역 결과
        </span>
        <button
          onClick={close}
          style={{
            background: "none",
            border: "none",
            color: "#6c7086",
            cursor: "pointer",
            fontSize: "14px",
            lineHeight: 1,
            padding: "2px 4px",
            borderRadius: "4px",
          }}
          onMouseEnter={(e) =>
            ((e.currentTarget as HTMLButtonElement).style.color = "#cdd6f4")
          }
          onMouseLeave={(e) =>
            ((e.currentTarget as HTMLButtonElement).style.color = "#6c7086")
          }
        >
          ✕
        </button>
      </div>

      {/* 내용 — 상하 스크롤만 허용 */}
      <div
        style={{
          flex: 1,
          overflowY: "auto",
          overflowX: "hidden",
          padding: "14px 16px",
        }}
      >
        {status === "idle" && (
          <p style={{ color: "#6c7086", fontSize: "13px" }}>
            OCR 영역을 클릭하면 번역이 시작됩니다.
          </p>
        )}
        {status === "translating" && (
          <p style={{ color: "#89b4fa", fontSize: "13px" }}>번역 중…</p>
        )}
        {status === "error" && (
          <p style={{ color: "#f38ba8", fontSize: "13px" }}>{content}</p>
        )}
        {status === "done" && (
          <div className="markdown-body">
            <ReactMarkdown>{content}</ReactMarkdown>
          </div>
        )}
      </div>

      <style>{`
        .markdown-body { font-size: 14px; line-height: 1.7; }
        .markdown-body p { margin-bottom: 0.6em; }
        .markdown-body h1, .markdown-body h2, .markdown-body h3 {
          margin: 0.8em 0 0.4em; font-weight: 600;
        }
        .markdown-body code {
          background: #313244; border-radius: 3px;
          padding: 1px 4px; font-size: 0.9em;
        }
        .markdown-body pre {
          background: #313244; border-radius: 6px;
          padding: 10px; overflow-x: auto; margin-bottom: 0.6em;
        }
        .markdown-body pre code { background: none; padding: 0; }
        .markdown-body ul, .markdown-body ol {
          padding-left: 1.4em; margin-bottom: 0.6em;
        }
        .markdown-body blockquote {
          border-left: 3px solid #45475a; padding-left: 10px;
          color: #a6adc8; margin-bottom: 0.6em;
        }
        ::-webkit-scrollbar { width: 6px; }
        ::-webkit-scrollbar-track { background: transparent; }
        ::-webkit-scrollbar-thumb { background: #45475a; border-radius: 3px; }
      `}</style>
    </div>
  );
}

createRoot(document.getElementById("root")!).render(<PopupApp />);
