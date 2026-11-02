// 通用对象工具 (原先在 TagsForm / ProtocolForm 各有一份实现)

/// 深拷贝: 配置文档只含 JSON 可表达的值, JSON 往返足够且最省事
export function clone(v) {
  return JSON.parse(JSON.stringify(v))
}

export function hasOwn(obj, key) {
  return !!obj && Object.prototype.hasOwnProperty.call(obj, key)
}
