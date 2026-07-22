import path from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig, loadEnv } from "vite";
import vue from "@vitejs/plugin-vue";
import { viteSingleFile } from "vite-plugin-singlefile";

import { mockDevice } from "./mock/plugin.js";

/** Always resolve `.env*` next to this config, not `process.cwd()` (running Vite from repo root breaks the latter). */
const configDir = path.dirname(fileURLToPath(import.meta.url));

const proxyRoute = (target, extra = {}) => ({
  target,
  changeOrigin: true,
  ...extra,
});

const wsRoute = (target) =>
  proxyRoute(target, {
    ws: true,
    /* Some embedded WS stacks reject Origin: http://localhost:5173 */
    rewriteWsOrigin: true,
    configure(proxy) {
      proxy.on("proxyReqWs", (proxyReq) => {
        /* http-proxy may forward Connection: close; WS upgrade requires Upgrade. */
        proxyReq.setHeader("Connection", "Upgrade");
      });
    },
  });

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, configDir, "");
  const target =
    process.env.ESPKVM_PROXY_TARGET ||
    env.ESPKVM_PROXY_TARGET ||
    "http://10.42.0.151";

  /* `npm run dev:mock` serves a simulated device instead of proxying to a real
     one, so the interface can be developed and reviewed without hardware. */
  const useMock = mode === "mock";

  return {
    plugins: useMock ? [vue(), mockDevice()] : [vue(), viteSingleFile()],
    /* Strip the parts of the Vue runtime this console never uses. The bundle
       lives in flash, so dead code costs real space on the device. */
    define: {
      __VUE_OPTIONS_API__: "false",
      __VUE_PROD_DEVTOOLS__: "false",
      __VUE_PROD_HYDRATION_MISMATCH_DETAILS__: "false",
    },
    build: {
      outDir: "dist",
      emptyOutDir: true,
      /* Everything must end up in one file: the firmware embeds a single blob
         and the device has no second request to spare. */
      assetsInlineLimit: 100000000,
      /* Terser incorrectly folded queueMouseAbs (dropped rAF coalescing). esbuild is fine here. */
      minify: "esbuild",
      cssMinify: true,
    },
    server: useMock
      ? {}
      : {
          proxy: {
            "/stream": proxyRoute(target, { timeout: 0 }),
            "/api": proxyRoute(target),
            /* Both websockets, or dev mode has input but no picture. */
            "/ws": wsRoute(target),
            "/video": wsRoute(target),
          },
        },
  };
});
