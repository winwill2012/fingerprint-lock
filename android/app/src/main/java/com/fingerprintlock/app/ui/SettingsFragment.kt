package com.fingerprintlock.app.ui

import android.content.Intent
import android.os.Bundle
import android.os.SystemClock
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment
import com.fingerprintlock.app.LockApp
import com.fingerprintlock.app.R
import com.fingerprintlock.app.databinding.FragmentSettingsBinding
import com.fingerprintlock.app.mqtt.MqttEvent
import com.fingerprintlock.app.settings.MqttSettings
import org.json.JSONObject

class SettingsFragment : Fragment() {
    private var _binding: FragmentSettingsBinding? = null
    private val binding get() = _binding!!
    private val settings get() = LockApp.instance.settings
    private val mqtt get() = LockApp.instance.mqtt
    private var ledUpdating = false
    /** 用户刚拨动开关后，短时间内忽略 status 回写，避免旧状态把开关打回去 */
    private var ledUserChangedAtMs = 0L

    private val listener: (MqttEvent) -> Unit = { event ->
        activity?.runOnUiThread {
            when (event) {
                is MqttEvent.Connection -> renderConnChip(event.connected, event.message)
                is MqttEvent.Status -> {
                    if (SystemClock.elapsedRealtime() - ledUserChangedAtMs < 3000L) return@runOnUiThread
                    event.status.ledBreath?.let { syncLedSwitch(it) }
                }
                is MqttEvent.Op -> {
                    if (event.type == "led") {
                        ledUserChangedAtMs = 0L
                        syncLedSwitch(event.phase != "off")
                        if (event.note.startsWith("fail")) {
                            Toast.makeText(requireContext(), "灯控失败：${event.note}", Toast.LENGTH_LONG).show()
                        }
                    }
                }
                else -> Unit
            }
        }
    }

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?): View {
        _binding = FragmentSettingsBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        val s = settings.load()
        bindSettings(s)
        binding.cardBle.setOnClickListener {
            startActivity(Intent(requireContext(), BleProvisionActivity::class.java))
        }
        binding.btnSave.setOnClickListener {
            val cfg = readSettings()
            if (!cfg.isValid()) {
                Toast.makeText(requireContext(), "请填写完整的 MQTT 地址、端口与 Topic", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            settings.save(cfg)
            renderConnChip(false, "正在连接…")
            mqtt.connect()
            Toast.makeText(requireContext(), "已保存并连接", Toast.LENGTH_SHORT).show()
        }
        binding.swLedBreath.setOnCheckedChangeListener { _, checked ->
            if (ledUpdating) return@setOnCheckedChangeListener
            ledUserChangedAtMs = SystemClock.elapsedRealtime()
            settings.saveLedBreath(checked)
            if (!mqtt.connected) {
                Toast.makeText(requireContext(), "MQTT 未连接，稍后会随状态同步", Toast.LENGTH_SHORT).show()
                return@setOnCheckedChangeListener
            }
            mqtt.cmd(JSONObject().put("type", "led").put("enable", checked))
            Toast.makeText(
                requireContext(),
                if (checked) "已开启呼吸灯" else "已关闭呼吸灯",
                Toast.LENGTH_SHORT
            ).show()
        }
        renderConnChip(mqtt.connected, if (mqtt.connected) "已连接" else "未连接")
        mqtt.lastStatus?.ledBreath?.let { syncLedSwitch(it) }
    }

    override fun onStart() {
        super.onStart()
        mqtt.addListener(listener)
        val s = settings.load()
        if (s.isValid() && !mqtt.connected) {
            mqtt.connect()
        }
    }

    override fun onStop() {
        mqtt.removeListener(listener)
        super.onStop()
    }

    private fun renderConnChip(connected: Boolean, message: String) {
        if (connected) {
            binding.tvConnStatus.text = "已连接"
            binding.tvConnStatus.setBackgroundResource(R.drawable.bg_chip_ok)
            binding.tvConnStatus.setTextColor(ContextCompat.getColor(requireContext(), R.color.primary_dark))
        } else {
            val connecting = message.contains("正在")
            binding.tvConnStatus.text = when {
                connecting -> "连接中"
                message.isBlank() || message == "未连接" || message == "已断开" -> "未连接"
                else -> "失败"
            }
            binding.tvConnStatus.setBackgroundResource(
                if (connecting) R.drawable.bg_chip_warn else R.drawable.bg_chip_err
            )
            binding.tvConnStatus.setTextColor(
                ContextCompat.getColor(
                    requireContext(),
                    if (connecting) R.color.warning else R.color.danger
                )
            )
        }
    }

    private fun syncLedSwitch(on: Boolean) {
        ledUpdating = true
        binding.swLedBreath.isChecked = on
        settings.saveLedBreath(on)
        ledUpdating = false
    }

    private fun bindSettings(s: MqttSettings) {
        binding.etHost.setText(s.host)
        binding.etPort.setText(s.port.toString())
        binding.etUsername.setText(s.username)
        binding.etPassword.setText(s.password)
        binding.etCmdTopic.setText(s.cmdTopic)
        binding.etRespTopic.setText(s.respTopic)
        ledUpdating = true
        binding.swLedBreath.isChecked = s.ledBreath
        ledUpdating = false
    }

    private fun readSettings(): MqttSettings {
        val port = binding.etPort.text?.toString()?.toIntOrNull() ?: 1883
        return MqttSettings(
            host = binding.etHost.text?.toString()?.trim().orEmpty(),
            port = port,
            username = binding.etUsername.text?.toString()?.trim().orEmpty(),
            password = binding.etPassword.text?.toString().orEmpty(),
            cmdTopic = binding.etCmdTopic.text?.toString()?.trim().orEmpty(),
            respTopic = binding.etRespTopic.text?.toString()?.trim().orEmpty(),
            ledBreath = binding.swLedBreath.isChecked
        )
    }

    override fun onDestroyView() {
        _binding = null
        super.onDestroyView()
    }
}
