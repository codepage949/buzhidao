import React, { useEffect, useState } from "react";
import { createRoot } from "react-dom/client";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { useListenerCleanup, useWindowKeydown } from "./app-hooks";

type Source = "en" | "ch";
type Device = "cpu" | "gpu";

type UserSettings = {
  source: Source;
  score_thresh: number;
  ocr_server_device: Device;
  ai_gateway_api_key: string;
  ai_gateway_model: string;
  system_prompt: string;
  word_gap: number;
  line_gap: number;
  capture_shortcut: string;
};

type SaveUserSettingsResult = {
  restart_required: boolean;
};

type GetUserSettingsResult = {
  settings: UserSettings;
  show_ocr_server_device: boolean;
};

type SettingsNoticePayload = {
  message: string;
  missing_fields: string[];
};

type HighlightableField = "ai_gateway_api_key" | "ai_gateway_model";

const formCardStyle: React.CSSProperties = {
  background: "#1e1e2e",
  border: "1px solid #45475a",
  borderRadius: "8px",
  padding: "20px 20px 18px",
  boxShadow: "0 12px 28px rgba(0, 0, 0, 0.28)",
};

function numberFromInput(value: string, fallback: number) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function RadioRow<T extends string>({
  name,
  value,
  onChange,
  options,
}: {
  name: string;
  value: T;
  onChange: (next: T) => void;
  options: Array<{ value: T; label: string }>;
}) {
  return (
    <div style={{ display: "flex", gap: "10px", flexWrap: "wrap" }}>
      {options.map((option) => {
        const active = option.value === value;
        return (
          <label
            key={option.value}
            style={{
              display: "inline-flex",
              alignItems: "center",
              gap: "8px",
              padding: "8px 12px",
              borderRadius: "6px",
              border: active ? "1px solid #89b4fa" : "1px solid #45475a",
              background: active ? "#313244" : "#181825",
              color: active ? "#cdd6f4" : "#a6adc8",
              cursor: "pointer",
              fontSize: "14px",
              fontWeight: 600,
            }}
          >
            <input
              type="radio"
              name={name}
              checked={active}
              onChange={() => onChange(option.value)}
            />
            {option.label}
          </label>
        );
      })}
    </div>
  );
}

function Field({
  label,
  hint,
  emphasized = false,
  children,
}: {
  label: string;
  hint?: string;
  emphasized?: boolean;
  children: React.ReactNode;
}) {
  return (
    <label style={{ display: "grid", gap: "10px" }}>
      <div style={{ display: "grid", gap: "4px" }}>
        <span
          style={{
            color: emphasized ? "#f9e2af" : "#cdd6f4",
            fontSize: "15px",
            fontWeight: 700,
          }}
        >
          {label}
        </span>
        {hint ? (
          <span style={{ color: "#a6adc8", fontSize: "12px", lineHeight: 1.5 }}>{hint}</span>
        ) : null}
      </div>
      {children}
    </label>
  );
}

