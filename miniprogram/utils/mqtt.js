/**
 * ============================================================
 * 轻量 MQTT 3.1.1 客户端（基于微信小程序 WebSocket）
 * ------------------------------------------------------------
 * 无需 npm 依赖，直接在小程序中可用。
 * 支持：CONNECT(clean/用户名密码/遗嘱)、SUBSCRIBE(QoS0/1)、
 *      PUBLISH(QoS0/1)、PINGREQ 心跳、断线自动重连（指数退避）。
 * 说明：本客户端按"订阅主题即为精确主题"设计，不做通配符匹配；
 *      如需通配符可自行扩展。
 * ============================================================
 */

// ---------- 基础编解码 ----------
function utf8ToBytes(str) {
  const enc = encodeURIComponent(String(str == null ? '' : str));
  const arr = [];
  for (let i = 0; i < enc.length; i++) {
    if (enc[i] === '%') {
      arr.push(parseInt(enc.substr(i + 1, 2), 16));
      i += 2;
    } else {
      arr.push(enc.charCodeAt(i));
    }
  }
  return new Uint8Array(arr);
}

function bytesToUtf8(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i++) {
    s += '%' + ('0' + bytes[i].toString(16)).slice(-2);
  }
  try {
    return decodeURIComponent(s);
  } catch (e) {
    return '';
  }
}

function u16(v) {
  return new Uint8Array([(v >> 8) & 0xff, v & 0xff]);
}

function concat(parts) {
  let len = 0;
  parts.forEach(function (p) { len += p.length; });
  const out = new Uint8Array(len);
  let off = 0;
  parts.forEach(function (p) {
    out.set(p, off);
    off += p.length;
  });
  return out;
}

function encodeStr(str) {
  const u = utf8ToBytes(str);
  return concat([u16(u.length), u]);
}

function encodeRemLen(n) {
  const b = [];
  do {
    let x = n % 128;
    n = Math.floor(n / 128);
    if (n > 0) x |= 0x80;
    b.push(x);
  } while (n > 0);
  return new Uint8Array(b);
}

function buildPacket(header, payload) {
  return concat([new Uint8Array([header]), encodeRemLen(payload.length), payload]);
}

// ---------- 客户端 ----------
class MqttClient {
  constructor(opts) {
    opts = opts || {};
    this.host = opts.host;
    this.port = opts.port || 8083;
    this.tls = !!opts.tls;
    this.path = opts.path || '/mqtt';
    this.username = opts.username || '';
    this.password = opts.password || '';
    this.clientId = opts.clientId || ('wx_' + Date.now() + '_' + Math.floor(Math.random() * 1e5));
    this.keepalive = opts.keepalive || 30;
    this.will = opts.will || null;

    this._listeners = {};
    this._seq = 0;
    this._state = 'idle';          // idle | connecting | open | closed
    this._manualClose = false;
    this._reconnects = 0;
    this._lastSent = 0;
    this._lastRecv = Date.now();
    this._pingTimer = null;
    this._reconnectTimer = null;
    this._rxBuf = new Uint8Array(0);
    this._parseReset();

    // 微信 WebSocket 全局回调（只需注册一次）
    wx.onSocketOpen(() => {
      if (this._state === 'connecting') {
        this._state = 'open';
        this._sendPacket(this._buildConnect());
        this._lastSent = Date.now();
        this._startKeepalive();
      }
    });
    wx.onSocketMessage((res) => {
      this._lastRecv = Date.now();
      let bytes;
      if (typeof res.data === 'string') {
        bytes = utf8ToBytes(res.data);
      } else {
        bytes = new Uint8Array(res.data);
      }
      this._onData(bytes);
    });
    wx.onSocketError((e) => {
      if (this._state !== 'closed') this._emit('error', e);
      // 连接阶段失败也走重连
      if (this._state === 'connecting' && !this._manualClose) {
        this._state = 'closed';
        this._scheduleReconnect();
      }
    });
    wx.onSocketClose(() => {
      this._stopKeepalive();
      if (!this._manualClose) {
        this._state = 'closed';
        this._emit('close');
        this._scheduleReconnect();
      } else {
        this._state = 'closed';
      }
    });
  }

