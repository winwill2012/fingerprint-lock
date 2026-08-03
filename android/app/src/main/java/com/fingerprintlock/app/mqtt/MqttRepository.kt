package com.fingerprintlock.app.mqtt

import com.fingerprintlock.app.settings.SettingsStore
import com.hivemq.client.mqtt.MqttClient
import com.hivemq.client.mqtt.datatypes.MqttQos
import com.hivemq.client.mqtt.mqtt3.Mqtt3AsyncClient
import com.hivemq.client.mqtt.mqtt3.message.publish.Mqtt3Publish
import org.json.JSONArray
import org.json.JSONObject
import java.nio.charset.StandardCharsets
import java.util.UUID
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicBoolean

data class DeviceStatus(
    val online: Boolean = false,
    val batteryMv: Int = 0,
    val batteryPct: Int = 0,
    val fpCount: Int = -1,
    val ip: String = "",
    val rssi: Int = 0,
    val uptime: Int = 0,
    val ledBreath: Boolean? = null
)

data class FpItem(
    val id: Int,
    val note: String,
    val ts: Long
)

sealed class MqttEvent {
    data class Connection(val connected: Boolean, val message: String = "") : MqttEvent()
    data class Status(val status: DeviceStatus) : MqttEvent()
    data class Unlock(val source: String, val fpId: Int, val confidence: Int) : MqttEvent()
    data class Enroll(val phase: String, val id: Int, val note: String) : MqttEvent()
    data class FingerprintList(val items: List<FpItem>) : MqttEvent()
    data class Op(val type: String, val phase: String, val id: Int, val note: String) : MqttEvent()
    data class Error(val code: String) : MqttEvent()
}

class MqttRepository(private val settingsStore: SettingsStore) {
    private val listeners = CopyOnWriteArrayList<(MqttEvent) -> Unit>()
    private var client: Mqtt3AsyncClient? = null
    private val connectedFlag = AtomicBoolean(false)
    private val listLock = Any()

    @Volatile
    var connected: Boolean = false
        private set

    @Volatile
    var deviceOnline: Boolean = false
        private set

    @Volatile
    var lastStatus: DeviceStatus? = null
        private set

    @Volatile
    var fingerprints: List<FpItem> = emptyList()
        private set

    /** 是否已收到过指纹列表（首页数量以此为准，避免被周期 status 的旧计数覆盖） */
    @Volatile
    var fingerprintListReady: Boolean = false
        private set

    /** 仅在用户主动删除/清空时允许列表变短 */
    private val pendingDeletedIds = mutableSetOf<Int>()
    private var pendingClear = false
    private var lastListSeq = 0L
    /** 重连后首份列表全量采信，之后再防短包跳变 */
    private var authorityListPending = true

    fun addListener(listener: (MqttEvent) -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: (MqttEvent) -> Unit) {
        listeners.remove(listener)
    }

    private fun emit(event: MqttEvent) {
        listeners.forEach { it(event) }
    }

    fun connect() {
        disconnect(silent = true)
        val s = settingsStore.load()
        if (!s.isValid()) {
            emit(MqttEvent.Connection(false, "请完善 MQTT 配置"))
            return
        }

        // 纯 TCP MQTT
        val builder = MqttClient.builder()
            .useMqttVersion3()
            .identifier("android_" + UUID.randomUUID().toString().take(8))
            .serverHost(s.host)
            .serverPort(s.port)

        val c = builder.buildAsync()
        client = c

        var connectBuilder = c.connectWith()
            .cleanSession(true)
            .keepAlive(30)

        if (s.username.isNotBlank()) {
            connectBuilder = connectBuilder.simpleAuth()
                .username(s.username)
                .password(s.password.toByteArray(StandardCharsets.UTF_8))
                .applySimpleAuth()
        }

        connectBuilder.send()
            .whenComplete { _, err ->
                if (err != null) {
                    connected = false
                    connectedFlag.set(false)
                    emit(MqttEvent.Connection(false, err.message ?: "连接失败"))
                    return@whenComplete
                }
                connected = true
                connectedFlag.set(true)
                synchronized(listLock) {
                    lastListSeq = 0L
                    pendingDeletedIds.clear()
                    pendingClear = false
                    authorityListPending = true
                    fingerprintListReady = false
                }
                emit(MqttEvent.Connection(true, "已连接"))
                c.subscribeWith()
                    .topicFilter(s.respTopic)
                    .qos(MqttQos.AT_LEAST_ONCE)
                    .callback { pub -> onPublish(pub, s.respTopic) }
                    .send()
                cmd(JSONObject().put("type", "status"))
                cmd(JSONObject().put("type", "list"))
            }
    }

