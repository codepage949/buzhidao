import type React from "react";

export const LANGUAGE_SELECT_BACKGROUND = "#181825";
export const LANGUAGE_SELECT_TEXT = "#cdd6f4";

export function buildLanguageSelectStyle(
  baseStyle: React.CSSProperties,
): React.CSSProperties {
  return {
    ...baseStyle,
    colorScheme: "dark",
    appearance: "none",
    WebkitAppearance: "none",
  };
}

export const languageSelectOptionStyle: React.CSSProperties = {
  background: LANGUAGE_SELECT_BACKGROUND,
  color: LANGUAGE_SELECT_TEXT,
};
