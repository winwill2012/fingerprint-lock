package com.fingerprintlock.app.ui

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.fingerprintlock.app.LockApp
import com.fingerprintlock.app.R
import com.fingerprintlock.app.databinding.FragmentFingerprintBinding
import com.fingerprintlock.app.databinding.ItemFingerprintBinding
import com.fingerprintlock.app.mqtt.FpItem
import com.fingerprintlock.app.mqtt.MqttEvent
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class FingerprintFragment : Fragment() {
    private var _binding: FragmentFingerprintBinding? = null
    private val binding get() = _binding!!
    private val mqtt get() = LockApp.instance.mqtt
    private val adapter = FpAdapter(
        onRename = { item -> rename(item) },
        onDelete = { item -> delete(item) }
    )
    private var enrolling = false

    private val phases = mapOf(
        "wait_finger1" to "请将手指放在指纹模块上…",
        "finger1_ok" to "第 1 次采集成功",
        "lift_finger" to "请移开手指",
        "wait_finger2" to "请再次按下同一根手指…",
        "done" to "录入成功",
        "canceled" to "已取消",
        "failed" to "录入失败"
    )

    private val listener: (MqttEvent) -> Unit = { event ->
        activity?.runOnUiThread { handle(event) }
    }

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?): View {
        _binding = FragmentFingerprintBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        binding.rvFingerprints.layoutManager = LinearLayoutManager(requireContext())
        binding.rvFingerprints.adapter = adapter
        binding.btnRefreshFp.setOnClickListener { mqtt.cmd(JSONObject().put("type", "list")) }
        binding.btnEnroll.setOnClickListener { startEnroll() }
        binding.btnEmptyEnroll.setOnClickListener { startEnroll() }
        binding.btnCancelEnroll.setOnClickListener {
            setEnrolling(false)
            mqtt.cmd(JSONObject().put("type", "cancel"))
            toast("已取消")
        }
        binding.btnClearAll.setOnClickListener {
            AppDialogs.confirm(
                context = requireContext(),
                title = "清空所有指纹？",
                message = "将删除模块中全部指纹，此操作不可恢复",
                confirmText = "清空",
                iconRes = R.drawable.ic_nav_fp_on,
                danger = true
            ) {
                mqtt.cmd(JSONObject().put("type", "clear"))
            }
        }
        renderList(mqtt.fingerprints)
    }

    override fun onStart() {
        super.onStart()
        mqtt.addListener(listener)
        mqtt.cmd(JSONObject().put("type", "list"))
    }

    override fun onStop() {
        mqtt.removeListener(listener)
        super.onStop()
    }

    private fun handle(event: MqttEvent) {
        when (event) {
            is MqttEvent.FingerprintList -> renderList(event.items)
            is MqttEvent.Enroll -> {
                when (event.phase) {
                    "done", "failed", "canceled" -> {
                        setEnrolling(false)
                        toast(
                            when (event.phase) {
                                "done" -> "指纹 #${event.id} 录入成功"
                                "canceled" -> "已取消录入"
                                else -> "录入失败：${event.note.ifBlank { event.phase }}"
                            }
                        )
                        if (event.phase == "done" && event.id >= 0) {
                            mqtt.optimisticUpsert(
                                FpItem(
                                    id = event.id,
                                    note = event.note.ifBlank { "指纹#${event.id}" },
                                    ts = System.currentTimeMillis() / 1000
                                )
                            )
                            // 固件 done 后已推 list；勿立刻再拉，避免与指示灯抢 UART 扫丢槽位
                        } else if (event.phase != "canceled") {
                            mqtt.cmd(JSONObject().put("type", "list"))
                        }
                    }
                    else -> {
                        setEnrolling(true)
                        binding.tvEnroll.text = phases[event.phase] ?: event.phase
                    }
                }
            }
            is MqttEvent.Op -> {
                when (event.type) {
                    "delete" -> when (event.phase) {
                        "ok" -> toast("已删除 #${event.id}")
                        "fail" -> {
                            toast("删除失败，请重试")
                            mqtt.cmd(JSONObject().put("type", "list"))
                        }
                    }
                    "clear" -> when (event.phase) {
                        "ok" -> toast("已清空指纹")
                        "fail" -> {
                            toast("清空失败，模块可能仍有指纹")
                            mqtt.cmd(JSONObject().put("type", "list"))
                        }
                    }
                    "rename" -> if (event.phase == "ok") toast("备注已更新")
                }
            }
            is MqttEvent.Error -> {
                if (event.code.contains("enroll") || event.code == "full") {
                    setEnrolling(false)
                    toast("错误：${event.code}")
                }
            }
            else -> Unit
        }
    }

    private fun startEnroll() {
        if (!mqtt.connected) {
            toast("MQTT 未连接")
            return
        }
        if (enrolling) {
            toast("已有录入进行中")
            return
        }
        val used = adapter.current.map { it.id }.toSet()
        var id = 0
        while (id in used) id++
        if (id >= 50) {
            toast("指纹库已满")
            return
        }
        AppDialogs.input(
            context = requireContext(),
            title = "录入指纹",
            message = "将使用槽位 #$id，按下开始后按提示放手指",
            hint = "备注（可留空）",
            confirmText = "开始录入",
            iconRes = R.drawable.ic_nav_fp_on
        ) { note ->
            setEnrolling(true)
            binding.tvEnroll.text = "指令已发送，请按手指…"
            mqtt.cmd(
                JSONObject().put("type", "enroll").put("id", id).put("note", note.take(24))
            )
            binding.tvEnroll.text = phases["wait_finger1"]
        }
    }

    private fun rename(item: FpItem) {
        AppDialogs.input(
            context = requireContext(),
            title = "修改备注",
            message = "指纹槽位 #${item.id}",
            hint = "备注",
            initial = item.note,
            confirmText = "保存",
            iconRes = R.drawable.ic_nav_fp_on
        ) { note ->
            mqtt.cmd(JSONObject().put("type", "rename").put("id", item.id).put("note", note.take(24)))
        }
    }

    private fun delete(item: FpItem) {
        AppDialogs.confirm(
            context = requireContext(),
            title = "删除指纹 #${item.id}？",
            message = "删除后需重新录入，确定吗？",
            confirmText = "删除",
            iconRes = R.drawable.ic_nav_fp_on,
            danger = true
        ) {
            // 等模块确认删除成功后再改列表，避免「列表空了还能开锁」
            mqtt.cmd(JSONObject().put("type", "delete").put("id", item.id))
        }
    }

    private fun setEnrolling(v: Boolean) {
        enrolling = v
        binding.cardEnroll.visibility = if (v) View.VISIBLE else View.GONE
        binding.btnEnroll.isEnabled = !v
        binding.btnEmptyEnroll.isEnabled = !v
    }

    private fun renderList(items: List<FpItem>) {
        adapter.submit(items)
        val empty = items.isEmpty()
        binding.emptyState.visibility = if (empty) View.VISIBLE else View.GONE
        binding.rvFingerprints.visibility = if (empty) View.GONE else View.VISIBLE
        binding.btnClearAll.visibility = if (empty) View.GONE else View.VISIBLE
    }

    private fun toast(msg: String) {
        android.widget.Toast.makeText(requireContext(), msg, android.widget.Toast.LENGTH_SHORT).show()
    }

    override fun onDestroyView() {
        _binding = null
        super.onDestroyView()
    }

    private class FpAdapter(
        val onRename: (FpItem) -> Unit,
        val onDelete: (FpItem) -> Unit
    ) : RecyclerView.Adapter<FpAdapter.VH>() {
        var current: List<FpItem> = emptyList()
            private set

        fun submit(list: List<FpItem>) {
            current = list
            notifyDataSetChanged()
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
            val b = ItemFingerprintBinding.inflate(LayoutInflater.from(parent.context), parent, false)
            return VH(b)
        }

        override fun getItemCount() = current.size

        override fun onBindViewHolder(holder: VH, position: Int) {
            val item = current[position]
            holder.b.tvIdNote.text = "#${item.id}  ${item.note.ifBlank { "未备注" }}"
            holder.b.tvTs.text = if (item.ts > 0) {
                SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault()).format(Date(item.ts * 1000))
            } else "录入时间未知"
            holder.b.btnRename.setOnClickListener { onRename(item) }
            holder.b.btnDelete.setOnClickListener { onDelete(item) }
        }

        class VH(val b: ItemFingerprintBinding) : RecyclerView.ViewHolder(b.root)
    }
}