    fun disconnect(silent: Boolean = false) {
        try {
            client?.disconnect()
        } catch (_: Exception) {
        }
        client = null
        connected = false
        connectedFlag.set(false)
        deviceOnline = false
        if (!silent) emit(MqttEvent.Connection(false, "已断开"))
    }

    fun cmd(obj: JSONObject): Boolean {
        val c = client
        if (!connected || c == null) {
            emit(MqttEvent.Connection(false, "MQTT 未连接"))
            return false
        }
        val s = settingsStore.load()
        obj.put("ts", System.currentTimeMillis() / 1000)
        val type = obj.optString("type")
        when (type) {
            "delete" -> {
                val id = obj.optInt("id", -1)
                if (id >= 0) {
                    synchronized(listLock) { pendingDeletedIds.add(id) }
                }
            }
            "clear" -> synchronized(listLock) { pendingClear = true }
        }
        val qos =
            if (type == "enroll" || type == "cancel") MqttQos.AT_MOST_ONCE else MqttQos.AT_LEAST_ONCE
        c.publishWith()
            .topic(s.cmdTopic)
            .qos(qos)
            .payload(obj.toString().toByteArray(StandardCharsets.UTF_8))
            .send()
        return true
    }

    private fun onPublish(pub: Mqtt3Publish, respTopic: String) {
        if (pub.topic.toString() != respTopic) return
        val text = String(pub.payloadAsBytes, StandardCharsets.UTF_8)
        val msg = try {
            JSONObject(text)
        } catch (_: Exception) {
            return
        }
        when (msg.optString("type")) {
            "status" -> {
                val reportedCount = when {
                    msg.has("fp_reg") -> msg.optInt("fp_reg", -1)
                    else -> msg.optInt("fp_count", -1)
                }
                // 已有列表时，数量与指纹页保持一致，不被周期 status 打回旧值
                val fpCount = if (fingerprintListReady) fingerprints.size else reportedCount
                val st = DeviceStatus(
                    online = msg.optBoolean("online"),
                    batteryMv = msg.optInt("battery_mv"),
                    batteryPct = msg.optInt("battery_pct"),
                    fpCount = fpCount,
                    ip = msg.optString("ip"),
                    rssi = msg.optInt("rssi"),
                    uptime = msg.optInt("uptime"),
                    ledBreath = if (msg.has("led_breath")) msg.optBoolean("led_breath") else null
                )
                deviceOnline = st.online
                lastStatus = st
                emit(MqttEvent.Status(st))
            }
            "unlock" -> emit(
                MqttEvent.Unlock(
                    source = msg.optString("source"),
                    fpId = msg.optInt("fp_id", -1),
                    confidence = msg.optInt("confidence", -1)
                )
            )
            "enroll" -> emit(
                MqttEvent.Enroll(
                    phase = msg.optString("phase"),
                    id = msg.optInt("id", -1),
                    note = msg.optString("note")
                )
            )
            "list" -> applyRemoteList(msg)
            "delete", "rename", "clear" -> {
                val type = msg.optString("type")
                val phase = msg.optString("phase")
                val id = msg.optInt("id", -1)
                val note = msg.optString("note")
                synchronized(listLock) {
                    when (type) {
                        "delete" -> {
                            pendingDeletedIds.remove(id)
                            if (phase == "ok" && id >= 0) {
                                applyLocalListLocked(fingerprints.filter { it.id != id })
                            }
                        }
                        "clear" -> {
                            if (phase == "ok") {
                                pendingClear = false
                                applyLocalListLocked(emptyList())
                            } else {
                                pendingClear = false
                            }
                        }
                        "rename" -> if (phase == "ok" && id >= 0) {
                            applyLocalListLocked(
                                fingerprints.map {
                                    if (it.id == id) it.copy(note = note) else it
                                }
                            )
                        }
                    }
                }
                emit(MqttEvent.Op(type = type, phase = phase, id = id, note = note))
            }
            "led" -> emit(
                MqttEvent.Op(
                    type = msg.optString("type"),
                    phase = msg.optString("phase"),
                    id = msg.optInt("id", -1),
                    note = msg.optString("note")
                )
            )
            "error" -> emit(
                MqttEvent.Error(
                    msg.optString("phase").ifEmpty { msg.optString("code") }
                )
            )
        }
    }

