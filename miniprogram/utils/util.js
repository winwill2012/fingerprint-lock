/** 通用工具 */

function pad(n) {
  return n < 10 ? '0' + n : '' + n;
}

/** 时间戳(秒) -> 本地时间字符串 */
function formatTime(ts) {
  if (!ts) return '';
  const d = new Date(ts * 1000);
  return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) +
    ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
}

/** 当前时间字符串（本地日志用） */
function nowText() {
  const d = new Date();
  return pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + ' ' +
    pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
}

const LOG_KEY = 'fp_logs';

function appendLog(tag, content) {
  try {
    const arr = wx.getStorageSync(LOG_KEY) || [];
    arr.unshift({ time: nowText(), tag: tag, content: content || '' });
    if (arr.length > 100) arr.length = 100;
    wx.setStorageSync(LOG_KEY, arr);
  } catch (e) { /* ignore */ }
}

function getLogs() {
  try { return wx.getStorageSync(LOG_KEY) || []; } catch (e) { return []; }
}

function clearLogs() {
  try { wx.removeStorageSync(LOG_KEY); } catch (e) { /* ignore */ }
}

module.exports = {
  formatTime: formatTime,
  nowText: nowText,
  appendLog: appendLog,
  getLogs: getLogs,
  clearLogs: clearLogs
};
