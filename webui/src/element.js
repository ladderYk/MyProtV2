// Element Plus 按需引入
//
// 此前 `app.use(ElementPlus)` 会把全部组件注册进应用, 产物里也就包含全部组件 JS
// 与全部 theme-chalk 样式 (实测首屏单 chunk 1.31MB)。这里改为: 只注册模板中
// 实际用到的组件, 样式也只引入对应组件 —— 未被引用的组件不会进入产物。
//
// 新增组件时: 在 COMPONENTS 与 STYLES 各加一行 (漏加样式会导致组件裸样式)。
// ElMessage 等函数式 API 由各 SFC 直接 import, 不在全局注册范围内。
import {
  ElAlert, ElButton, ElButtonGroup, ElCard, ElCollapse, ElCollapseItem,
  ElConfigProvider, ElDialog, ElDivider, ElDrawer, ElDropdown, ElDropdownItem,
  ElDropdownMenu, ElForm, ElFormItem, ElInput, ElInputNumber, ElOption,
  ElRadioButton, ElRadioGroup, ElSelect, ElSwitch, ElTable, ElTableColumn,
  ElTabPane, ElTabs, ElTag, ElTooltip
} from 'element-plus'

// 每个组件样式入口会自行带入 base.css 与依赖组件样式, 故只需列用到的组件。
import 'element-plus/es/components/alert/style/css'
import 'element-plus/es/components/button/style/css'
import 'element-plus/es/components/button-group/style/css'
import 'element-plus/es/components/card/style/css'
import 'element-plus/es/components/collapse/style/css'
import 'element-plus/es/components/collapse-item/style/css'
import 'element-plus/es/components/config-provider/style/css'
import 'element-plus/es/components/dialog/style/css'
import 'element-plus/es/components/divider/style/css'
import 'element-plus/es/components/drawer/style/css'
import 'element-plus/es/components/dropdown/style/css'
import 'element-plus/es/components/dropdown-item/style/css'
import 'element-plus/es/components/dropdown-menu/style/css'
import 'element-plus/es/components/form/style/css'
import 'element-plus/es/components/form-item/style/css'
import 'element-plus/es/components/input/style/css'
import 'element-plus/es/components/input-number/style/css'
import 'element-plus/es/components/message/style/css'
import 'element-plus/es/components/option/style/css'
import 'element-plus/es/components/radio-button/style/css'
import 'element-plus/es/components/radio-group/style/css'
import 'element-plus/es/components/select/style/css'
import 'element-plus/es/components/switch/style/css'
import 'element-plus/es/components/table/style/css'
import 'element-plus/es/components/table-column/style/css'
import 'element-plus/es/components/tab-pane/style/css'
import 'element-plus/es/components/tabs/style/css'
import 'element-plus/es/components/tag/style/css'
import 'element-plus/es/components/tooltip/style/css'

const COMPONENTS = [
  ElAlert, ElButton, ElButtonGroup, ElCard, ElCollapse, ElCollapseItem,
  ElConfigProvider, ElDialog, ElDivider, ElDrawer, ElDropdown, ElDropdownItem,
  ElDropdownMenu, ElForm, ElFormItem, ElInput, ElInputNumber, ElOption,
  ElRadioButton, ElRadioGroup, ElSelect, ElSwitch, ElTable, ElTableColumn,
  ElTabPane, ElTabs, ElTag, ElTooltip
]

/// 全局注册 (模板使用 kebab-case <el-xxx>); 保留全局注册是为了让
/// installControlledInputFix 仍能覆盖 ElInput/ElSelect。
export function installElement(app) {
  for (const c of COMPONENTS) app.component(c.name, c)
}
