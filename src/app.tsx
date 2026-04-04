import React, { useState, useEffect } from "react";
import { createRoot } from "react-dom/client";
import { listen } from "@tauri-apps/api/event";

type Status = "idle" | "capturing" | "translating" | "error";

function App() {
  const [translation, setTranslation] = useState<string>(
    "PrtSc 를 눌러 번역할 영역을 선택하세요.",
  );
  const [status, setStatus] = useState<Status>("idle");

  useEffect(() => {
    const unlisteners = [
      listen<string>("translation_result", (e) => {
        setTranslation(e.payload);
        setStatus("idle");
      }),
      listen<string>("translation_error", (e) => {
        setTranslation(`오류: ${e.payload}`);
        setStatus("error");
      }),
      listen("capturing", () => setStatus("capturing")),
      listen("translating", () => setStatus("translating")),
    ];

    return () => {
      unlisteners.forEach((p) => p.then((f) => f()));
    };
  }, []);

  const statusLabel: Record<Status, string> = {
    idle: "",
    capturing: "화면 캡처 중…",
    translating: "번역 중…",
    error: "",
  };

  return (
    <div
      style={{
        display: "flex",
        flexDirection: "column",
        height: "100%",
        padding: "14px",
        gap: "8px",
      }}
    >
      {statusLabel[status] && (
        <div style={{ fontSize: "12px", color: "#888" }}>
          {statusLabel[status]}
        </div>
      )}
      <div
        style={{
          flex: 1,
          fontSize: "14px",
          lineHeight: "1.6",
          whiteSpace: "pre-wrap",
          overflowY: "auto",
          color: status === "error" ? "#ff6b6b" : "#f0f0f0",
        }}
      >
        {translation}
      </div>
    </div>
  );
}

createRoot(document.getElementById("root")!).render(<App />);
