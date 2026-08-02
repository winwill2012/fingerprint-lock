const MqttManager = require('./utils/mqtt-manager');

App({
  onLaunch() {
    // 全局 MQTT 管理器：自动读取本地配置并连接
    this.manager = new MqttManager();
    this.manager.start();
  },
  globalData: {}
});
