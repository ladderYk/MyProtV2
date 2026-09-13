// v4.13 受控输入适配层 — 修「键盘输入不落字」
//
// 现象: <el-input :model-value="x" @change="..."> 键盘打字时字符瞬间消失, 只有失焦才可能生效。
// 原因: 这种写法只声明了「受控 + 失焦提交」语义, 没有回传 update:modelValue。
//   Element Plus 的输入组件在每次键入后 (nextTick) 会执行 setNativeInputValue(),
//   把 DOM 值强制同步回 modelValue; 而 modelValue 从未更新 → 刚键入的字符被立即抹掉。
//   (佐证: 本仓唯二能正常输入的字段, 恰好是唯二带 @update:model-value 的两处)
//
// 方案: 在全局注册层包一层「本地草稿」——键入写入 draft (DOM 保持用户输入),
//   失焦/选择时照旧 emit('change'), 其余事件与属性原样透传。
//   已用 v-model / @update:model-value 的调用点行为不变 (正常向上回传)。
//
// 用法: 必须在 app.use(ElementPlus) 之后调用 installControlledInputFix(app),
//   以覆盖 Element 的全局注册 (本仓各组件均使用全局注册, 无局部 import)。

import { ElInput, ElSelect } from 'element-plus'
import { defineComponent, h, nextTick, ref, watch } from 'vue'

function withDraft(Original) {
  return defineComponent({
    name: 'Fixed' + (Original.name || 'Control'),
    inheritAttrs: false,
    props: { modelValue: { default: undefined } },
    emits: ['update:modelValue', 'change'],
    setup(props, { attrs, emit, slots }) {
      const draft = ref(props.modelValue)
      // 外部模型变化 (加载/重置/其它字段联动) 时同步草稿
      watch(() => props.modelValue, v => { if (v !== draft.value) draft.value = v })
      return () => h(Original, {
        ...attrs,
        modelValue: draft.value,
        'onUpdate:modelValue': v => {
          draft.value = v                 // 键入立即反映到 DOM, 不被回滚
          emit('update:modelValue', v)    // v-model 调用点照常工作
        },
        onChange: v => {
          emit('change', v)               // 失焦提交: 原有 @change 逻辑不变
          // 父层若未接受本次修改 (校验失败/忽略), 下一拍把显示拉回真实模型值
          nextTick(() => {
            if (props.modelValue !== draft.value) draft.value = props.modelValue
          })
        }
      }, slots)
    }
  })
}

export function installControlledInputFix(app) {
  app.component('ElInput', withDraft(ElInput))
  app.component('ElSelect', withDraft(ElSelect))
}
