// 变量编辑区子字段说明 (VariableConfig 的 source/value/strategy/params/expr/enum/...)
// 这些概念不属"注册表字段" (注册表只覆盖 协议/设备/标签 三层的顶层字段),
// 故在此集中维护; 与 Config_Schema §3.2 的措辞保持一致.
export const VAR_HELP = {
  source: '变量来源声明。static = 固定值/纯 UI 提示(不进变量池); auto = 运行时按 strategy 自动计算',
  value: 'source=static 时注入变量池的默认值 (标签同名变量可覆盖); 留空 = 仅作 UI 提示',
  strategy: 'auto 的算法: autoIncrement(自增计数) / frameSlice(按已渲染帧切片) / expr(算术表达式) / crc(校验和)',
  params: 'strategy 的参数对象, 如 autoIncrement 的 {"seed": 1}; 键名见 Config_Schema §3.2',
  expr: '派生长度表达式, 只用于 outputs。可引用 inputs 名与结构原语: {名称:len}(载荷字节数)、{Frame:fixed}(模板固定段宽度)、{名称:offset}(占位符偏移)',
  enum: 'UI 下拉候选集: 元素为裸数字或 {value,label} 对象; value 须为整数且不重复',
  label: '显示名 (仅 UI 展示, 引擎不读取)',
  unit: '单位 (仅 UI 展示)',
  placeholder: '输入框占位提示 (仅 UI 展示)'
}