  // ---------- 生命周期 ----------
  connect() {
    if (this._state === 'connecting' || this._state === 'open') return;
    this._manualClose = false;
    this._state = 'connecting';
    this._reconnects = 0;
    const scheme = this.tls ? 'wss' : 'ws';
    const url = scheme + '://' + this.host + ':' + this.port + this.path;
    wx.connectSocket({
      url: url,
      // MQTT over WebSocket 必须声明子协议 "mqtt"，否则 EMQX 等 Broker 会返回 400
      header: { 'Sec-WebSocket-Protocol': 'mqtt' },
      timeout: 10000,
      fail: (e) => {
        this._emit('error', e);
        if (!this._manualClose) this._scheduleReconnect();
      }
    });
  }

  end() {
    this._manualClose = true;
    this._stopKeepalive();
    this._stopReconnect();
    if (this._state === 'open') {
      try { this._sendPacket(new Uint8Array([0xe0, 0x00])); } catch (e) { /* ignore */ }
    }
    try { wx.closeSocket({ code: 1000, reason: 'mqtt end' }); } catch (e) { /* ignore */ }
    this._state = 'closed';
  }

  _closeSocket() {
    try { wx.closeSocket({ code: 1000, reason: 'mqtt close' }); } catch (e) { /* ignore */ }
  }

  _scheduleReconnect() {
    if (this._manualClose) return;
    const delay = Math.min(30000, 1000 * Math.pow(2, this._reconnects++));
    if (this._reconnectTimer) clearTimeout(this._reconnectTimer);
    this._reconnectTimer = setTimeout(() => {
      this._reconnectTimer = null;
      if (!this._manualClose) this.connect();
    }, delay);
  }

  _stopReconnect() {
    if (this._reconnectTimer) {
      clearTimeout(this._reconnectTimer);
      this._reconnectTimer = null;
    }
  }

  _startKeepalive() {
    this._stopKeepalive();
    const interval = Math.max(1000, this.keepalive * 500);
    this._pingTimer = setInterval(() => {
      const now = Date.now();
      if (now - this._lastRecv > this.keepalive * 1500) {
        // 超时未收到任何报文，判定连接已死
        this._closeSocket();
        return;
      }
      if (now - this._lastSent >= this.keepalive * 1000) {
        this._sendPacket(new Uint8Array([0xc0, 0x00])); // PINGREQ
        this._lastSent = now;
      }
    }, interval);
  }

  _stopKeepalive() {
    if (this._pingTimer) {
      clearInterval(this._pingTimer);
      this._pingTimer = null;
    }
  }

  // ---------- 对外 API ----------
  on(name, cb) {
    (this._listeners[name] = this._listeners[name] || []).push(cb);
    return this;
  }

  _emit(name, data) {
    (this._listeners[name] || []).slice().forEach((cb) => {
      try { cb(data); } catch (e) { /* ignore */ }
    });
  }

  get connected() {
    return this._state === 'open';
  }

  subscribe(topic, qos) {
    const pid = this._nextPid();
    const payload = concat([u16(pid), encodeStr(topic), new Uint8Array([qos || 0])]);
    return this._sendPacket(buildPacket(0x82, payload));
  }

  publish(topic, message, qos) {
    qos = qos || 0;
    if (qos === 1) {
      return this._sendPacket(this._buildPublish(topic, message, 1, this._nextPid()));
    }
    return this._sendPacket(this._buildPublish(topic, message, 0, 0));
  }

  // ---------- 报文构造 ----------
  _nextPid() {
    this._seq = (this._seq % 0xffff) + 1;
    return this._seq;
  }

  _buildConnect() {
    let vh = concat([encodeStr('MQTT'), new Uint8Array([0x04])]);
    let flags = 0x02; // clean session
    const u = this.username;
    const p = this.password;
    const w = this.will;
    if (u) flags |= 0x80;
    if (p) flags |= 0x40;
    if (w && w.topic) {
      flags |= 0x04;
      flags |= (w.qos || 0) << 3;
      if (w.retain) flags |= 0x20;
    }
    const ka = this.keepalive;
    vh = concat([vh, new Uint8Array([flags, (ka >> 8) & 0xff, ka & 0xff])]);

    let payload = encodeStr(this.clientId);
    if (w && w.topic) {
      payload = concat([payload, encodeStr(w.topic), utf8ToBytes(String(w.message == null ? '' : w.message))]);
    }
    if (u) payload = concat([payload, encodeStr(u)]);
    if (p) payload = concat([payload, encodeStr(p)]);
    return buildPacket(0x10, concat([vh, payload]));
  }

