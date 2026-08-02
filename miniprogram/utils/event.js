/** 轻量事件总线：页面/管理器之间解耦通信 */
const handlers = {};

function on(name, cb) {
  (handlers[name] = handlers[name] || []).push(cb);
  return function off() {
    remove(name, cb);
  };
}

function remove(name, cb) {
  if (!handlers[name]) return;
  const i = handlers[name].indexOf(cb);
  if (i >= 0) handlers[name].splice(i, 1);
}

function emit(name, data) {
  (handlers[name] || []).slice().forEach(function (cb) {
    try { cb(data); } catch (e) { /* 忽略回调异常 */ }
  });
}

module.exports = { on: on, remove: remove, emit: emit };
