const app = getApp();
const eventBus = require('../../utils/event');
const util = require('../../utils/util');

const PHASES = {
  wait_finger1: '请将手指放在指纹模块上…',
  finger1_ok: '第 1 次采集成功',
  lift_finger: '请移开手指',
  wait_finger2: '请再次按下同一根手指…',
  done: '录入成功',
  canceled: '已取消',
  failed: '录入失败'
};

Page({
  data: {
    list: [],
    enrolling: false,
    enrollText: ''
  },

  onShow() {
    const that = this;
    this._unsubs = [
      eventBus.on('enroll', (m) => this.onEnrollMsg(m)),
      eventBus.on('fplist', (l) => {
        that.setData({
          list: l.map((i) => ({
            id: i.id,
            note: i.note || '',
            tsText: util.formatTime(i.ts)
          }))
        });
      })
    ];
    this.refresh();
  },

  onHide() {
    (this._unsubs || []).forEach((fn) => fn());
    this._unsubs = [];
  },

  refresh() {
    if (app.manager.connected) app.manager.cmd({ type: 'list' });
  },

  onAdd() {
    if (!app.manager.connected) {
      wx.showToast({ title: 'MQTT 未连接', icon: 'none' });
      return;
    }
    if (this.data.enrolling) {
      wx.showToast({ title: '已有录入进行中', icon: 'none' });
      return;
    }
    // 自动分配最小空闲槽位
    const used = this.data.list.map((i) => i.id);
    let id = 0;
    while (used.indexOf(id) >= 0) id++;
    if (id >= 100) {
      wx.showToast({ title: '指纹库已满(100)', icon: 'none' });
      return;
    }
    const that = this;
    wx.showModal({
      title: '录入指纹（槽位 #' + id + '）',
      editable: true,
      placeholderText: '输入备注，如：大门指纹（可留空）',
      success(res) {
        if (!res.confirm) return;
        const note = (res.content || '').trim().slice(0, 24);
        that.setData({ enrolling: true, enrollText: PHASES.wait_finger1 });
        app.manager.cmd({ type: 'enroll', id: id, note: note });
      }
    });
  },

  onEnrollMsg(m) {
    const that = this;
    if (m.phase === 'done' || m.phase === 'failed' || m.phase === 'canceled') {
      const title = m.phase === 'done'
        ? '指纹 #' + m.id + ' 录入成功'
        : (m.phase === 'canceled' ? '已取消录入' : '录入失败：' + (m.note || ''));
      that.setData({ enrolling: false, enrollText: '' });
      wx.showToast({ title: title, icon: m.phase === 'done' ? 'success' : 'none' });
      that.refresh();
    } else {
      that.setData({ enrollText: PHASES[m.phase] || m.phase });
    }
  },

  onCancelEnroll() {
    app.manager.cmd({ type: 'cancel' });
  },

  onRename(e) {
    const id = e.currentTarget.dataset.id;
    const item = this.data.list.find((i) => i.id === id);
    wx.showModal({
      title: '修改备注（#' + id + '）',
      editable: true,
      content: item ? item.note : '',
      placeholderText: '输入新备注',
      success(res) {
        if (res.confirm) {
          const note = (res.content || '').trim().slice(0, 24);
          app.manager.cmd({ type: 'rename', id: id, note: note });
        }
      }
    });
  },

  onDelete(e) {
    const id = e.currentTarget.dataset.id;
    wx.showModal({
      title: '删除指纹 #' + id,
      content: '删除后需要重新录入该指纹，确定删除吗？',
      confirmColor: '#E5484D',
      success(res) {
        if (res.confirm) app.manager.cmd({ type: 'delete', id: id });
      }
    });
  },

  onClearAll() {
    wx.showModal({
      title: '清空所有指纹',
      content: '将删除模块中全部指纹（不可恢复），确定继续？',
      confirmColor: '#E5484D',
      success(res) {
        if (res.confirm) app.manager.cmd({ type: 'clear' });
      }
    });
  }
});
