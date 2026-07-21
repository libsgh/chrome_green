import tailwindcss from "@tailwindcss/vite";
import vue from "@vitejs/plugin-vue";
import { defineConfig } from "vite";
import { viteSingleFile } from "vite-plugin-singlefile";

export default defineConfig({
  plugins: [vue(), tailwindcss(), viteSingleFile()],
  base: "./",
  build: {
    assetsInlineLimit: 100000,
  },
  server: {
    // Proxy /api requests to the chrome_green HTTP server.
    // This allows local dev (npm run dev) to call the chrome_green API
    // without CORS issues. The dev server (e.g. localhost:5173) forwards
    // /api/* to the running instance. The target port defaults to 8090 but
    // can be overridden with CG_DEV_PORT (each install binds a per-install
    // port derived from its path — see GetConfigPort in appid.cc).
    host: "0.0.0.0",
    proxy: {
      "/api": {
        target: "http://127.0.0.1:" + (process.env.CG_DEV_PORT || "8090"),
        changeOrigin: true,
      },
    },
  },
});
