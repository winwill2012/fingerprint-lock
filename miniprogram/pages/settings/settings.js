const app = getApp();
const eventBus = require('../../utils/event');
const managerMod = require('../../utils/mqtt-manager');

Page({
  data: {
    host: '',
    port: '',
    tls: false,
    path: '/mqtt',
    username: '',
    password: '',
    key: '',
    cmdTopic: '',
    respTopic: '',
    connected: false,
    statusText: '未连接'
  },

  onShow() {
    const s = app.manager.loadSettings();
    this.setData({
      host: s.host,
      port: String(s.port),
      tls: !!s.tls,
      path: s.path || '/mqtt',
      username: s.username || '',
      password: s.password || '',
      key: s.key || '',
      cmdTopic: s.cmdTopic,
      respTopic: s.respTopic,
      connected: app.manager.connected
    });
    this._unsubs = [
      eventBus.on('mqtt', (d) => {
        this.setData({
          connected: !!d.connected,
          statusText: d.connected ? '已连接' : (d.error || '未连接')
        });
      })
    ];
  },

  onHide() {
    (this._unsubs || []).forEach((fn) => fn());
    this._unsubs = [];
  },

  onInput(e) {
    this.setData({ [e.currentTarget.dataset.field]: e.detail.value });
  },

  onTls(e) {
    this.setData({ tls: !!e.detail.value });
  },

  onSave() {
    const d = this.data;
    if (!d.host) {
      wx.showToast({ title: '请填写 MQTT 地址', icon: 'none' });
      return;
    }
    const cmdTopic = (d.cmdTopic || 'lock/esp32c3/cmd').trim();
    app.manager.saveSettings({
      host: d.host.trim(),
      port: parseInt(d.port, 10) || 8083,
      tls: !!d.tls,
      path: d.path || '/mqtt',
      username: d.username.trim(),
      password: d.password,
      key: d.key.trim(),
      cmdTopic: cmdTopic,
      respTopic: (d.respTopic || cmdTopic + '/resp').trim()
    });
    wx.showToast({ title: '已保存', icon: 'success' });
  },

  onConnect() {
    app.manager.connect();
  },

  onDisconnect() {
    app.manager.disconnect();
  }
});