function SettingsApp() {
  const [settings, setSettings] = useState<UserSettings | null>(null);
  const [showOcrServerDevice, setShowOcrServerDevice] = useState(false);
  const [status, setStatus] = useState<string>("설정을 불러오는 중입니다.");
  const [error, setError] = useState<string>("");
  const [saving, setSaving] = useState(false);
  const [highlightedFields, setHighlightedFields] = useState<HighlightableField[]>([]);

  function missingRequiredFields(next: UserSettings): HighlightableField[] {
    const missing: HighlightableField[] = [];
    if (!next.ai_gateway_api_key.trim()) {
      missing.push("ai_gateway_api_key");
    }
    if (!next.ai_gateway_model.trim()) {
      missing.push("ai_gateway_model");
    }
    return missing;
  }

  function isHighlighted(field: HighlightableField) {
    return highlightedFields.includes(field);
  }

  useEffect(() => {
    let cancelled = false;
    invoke<GetUserSettingsResult>("get_user_settings")
      .then((payload) => {
        if (cancelled) return;
        setSettings(payload.settings);
        setShowOcrServerDevice(payload.show_ocr_server_device);
        setHighlightedFields(missingRequiredFields(payload.settings));
        setStatus("");
      })
      .catch((err) => {
        if (cancelled) return;
        setError(String(err));
        setStatus("");
      });
    return () => {
      cancelled = true;
    };
  }, []);

  useListenerCleanup(
    () => [
      listen<SettingsNoticePayload>("settings_notice", (event) => {
        setError(event.payload.message);
        setHighlightedFields(event.payload.missing_fields as HighlightableField[]);
        setStatus("");
      }),
      getCurrentWindow().onCloseRequested(async (event) => {
        event.preventDefault();
        await getCurrentWindow().hide();
      }),
    ],
    [],
  );

  useWindowKeydown((event) => {
    if (event.key === "Escape" && !saving) {
      void getCurrentWindow().hide();
    }
  }, [saving]);

  async function save() {
    if (!settings || saving) return;
    const missing = missingRequiredFields(settings);
    if (missing.length > 0) {
      setHighlightedFields(missing);
      setError("필수 항목을 입력하세요.");
      setStatus("");
      return;
    }
    setSaving(true);
    setError("");
    setStatus("설정을 저장하는 중입니다.");
    try {
      const result = await invoke<SaveUserSettingsResult>("save_user_settings", { settings });
      setHighlightedFields([]);
      setStatus(
        result.restart_required
          ? "저장되었습니다. OCR 장치 변경은 다음 앱 실행부터 적용됩니다."
          : "저장되었습니다.",
      );
      await getCurrentWindow().hide();
    } catch (err) {
      setError(String(err));
      setStatus("");
    } finally {
      setSaving(false);
    }
  }

  if (!settings) {
    return (
      <div style={rootStyle}>
        <div style={shellStyle}>
          <div style={heroStyle}>
            <h1 style={heroTitleStyle}>설정</h1>
            <p style={heroBodyStyle}>{error || status}</p>
          </div>
        </div>
      </div>
    );
  }

  return (
    <div style={rootStyle}>
      <div style={shellStyle}>
        <section style={{ ...formCardStyle, display: "grid", gap: "18px" }}>
          <Field label="번역 소스 언어">
            <RadioRow
              name="source"
              value={settings.source}
              onChange={(source) => setSettings({ ...settings, source })}
              options={[
                { value: "en", label: "영어" },
                { value: "ch", label: "중국어" },
              ]}
            />
          </Field>

          <Field
            label={`OCR 점수 임계값 (${settings.score_thresh.toFixed(2)})`}
            hint="낮추면 더 많은 텍스트를 통과시키고, 높이면 오탐을 더 강하게 걸러냅니다."
          >
            <input
              type="range"
              min="0"
              max="1"
              step="0.01"
              value={settings.score_thresh}
              onChange={(event) =>
                setSettings({
                  ...settings,
                  score_thresh: numberFromInput(event.currentTarget.value, settings.score_thresh),
                })
              }
            />
          </Field>

          {showOcrServerDevice ? (
            <Field
              label="OCR 장치"
              hint="저장은 즉시 되지만 OCR 서버 재시작은 하지 않습니다. 다음 앱 실행 때 적용됩니다."
            >
              <RadioRow
                name="device"
                value={settings.ocr_server_device}
                onChange={(ocr_server_device) => setSettings({ ...settings, ocr_server_device })}
                options={[
                  { value: "cpu", label: "CPU" },
                  { value: "gpu", label: "GPU" },
                ]}
              />
            </Field>
          ) : null}

          <Field label="AI Gateway API Key" emphasized={isHighlighted("ai_gateway_api_key")}>
            <input
              type="password"
              value={settings.ai_gateway_api_key}
              onChange={(event) => {
                const next = { ...settings, ai_gateway_api_key: event.currentTarget.value };
                setSettings(next);
                setHighlightedFields(missingRequiredFields(next));
              }}
              style={inputStyle(isHighlighted("ai_gateway_api_key"))}
            />
          </Field>

          <Field label="AI Gateway Model" emphasized={isHighlighted("ai_gateway_model")}>
            <input
              type="text"
              value={settings.ai_gateway_model}
              onChange={(event) => {
                const next = { ...settings, ai_gateway_model: event.currentTarget.value };
                setSettings(next);
                setHighlightedFields(missingRequiredFields(next));
              }}
              style={inputStyle(isHighlighted("ai_gateway_model"))}
            />
          </Field>

          <div style={gridTwoStyle}>
            <Field label="단어 간격" hint="단어 박스 병합 간격입니다.">
              <input
                type="number"
                min="0"
                value={settings.word_gap}
                onChange={(event) =>
                  setSettings({
                    ...settings,
                    word_gap: numberFromInput(event.currentTarget.value, settings.word_gap),
                  })
                }
                style={textInputStyle}
              />
            </Field>

            <Field label="줄 간격" hint="줄 병합 간격입니다.">
              <input
                type="number"
                min="0"
                value={settings.line_gap}
                onChange={(event) =>
                  setSettings({
                    ...settings,
                    line_gap: numberFromInput(event.currentTarget.value, settings.line_gap),
                  })
                }
                style={textInputStyle}
              />
            </Field>
          </div>

          <Field
            label="캡처 단축키"
            hint="예: Ctrl+Alt+A, Cmd+Shift+A. 수식키(Ctrl/Alt/Shift/Cmd)를 포함한 조합만 허용합니다. 저장하면 즉시 반영됩니다."
          >
            <input
              type="text"
              value={settings.capture_shortcut}
              onChange={(event) =>
                setSettings({ ...settings, capture_shortcut: event.currentTarget.value })
              }
              placeholder="Ctrl+Alt+A"
              style={textInputStyle}
            />
          </Field>

          <Field label="System Prompt">
            <textarea
              value={settings.system_prompt}
              onChange={(event) =>
                setSettings({ ...settings, system_prompt: event.currentTarget.value })
              }
              rows={8}
              style={{ ...textInputStyle, resize: "vertical", minHeight: "180px" }}
            />
          </Field>

          <div
            style={{
              display: "flex",
              justifyContent: "space-between",
              alignItems: "center",
              gap: "16px",
              flexWrap: "wrap",
            }}
          >
            <div style={{ minHeight: "22px", color: error ? "#f38ba8" : "#a6adc8", fontSize: "13px" }}>
              {error || status}
            </div>
            <div style={{ display: "flex", gap: "10px", flexWrap: "wrap" }}>
              <button type="button" onClick={() => void save()} disabled={saving} style={primaryButtonStyle}>
                {saving ? "저장 중..." : "저장"}
              </button>
            </div>
          </div>
        </section>
      </div>
    </div>
  );
}