  _buildPublish(topic, message, qos, pid) {
    let body = encodeStr(topic);
    if (qos === 1) body = concat([body, u16(pid)]);
    body = concat([body, utf8ToBytes(String(message == null ? '' : message))]);
    return buildPacket(qos === 1 ? 0x32 : 0x30, body);
  }

  _sendPacket(packet) {
    if (this._state !== 'open') return false;
    try {
      const buf = (packet instanceof Uint8Array) ? packet.buffer : packet;
      wx.sendSocketMessage({ data: buf });
      this._lastSent = Date.now();
      return true;
    } catch (e) {
      return false;
    }
  }

  // ---------- 接收与解析 ----------
  _parseReset() {
    this._pktHeader = 0;
    this._remLen = 0;
    this._remMult = 1;
    this._needHeader = true;
    this._needLen = false;
    this._pktPayload = null;
    this._pktPos = 0;
  }

  _onData(bytes) {
    if (bytes.length === 0) return;
    const merged = new Uint8Array(this._rxBuf.length + bytes.length);
    merged.set(this._rxBuf, 0);
    merged.set(bytes, this._rxBuf.length);
    this._rxBuf = merged;
    this._parse();
  }

  _parse() {
    let i = 0;
    const buf = this._rxBuf;
    for (;;) {
      if (this._needHeader) {
        if (buf.length - i < 1) break;
        this._pktHeader = buf[i++];
        this._remLen = 0;
        this._remMult = 1;
        this._needHeader = false;
        this._needLen = true;
      }
      if (this._needLen) {
        while (i < buf.length && this._needLen) {
          const b = buf[i++];
          this._remLen += (b & 0x7f) * this._remMult;
          if (b & 0x80) {
            this._remMult *= 128;
          } else {
            this._needLen = false;
          }
        }
        if (this._needLen) break; // 剩余长度字节不完整
        this._pktPayload = new Uint8Array(this._remLen);
        this._pktPos = 0;
      }
      if (this._pktPos < this._remLen) {
        const take = Math.min(this._remLen - this._pktPos, buf.length - i);
        this._pktPayload.set(buf.subarray(i, i + take), this._pktPos);
        this._pktPos += take;
        i += take;
        if (this._pktPos < this._remLen) break; // payload 不完整
      }
      // 一个完整报文
      this._handlePacket(this._pktHeader, this._pktPayload);
      this._parseReset();
      if (i >= buf.length) break;
    }
    this._rxBuf = (i >= buf.length) ? new Uint8Array(0) : buf.slice(i);
  }

  _handlePacket(header, payload) {
    const type = header >> 4;
    switch (type) {
      case 0x2: { // CONNACK
        const rc = payload.length >= 2 ? payload[1] : 0xff;
        if (rc === 0) {
          this._reconnects = 0;
          this._emit('connect');
        } else {
          this._emit('connectfail', rc);
        }
        break;
      }
      case 0x3: { // PUBLISH
        const qos = (header >> 1) & 3;
        const tlen = (payload[0] << 8) | payload[1];
        let off = 2;
        const topic = bytesToUtf8(payload.subarray(off, off + tlen));
        off += tlen;
        let pid = 0;
        if (qos > 0) {
          pid = (payload[off] << 8) | payload[off + 1];
          off += 2;
        }
        const msg = bytesToUtf8(payload.subarray(off));
        if (qos === 1) {
          this._sendPacket(new Uint8Array([0x40, 0x02, (pid >> 8) & 0xff, pid & 0xff])); // PUBACK
        }
        this._emit('message', { topic: topic, payload: msg, qos: qos, pid: pid });
        break;
      }
      case 0x4: // PUBACK
      case 0x5: // PUBREC
      case 0x6: // PUBREL
      case 0x7: // PUBCOMP
        break;
      case 0x9: // SUBACK
        this._emit('suback', payload);
        break;
      case 0xd: // PINGRESP
        break;
      default:
        break;
    }
  }
}

module.exports = MqttClient;
