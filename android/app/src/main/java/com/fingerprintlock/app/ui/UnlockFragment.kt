package com.fingerprintlock.app.ui

import android.animation.AnimatorSet
import android.animation.ObjectAnimator
import android.animation.ValueAnimator
import android.os.Bundle
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.animation.AccelerateDecelerateInterpolator
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.fingerprintlock.app.LockApp
import com.fingerprintlock.app.R
import com.fingerprintlock.app.data.EventLog
import com.fingerprintlock.app.databinding.FragmentUnlockBinding
import com.fingerprintlock.app.databinding.ItemEventBinding
import com.fingerprintlock.app.mqtt.MqttEvent
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class UnlockFragment : Fragment() {
    private var _binding: FragmentUnlockBinding? = null
    private val binding get() = _binding!!
    private val mqtt get() = LockApp.instance.mqtt
    private val events get() = LockApp.instance.events
    private val adapter = EventAdapter()
    private var breathAnimator: ObjectAnimator? = null

    private val listener: (MqttEvent) -> Unit = { event ->
        activity?.runOnUiThread { handle(event) }
    }

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?): View {
        _binding = FragmentUnlockBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        binding.rvEvents.layoutManager = LinearLayoutManager(requireContext())
        binding.rvEvents.adapter = adapter
        setupUnlockButton()
        binding.btnRefresh.setOnClickListener {
            mqtt.cmd(JSONObject().put("type", "status"))
        }
        renderConnection()
        renderEvents(events.load())
        mqtt.lastStatus?.let { applyStatus(it) }
    }

    private fun setupUnlockButton() {
        binding.btnUnlock.setOnClickListener {
            if (!mqtt.connected || !mqtt.deviceOnline) {
                toast("设备不在线，请稍后重试")
                return@setOnClickListener
            }
            AppDialogs.confirm(
                context = requireContext(),
                title = "确认远程开锁？",
                message = "将向门锁下发开锁指令",
                confirmText = "开锁",
                iconRes = R.drawable.ic_nav_home_on
            ) {
                mqtt.cmd(JSONObject().put("type", "unlock"))
                appendEvent("远程开锁指令已发送")
                playUnlockPulse()
                toast("指令已发送")
            }
        }
        binding.btnUnlock.setOnTouchListener { v, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    v.animate().scaleX(0.94f).scaleY(0.94f).setDuration(90).start()
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    v.animate().scaleX(1f).scaleY(1f).setDuration(140).start()
                }
            }
            false
        }
    }

    override fun onStart() {
        super.onStart()
        mqtt.addListener(listener)
        renderConnection()
        renderEvents(events.load())
    }

    override fun onStop() {
        mqtt.removeListener(listener)
        stopBreath()
        super.onStop()
    }

    private fun handle(event: MqttEvent) {
        when (event) {
            is MqttEvent.Connection -> renderConnection()
            is MqttEvent.Status -> applyStatus(event.status)
            is MqttEvent.FingerprintList -> renderDeviceLine()
            is MqttEvent.Unlock -> {
                val src = if (event.source == "finger") "指纹开锁" else "远程开锁"
                val id = if (event.fpId >= 0) "#${event.fpId} " else ""
                appendEvent("$id$src")
                playUnlockPulse()
            }
            else -> Unit
        }
    }

    private fun appendEvent(title: String) {
        renderEvents(events.add(title))
    }

    private fun renderEvents(list: List<EventLog>) {
        adapter.submit(list)
        binding.tvLastEvent.visibility = if (list.isEmpty()) View.VISIBLE else View.GONE
        binding.rvEvents.visibility = if (list.isEmpty()) View.GONE else View.VISIBLE
    }

    /** 指纹数量与指纹页列表保持一致 */
    private fun fingerprintCountLabel(): String? {
        val n = when {
            mqtt.fingerprintListReady -> mqtt.fingerprints.size
            else -> mqtt.lastStatus?.fpCount?.takeIf { it >= 0 }
        } ?: return null
        return "$n 枚指纹"
    }

    private fun renderDeviceLine(st: com.fingerprintlock.app.mqtt.DeviceStatus? = mqtt.lastStatus) {
        if (_binding == null) return
        val online = mqtt.connected && mqtt.deviceOnline
        binding.tvDeviceState.text = when {
            !mqtt.connected -> "请到设置页检查网络配置"
            online -> buildString {
                if (st?.ip?.isNotBlank() == true) append(st.ip)
                fingerprintCountLabel()?.let {
                    if (isNotEmpty()) append("  ·  ")
                    append(it)
                }
                if (isEmpty()) append("运行正常")
            }
            else -> "等待设备上线…"
        }
    }

    private fun renderConnection() {
        val online = mqtt.connected && mqtt.deviceOnline
        binding.tvMqttState.text = if (online) "设备在线" else "设备离线"
        setDot(online)
        renderDeviceLine()
        renderUnlockVisual(online)
    }

    private fun setDot(ok: Boolean) {
        val res = if (ok) R.drawable.bg_status_dot_ok else R.drawable.bg_status_dot_err
        binding.tvMqttDot.background = ContextCompat.getDrawable(requireContext(), res)
    }

    private fun applyStatus(st: com.fingerprintlock.app.mqtt.DeviceStatus) {
        val online = mqtt.connected && st.online
        binding.tvMqttState.text = if (online) "设备在线" else "设备离线"
        setDot(online)
        renderDeviceLine(st)
        renderBattery(st.batteryPct, st.batteryMv)
        renderUnlockVisual(online)
    }

    private fun renderUnlockVisual(online: Boolean) {
        if (_binding == null) return
        binding.btnUnlock.background = ContextCompat.getDrawable(
            requireContext(),
            if (online) R.drawable.bg_unlock_btn else R.drawable.bg_unlock_btn_offline
        )
        binding.btnUnlock.alpha = if (online) 1f else 0.88f
        binding.unlockHalo.alpha = if (online) 0.55f else 0.18f
        binding.unlockRing.alpha = if (online) 1f else 0.55f
        binding.tvUnlockHint.text = getString(
            if (online) R.string.unlock_hint_online else R.string.unlock_hint_offline
        )
        if (online) startBreath() else stopBreath()
    }

    private fun startBreath() {
        if (breathAnimator?.isRunning == true) return
        val halo = binding.unlockHalo
        breathAnimator = ObjectAnimator.ofFloat(halo, View.SCALE_X, 0.94f, 1.02f).apply {
            duration = 1600
            repeatCount = ValueAnimator.INFINITE
            repeatMode = ValueAnimator.REVERSE
            interpolator = AccelerateDecelerateInterpolator()
            addUpdateListener {
                halo.scaleY = halo.scaleX
            }
            start()
        }
    }

    private fun stopBreath() {
        breathAnimator?.cancel()
        breathAnimator = null
        _binding?.unlockHalo?.apply {
            scaleX = 1f
            scaleY = 1f
        }
    }

    private fun playUnlockPulse() {
        if (_binding == null) return
        val ring = binding.unlockRing
        val set = AnimatorSet()
        val sx = ObjectAnimator.ofFloat(ring, View.SCALE_X, 1f, 1.08f, 1f)
        val sy = ObjectAnimator.ofFloat(ring, View.SCALE_Y, 1f, 1.08f, 1f)
        set.playTogether(sx, sy)
        set.duration = 420
        set.interpolator = AccelerateDecelerateInterpolator()
        set.start()
    }

    private fun renderBattery(pct: Int, mv: Int) {
        if (mv <= 0 && pct <= 0) {
            binding.rowBattery.visibility = View.GONE
            return
        }
        val level = pct.coerceIn(0, 100)
        binding.rowBattery.visibility = View.VISIBLE
        binding.tvBattery.text = "$level%"
        val (icon, color) = when {
            level >= 80 -> R.drawable.ic_battery_full to R.color.primary
            level >= 50 -> R.drawable.ic_battery_high to R.color.primary
            level >= 20 -> R.drawable.ic_battery_mid to R.color.warning
            level > 0 -> R.drawable.ic_battery_low to R.color.danger
            else -> R.drawable.ic_battery_empty to R.color.tab_unselected
        }
        binding.ivBattery.setImageResource(icon)
        val c = ContextCompat.getColor(requireContext(), color)
        binding.ivBattery.clearColorFilter()
        binding.ivBattery.setColorFilter(c)
        binding.tvBattery.setTextColor(c)
    }

    private fun toast(msg: String) {
        android.widget.Toast.makeText(requireContext(), msg, android.widget.Toast.LENGTH_SHORT).show()
    }

    override fun onDestroyView() {
        stopBreath()
        _binding = null
        super.onDestroyView()
    }

    private class EventAdapter : RecyclerView.Adapter<EventAdapter.VH>() {
        private var data: List<EventLog> = emptyList()

        fun submit(list: List<EventLog>) {
            data = list
            notifyDataSetChanged()
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
            val b = ItemEventBinding.inflate(LayoutInflater.from(parent.context), parent, false)
            return VH(b)
        }

        override fun getItemCount() = data.size

        override fun onBindViewHolder(holder: VH, position: Int) {
            val item = data[position]
            holder.b.tvEventTitle.text = item.title
            holder.b.tvEventTime.text =
                SimpleDateFormat("MM-dd HH:mm:ss", Locale.getDefault()).format(Date(item.ts))
        }

        class VH(val b: ItemEventBinding) : RecyclerView.ViewHolder(b.root)
    }
}
