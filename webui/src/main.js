import { createApp } from 'vue'
// Element Plus 按需注册 (26 个实际使用的组件 + 对应样式), 不再全量 app.use
import { installElement } from './element'
// 样式层顺序: Element Plus 默认 → 设计 token → 全站基础样式
// 不再覆盖 Element Plus 主题, 组件保持官方默认外观
import './styles/tokens.css'
import './style.css'
import App from './App.vue'
import { installControlledInputFix } from './controlled-input-fix'

const app = createApp(App)
// 组件语言由 App.vue 的 <el-config-provider :locale> 下发 (随顶栏中英切换), 无需全局 locale
installElement(app)
// v4.13: 修「键盘输入不落字」— 必须在 installElement 之后覆盖 ElInput/ElSelect 全局注册
installControlledInputFix(app)
app.mount('#app')