    /**
     * 远程列表策略：
     * - 可增、可更新备注
     * - 禁止无故变短（MQTT 残包/乱序曾导致 3→2 跳变）
     * - 仅 pending 删除/清空时允许变短
     */
    private fun applyRemoteList(msg: JSONObject) {
        val arr = msg.optJSONArray("items") ?: JSONArray()
        val declared = msg.optInt("count", arr.length())
        // count 与 items 不一致 → 视为残包，直接丢弃
        if (declared != arr.length()) return

        val incoming = mutableListOf<FpItem>()
        val seen = HashSet<Int>()
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: return // 残包
            val id = o.optInt("id", -1)
            if (id < 0 || !seen.add(id)) continue
            incoming.add(
                FpItem(
                    id = id,
                    note = o.optString("note"),
                    ts = o.optLong("ts")
                )
            )
        }

        val seq = msg.optLong("seq", 0L)
        synchronized(listLock) {
            if (seq > 0 && seq < lastListSeq) return // 乱序旧包
            if (seq > lastListSeq) lastListSeq = seq

            if (authorityListPending) {
                authorityListPending = false
                pendingDeletedIds.clear()
                pendingClear = false
                applyLocalListLocked(incoming.sortedBy { it.id })
                return
            }

            if (pendingClear) {
                if (incoming.isEmpty()) {
                    pendingClear = false
                    applyLocalListLocked(emptyList())
                }
                // 清空进行中若仍收到非空列表，先不覆盖
                return
            }

            val byId = LinkedHashMap<Int, FpItem>()
            // 保留本地条目，避免短包/残包把列表打成 2 条
            for (item in fingerprints) {
                byId[item.id] = item
            }
            // 远端覆盖/追加；正在删除的 id 不接受远端再加回来
            for (item in incoming) {
                if (item.id in pendingDeletedIds) continue
                byId[item.id] = item
            }
            // 已确认清空前，若远端空列表且非 pendingClear，不要在上面提前 return
            // 主动删除成功后由 Op 分支删本地；此处若远端更短，本地多出的仍保留
            val merged = byId.values.sortedBy { it.id }
            applyLocalListLocked(merged)
        }
    }

    /** 录入成功：立刻插入/更新本地列表 */
    fun optimisticUpsert(item: FpItem) {
        synchronized(listLock) {
            val rest = fingerprints.filter { it.id != item.id }
            applyLocalListLocked((rest + item).sortedBy { it.id })
        }
    }

    private fun applyLocalListLocked(items: List<FpItem>) {
        fingerprints = items
        fingerprintListReady = true
        lastStatus = lastStatus?.copy(fpCount = items.size) ?: DeviceStatus(fpCount = items.size)
        emit(MqttEvent.FingerprintList(items))
    }
}
