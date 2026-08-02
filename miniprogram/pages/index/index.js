const app = getApp();
const eventBus = require('../../utils/event');
const util = require('../../utils/util');

Page({
  data: {
    connected: false,
    deviceOnline: false,
    status: null,
    batteryText: '',
    lastEvent: ''
  },

  onShow() {
    const that = this;
    this._unsubs = [
      eventBus.on('mqtt', (d) => {
        that.setData({ connected: !!d.connected });
      }),
      eventBus.on('status', (s) => {
        that.applyStatus(s);
      }),
      eventBus.on('unlock', (m) => {
        const src = m.source === 'finger' ? '指纹识别' : '远程开锁';
        const id = m.fp_id >= 0 ? ('#' + m.fp_id + ' ') : '';
        that.setData({ lastEvent: id + src + ' 于 ' + util.nowText() });
      })
    ];
    // 拉取内存中的最新状态
    const st = app.manager.status;
    if (st) this.applyStatus(st);
    this.setData({
      connected: app.manager.connected,
      deviceOnline: app.manager.deviceOnline
    });
  },

  onHide() {
    (this._unsubs || []).forEach((fn) => fn());
    this._unsubs = [];
  },

  applyStatus(s) {
    let batteryText = '';
    if (s && s.battery_mv) {
      batteryText = (s.battery_pct || 0) + '% · ' + (s.battery_mv / 1000).toFixed(2) + 'V';
    }
    this.setData({
      status: s,
      batteryText: batteryText,
      deviceOnline: !!s.online,
      connected: app.manager.connected
    });
  },

  onUnlock() {
    if (!app.manager.connected) {
      wx.showToast({ title: 'MQTT 未连接', icon: 'none' });
      return;
    }
    const that = this;
    wx.showModal({
      title: '确认开锁',
      content: '确定要向设备下发远程开锁指令吗？',
      confirmColor: '#1B66E9',
      success(res) {
        if (res.confirm) {
          app.manager.cmd({ type: 'unlock' });
          that.setData({ lastEvent: '远程开锁指令已发送 ' + util.nowText() });
          wx.showToast({ title: '指令已发送', icon: 'success' });
        }
      }
    });
  },

  onStatus() {
    app.manager.cmd({ type: 'status' });
    wx.showToast({ title: '已请求状态', icon: 'none' });
  }
});
