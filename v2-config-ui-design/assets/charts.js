(function() {
  var style = getComputedStyle(document.documentElement);
  var accent = style.getPropertyValue('--accent').trim();
  var accent2 = style.getPropertyValue('--accent2').trim();
  var ink = style.getPropertyValue('--ink').trim();
  var muted = style.getPropertyValue('--muted').trim();
  var rule = style.getPropertyValue('--rule').trim();
  var bg2 = style.getPropertyValue('--bg2').trim();

  // --- Chart: 阶段相对工作量 ---
  var el = document.getElementById('chart-effort');
  if (el) {
    var chart = echarts.init(el, null, { renderer: 'svg' });
    chart.setOption({
      animation: false,
      grid: { left: 60, right: 40, top: 30, bottom: 40 },
      tooltip: {
        trigger: 'axis',
        appendToBody: true,
        axisPointer: { type: 'shadow' },
        formatter: function(params) {
          var p = params[0];
          return p.name + '<br/>相对工作量：<b>' + p.value + ' 人日</b>';
        }
      },
      xAxis: { type: 'category', data: ['M1 后端', 'M2 schema', 'M3 前端', 'M4 加固测试'], axisLine: { lineStyle: { color: rule } }, axisLabel: { color: ink } },
      yAxis: { type: 'value', name: '人日', nameTextStyle: { color: muted }, axisLine: { lineStyle: { color: rule } }, axisLabel: { color: muted }, splitLine: { lineStyle: { color: rule } } },
      series: [{
        type: 'bar',
        data: [4, 3, 5, 3],
        barWidth: 42,
        itemStyle: { color: accent, borderRadius: [6, 6, 0, 0] },
        label: { show: true, position: 'top', color: ink, fontWeight: 'bold' }
      }]
    });
    window.addEventListener('resize', function() { chart.resize(); });
  }
})();