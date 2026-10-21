import { createApp } from 'vue'
import ElementPlus from 'element-plus'
import zhCn from 'element-plus/es/locale/lang/zh-cn'
import 'element-plus/dist/index.css'
// 样式层顺序: Element Plus 默认 → 设计 token → Element Plus 覆盖 → 全站基础样式
import './styles/tokens.css'
import './styles/element.css'
import './style.css'
import App from './App.vue'
import { installControlledInputFix } from './controlled-input-fix'

const app = createApp(App).use(ElementPlus, { locale: zhCn })
// v4.13: 修「键盘输入不落字」— 必须在 use() 之后覆盖 ElInput/ElSelect 全局注册
installControlledInputFix(app)
app.mount('#app')
