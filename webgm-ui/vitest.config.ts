import { defineConfig } from 'vitest/config'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  // 组件测试要挂 .vue（App.test.ts 直接 mount 页面），这里得跟上 vite.config.ts。
  plugins: [vue()],
  test: {
    environment: 'jsdom',
    globals: true,
  },
})
