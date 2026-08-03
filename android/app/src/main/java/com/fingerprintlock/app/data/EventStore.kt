package com.fingerprintlock.app.data

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

data class EventLog(
    val id: Long,
    val title: String,
    val detail: String,
    val ts: Long
)

/** 本地最近事件，最多保留 20 条（SharedPreferences JSON） */
class EventStore(context: Context) {
    private val prefs = context.getSharedPreferences("event_log", Context.MODE_PRIVATE)

    fun load(): List<EventLog> {
        val raw = prefs.getString("items", "[]") ?: "[]"
        return try {
            val arr = JSONArray(raw)
            buildList {
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    add(
                        EventLog(
                            id = o.optLong("id"),
                            title = o.optString("title"),
                            detail = o.optString("detail"),
                            ts = o.optLong("ts")
                        )
                    )
                }
            }
        } catch (_: Exception) {
            emptyList()
        }
    }

    fun add(title: String, detail: String = ""): List<EventLog> {
        val list = load().toMutableList()
        list.add(
            0,
            EventLog(
                id = System.currentTimeMillis(),
                title = title,
                detail = detail,
                ts = System.currentTimeMillis()
            )
        )
        while (list.size > MAX) list.removeAt(list.lastIndex)
        save(list)
        return list
    }

    private fun save(list: List<EventLog>) {
        val arr = JSONArray()
        list.forEach { e ->
            arr.put(
                JSONObject()
                    .put("id", e.id)
                    .put("title", e.title)
                    .put("detail", e.detail)
                    .put("ts", e.ts)
            )
        }
        prefs.edit().putString("items", arr.toString()).apply()
    }

    companion object {
        const val MAX = 20
    }
}
