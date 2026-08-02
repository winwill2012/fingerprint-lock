/**
 * MQTT 全局管理器：
 * - 持有唯一的 MqttClient 实例，自动重连
 * - 负责消息分发：状态 / 开锁事件 / 录入进度 / 指纹列表 / 操作结果
 * - 提供 cmd() 统一封装指令下发（自动携带 ts、可选 key）
 */
const MqttClient = require('./mqtt');
const eventBus = require('./event');
const util = require('./util');

const DEFAULTS = {
  host: 'broker.emqx.io',
  port: 8083,
  tls: false,
  path: '/mqtt',
  username: '',
  password: '',
  key: '',                                   // 可选命令密钥，需与固件 MQTT_CMD_KEY 一致
  cmdTopic: 'lock/esp32c3/cmd',              // 指令下发 Topic（小程序 -> 设备）
  respTopic: 'lock/esp32c3/cmd/resp',        // 状态反馈 Topic（设备 -> 小程序）
  autoConnect: true
};

const SETTINGS_KEY = 'mqttSettings';

class MqttManager {
  constructor() {
    this.client = null;
    this.connected = false;
    this.connecting = false;
    this.deviceOnline = false;   // 设备是否在线（区别于本端 MQTT 连接）
    this.status = null;          // 最近一次设备状态
    this.fpList = [];            // 指纹列表
    this._started = false;
  }

  loadSettings() {
    try {
      return Object.assign({}, DEFAULTS, wx.getStorageSync(SETTINGS_KEY) || {});
    } catch (e) {
      return Object.assign({}, DEFAULTS);
    }
  }

  saveSettings(partial) {
    const s = Object.assign(this.loadSettings(), partial || {});
    wx.setStorageSync(SETTINGS_KEY, s);
    return s;
  }

  start() {
    if (this._started) return;
    this._started = true;
    this.connect({ silent: true });
  }

  connect(opts) {
    opts = opts || {};
    const s = this.loadSettings();
    if (!s.host) {
      if (!opts.silent) wx.showToast({ title: '请先在"设置"页填写 MQTT 信息', icon: 'none' });
      return;
    }
    if (this.client) {
      try { this.client.end(); } catch (e) { /* ignore */ }
      this.client = null;
    }
    this.connected = false;
    this.connecting = true;
    this.deviceOnline = false;

    const client = new MqttClient({
      host: s.host,
      port: Number(s.port) || 8083,
      tls: !!s.tls,
      path: s.path || '/mqtt',
      username: s.username || '',
      password: s.password || '',
      clientId: 'wx_' + Date.now() + '_' + Math.floor(Math.random() * 1e5),
      keepalive: 30,
      will: {
        topic: s.respTopic,
        message: JSON.stringify({ type: 'status', online: false }),
        qos: 0,
        retain: false
      }
    });

    client.on('connect', () => {
      this.connected = true;
      this.connecting = false;
      client.subscribe(s.respTopic, 0);
      this.cmd({ type: 'status' });
      this.cmd({ type: 'list' });
      eventBus.emit('mqtt', { connected: true });
      wx.showToast({ title: 'MQTT 已连接', icon: 'success' });
    });
    client.on('connectfail', (rc) => {
      this.connected = false;
      this.connecting = false;
      eventBus.emit('mqtt', { connected: false, error: '连接被拒绝 code=' + rc });
      wx.showToast({ title: '连接失败(code ' + rc + ')，请检查配置', icon: 'none' });
    });
    client.on('close', () => {
      this.connected = false;
      this.connecting = false;
      this.deviceOnline = false;
      eventBus.emit('mqtt', { connected: false, error: '连接断开，自动重连中' });
    });
    client.on('error', (e) => {
      eventBus.emit('mqtt', { connected: false, error: (e && e.errMsg) || '连接错误' });
    });
    client.on('message', (m) => this._onMessage(m.topic, m.payload));

    this.client = client;
    client.connect();
  }

  disconnect() {
    this.connected = false;
    this.connecting = false;
    this.deviceOnline = false;
    if (this.client) {
      try { this.client.end(); } catch (e) { /* ignore */ }
      this.client = null;
    }
    eventBus.emit('mqtt', { connected: false });
    wx.showToast({ title: '已断开', icon: 'none' });
  }

  /** 下发指令（统一入口） */
  cmd(obj) {
    if (!this.connected || !this.client) {
      wx.showToast({ title: 'MQTT 未连接', icon: 'none' });
      return false;
    }
    obj.ts = Math.floor(Date.now() / 1000);
    const s = this.loadSettings();
    if (s.key) obj.key = s.key;
    this.client.publish(s.cmdTopic, JSON.stringify(obj), 1); // QoS1 保证送达
    return true;
  }

  /** 消息分发 */
  _onMessage(topic, payload) {
    const s = this.loadSettings();
    if (topic !== s.respTopic) return;

    let msg = null;
    try { msg = JSON.parse(payload); } catch (e) { return; }
    if (!msg || !msg.type) return;

    switch (msg.type) {
      case 'status':
        this.deviceOnline = !!msg.online;
        this.status = msg;
        eventBus.emit('status', msg);
        if (!msg.online) this._log('设备', '设备离线');
        break;
      case 'unlock': {
        const src = msg.source === 'finger' ? '指纹开锁' : '远程开锁';
        const id = msg.fp_id >= 0 ? (' #' + msg.fp_id) : '';
        this._log('开锁', src + id);
        eventBus.emit('unlock', msg);
        break;
      }
      case 'scan':
        eventBus.emit('scan', msg);
        break;
      case 'enroll':
        this._log('录入', msg.phase + (msg.id >= 0 ? (' #' + msg.id) : ''));
        eventBus.emit('enroll', msg);
        if (msg.phase === 'done') this.cmd({ type: 'list' });
        break;
      case 'delete':
        this._log('删除', msg.phase === 'ok' ? ('#' + msg.id + ' 成功') : ('#' + msg.id + ' 失败'));
        eventBus.emit('op', msg);
        if (msg.phase === 'ok') this.cmd({ type: 'list' });
        break;
      case 'rename':
        this._log('备注', msg.phase === 'ok' ? ('#' + msg.id + ' 已修改') : ('#' + msg.id + ' 修改失败'));
        eventBus.emit('op', msg);
        if (msg.phase === 'ok') this.cmd({ type: 'list' });
        break;
      case 'clear':
        this._log('清空', msg.phase === 'ok' ? '全部指纹已清空' : '清空失败');
        eventBus.emit('op', msg);
        if (msg.phase === 'ok') this.cmd({ type: 'list' });
        break;
      case 'list':
        this.fpList = msg.items || [];
        eventBus.emit('fplist', this.fpList);
        break;
      case 'error':
        this._log('错误', msg.phase || msg.code || 'unknown');
        eventBus.emit('op', msg);
        break;
      default:
        break;
    }
  }

  _log(tag, content) {
    util.appendLog(tag, content);
    eventBus.emit('log');
  }
}

module.exports = MqttManager;
module.exports.DEFAULTS = DEFAULTS;
