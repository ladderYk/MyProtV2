import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 开发模式: 前端跑在 5173, API 代理到 C++ WebApi (127.0.0.1:8080)
// 生产模式: npm run build → dist/ 由 C++ WebApiServer 直接托管 (webApi.webRoot)
export default defineConfig({
  plugins: [vue()],
  // 相对路径 base: 生产部署到子路径 (如 http://host/myprot/) 时仍能正确加载
  // 资产; 默认 '/' 在非根路径部署下会 404。Localhost 部署不受影响。
  base: './',
  server: {
    port: 5173,
    proxy: {
      '/api': 'http://127.0.0.1:8080',
      '/health': 'http://127.0.0.1:8080'
    }
  }
})