const rootStyle: React.CSSProperties = {
  minHeight: "100vh",
  background:
    "linear-gradient(180deg, #181825 0%, #1e1e2e 100%)",
  padding: "0",
  boxSizing: "border-box",
  fontFamily: "'Segoe UI', 'Noto Sans KR', sans-serif",
};

const shellStyle: React.CSSProperties = {
  width: "100%",
  maxWidth: "100%",
  margin: "0",
  display: "grid",
  gap: "0",
};

const heroStyle: React.CSSProperties = {
  display: "grid",
  gap: "8px",
  color: "#cdd6f4",
  padding: "4px 2px",
};

const eyebrowStyle: React.CSSProperties = {
  margin: 0,
  textTransform: "uppercase",
  letterSpacing: "0.14em",
  fontSize: "12px",
  fontWeight: 800,
  color: "#89b4fa",
};

const heroTitleStyle: React.CSSProperties = {
  margin: 0,
  fontSize: "32px",
  lineHeight: 1.15,
  fontWeight: 800,
};

const heroBodyStyle: React.CSSProperties = {
  margin: 0,
  color: "#a6adc8",
  fontSize: "15px",
  lineHeight: 1.6,
};

const textInputStyle: React.CSSProperties = {
  width: "100%",
  boxSizing: "border-box",
  borderRadius: "6px",
  border: "1px solid #45475a",
  background: "#181825",
  color: "#cdd6f4",
  padding: "10px 12px",
  fontSize: "14px",
  lineHeight: 1.5,
};

function inputStyle(emphasized: boolean): React.CSSProperties {
  return emphasized
    ? {
        ...textInputStyle,
        border: "1px solid #f9e2af",
        boxShadow: "0 0 0 1px rgba(249, 226, 175, 0.25)",
      }
    : textInputStyle;
}

const gridTwoStyle: React.CSSProperties = {
  display: "grid",
  gridTemplateColumns: "repeat(auto-fit, minmax(220px, 1fr))",
  gap: "16px",
};

const secondaryButtonStyle: React.CSSProperties = {
  borderRadius: "6px",
  border: "1px solid #45475a",
  background: "#181825",
  color: "#cdd6f4",
  padding: "10px 16px",
  fontSize: "14px",
  fontWeight: 700,
  cursor: "pointer",
};

const primaryButtonStyle: React.CSSProperties = {
  borderRadius: "6px",
  border: "1px solid #6c7086",
  background: "#313244",
  color: "#cdd6f4",
  padding: "10px 18px",
  fontSize: "14px",
  fontWeight: 800,
  cursor: "pointer",
};

createRoot(document.getElementById("root")!).render(<SettingsApp />);
