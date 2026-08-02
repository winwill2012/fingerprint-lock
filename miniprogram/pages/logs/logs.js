const util = require('../../utils/util');
const eventBus = require('../../utils/event');

Page({
  data: { logs: [] },

  onShow() {
    const that = this;
    this._unsubs = [eventBus.on('log', () => this.refresh())];
    this.refresh();
  },

  onHide() {
    (this._unsubs || []).forEach((fn) => fn());
    this._unsubs = [];
  },

  refresh() {
    this.setData({ logs: util.getLogs() });
  },

  onClear() {
    wx.showModal({
      title: '清空日志',
      content: '确定清空本地日志记录吗？',
      success(res) {
        if (res.confirm) {
          util.clearLogs();
          this.refresh();
        }
      }
    });
  }
});
