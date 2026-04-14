import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));

export default defineConfig({
  plugins: [react()],
  root: "src",
  build: {
    outDir: "../dist",
    emptyOutDir: true,
    rollupOptions: {
      input: {
        popup: resolve(__dirname, "src/popup.html"),
        overlay: resolve(__dirname, "src/overlay.html"),
        loading: resolve(__dirname, "src/loading.html"),
        settings: resolve(__dirname, "src/settings.html"),
      },
    },
  },
  server: {
    port: 1420,
    strictPort: true,
  },
});
