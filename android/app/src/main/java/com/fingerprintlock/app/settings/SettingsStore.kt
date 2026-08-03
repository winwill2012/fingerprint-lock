package com.fingerprintlock.app.settings

import android.content.Context
import android.content.SharedPreferences

data class MqttSettings(
    val host: String = "iot.welinklab.com",
    val port: Int = 1883,
    val username: String = "wl_wdjbr6wp2tzaq",
    val password: String = "VVwFcmYIB8kaE1GX_1mb2qHH4ZLQsE_P",
    val cmdTopic: String = "welink/wl_wdjbr6wp2tzaq/fingerprint-lock-cmd",
    val respTopic: String = "welink/wl_wdjbr6wp2tzaq/fingerprint-lock-upload",
    val ledBreath: Boolean = true
) {
    fun isValid(): Boolean =
        host.isNotBlank() && port in 1..65535 &&
            cmdTopic.isNotBlank() && respTopic.isNotBlank()
}

class SettingsStore(context: Context) {
    private val prefs: SharedPreferences =
        context.getSharedPreferences("mqtt_settings", Context.MODE_PRIVATE)

    init {
        if (!prefs.getBoolean("use_tcp_mqtt", false)) {
            prefs.edit()
                .putInt("port", 1883)
                .putBoolean("tls", false)
                .remove("path")
                .putBoolean("use_tcp_mqtt", true)
                .apply()
        }
    }

    fun load(): MqttSettings = MqttSettings(
        host = prefs.getString("host", MqttSettings().host) ?: MqttSettings().host,
        port = prefs.getInt("port", MqttSettings().port),
        username = prefs.getString("username", MqttSettings().username) ?: "",
        password = prefs.getString("password", MqttSettings().password) ?: "",
        cmdTopic = prefs.getString("cmdTopic", MqttSettings().cmdTopic) ?: "",
        respTopic = prefs.getString("respTopic", MqttSettings().respTopic) ?: "",
        ledBreath = prefs.getBoolean("ledBreath", true)
    )

    fun save(s: MqttSettings) {
        prefs.edit()
            .putString("host", s.host)
            .putInt("port", s.port)
            .putBoolean("tls", false)
            .putString("username", s.username)
            .putString("password", s.password)
            .putString("cmdTopic", s.cmdTopic)
            .putString("respTopic", s.respTopic)
            .putBoolean("ledBreath", s.ledBreath)
            .putBoolean("use_tcp_mqtt", true)
            .apply()
    }

    fun saveLedBreath(on: Boolean) {
        prefs.edit().putBoolean("ledBreath", on).apply()
    }
}
