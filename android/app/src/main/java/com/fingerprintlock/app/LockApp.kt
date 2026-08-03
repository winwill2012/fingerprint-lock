package com.fingerprintlock.app

import android.app.Application
import com.fingerprintlock.app.data.EventStore
import com.fingerprintlock.app.mqtt.MqttRepository
import com.fingerprintlock.app.settings.SettingsStore

class LockApp : Application() {
    lateinit var settings: SettingsStore
        private set
    lateinit var mqtt: MqttRepository
        private set
    lateinit var events: EventStore
        private set

    override fun onCreate() {
        super.onCreate()
        instance = this
        settings = SettingsStore(this)
        events = EventStore(this)
        mqtt = MqttRepository(settings)
        val s = settings.load()
        if (s.isValid()) {
            mqtt.connect()
        }
    }

    companion object {
        lateinit var instance: LockApp
            private set
    }
}
