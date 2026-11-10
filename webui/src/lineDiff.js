// 行级 diff: 回滚备份前预览「会丢什么 / 会回来什么」
// 纯函数, 不依赖 Vue — 由 tests/lineDiff.test.js 覆盖边界

const GAP_CONTEXT = 6          // 折叠时保留的上下文行数
const MAX_CELLS = 4000000      // LCS 表格上限 (配置通常几十~几百行, 超出退化为整体替换)

/// 空文本视为 0 行 (而不是 1 个空行), 否则空配置会凭空多出一条 same
function splitLines(text) {
  const v = String(text == null ? '' : text).split(/\r?\n/)
  return v.length === 1 && v[0] === '' ? [] : v
}

/// 逐行比较 a(当前) 与 b(目标), 返回 [{kind:'same'|'add'|'del', text}]
export function diffLines(aText, bText) {
  const a = splitLines(aText)
  const b = splitLines(bText)
  const n = a.length
  const m = b.length
  if (n * m > MAX_CELLS) {
    return a.map(text => ({ kind: 'del', text })).concat(b.map(text => ({ kind: 'add', text })))
  }
  // dp[i][j] = a[i..] 与 b[j..] 的最长公共子序列长度
  const dp = new Array(n + 1)
  for (let i = 0; i <= n; i++) dp[i] = new Int32Array(m + 1)
  for (let i = n - 1; i >= 0; i--) {
    for (let j = m - 1; j >= 0; j--) {
      dp[i][j] = a[i] === b[j] ? dp[i + 1][j + 1] + 1 : Math.max(dp[i + 1][j], dp[i][j + 1])
    }
  }
  const rows = []
  let i = 0
  let j = 0
  while (i < n && j < m) {
    if (a[i] === b[j]) { rows.push({ kind: 'same', text: a[i] }); i++; j++ }
    else if (dp[i + 1][j] >= dp[i][j + 1]) { rows.push({ kind: 'del', text: a[i] }); i++ }
    else { rows.push({ kind: 'add', text: b[j] }); j++ }
  }
  while (i < n) rows.push({ kind: 'del', text: a[i++] })
  while (j < m) rows.push({ kind: 'add', text: b[j++] })
  return rows
}

/// 连续的相同行超过 13 行时折叠中段, 只留首尾各 6 行上下文
/// 折叠行形如 {kind:'gap', text: 被隐藏的行数}
export function collapseSame(rows) {
  const out = []
  let i = 0
  while (i < rows.length) {
    if (rows[i].kind !== 'same') { out.push(rows[i]); i++; continue }
    let j = i
    while (j < rows.length && rows[j].kind === 'same') j++
    const run = j - i
    if (run > GAP_CONTEXT * 2 + 1) {
      for (let k = i; k < i + GAP_CONTEXT; k++) out.push(rows[k])
      out.push({ kind: 'gap', text: run - GAP_CONTEXT * 2 })
      for (let k = j - GAP_CONTEXT; k < j; k++) out.push(rows[k])
    } else {
      for (let k = i; k < j; k++) out.push(rows[k])
    }
    i = j
  }
  return out
}
