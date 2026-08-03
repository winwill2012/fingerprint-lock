package com.fingerprintlock.app.ui

import android.Manifest
import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.fingerprintlock.app.R
import com.fingerprintlock.app.ble.BleDeviceItem
import com.fingerprintlock.app.ble.BleProvisionManager
import com.fingerprintlock.app.databinding.ActivityBleProvisionBinding
import com.fingerprintlock.app.databinding.DialogWifiPickerBinding
import com.fingerprintlock.app.databinding.ItemBleDeviceBinding
import com.fingerprintlock.app.databinding.ItemWifiNetworkBinding
import com.google.android.material.bottomsheet.BottomSheetDialog
import org.json.JSONObject

data class WifiNetworkItem(
    val ssid: String,
    val rssi: Int,
    val frequency: Int,
    val secured: Boolean
)

class BleProvisionActivity : AppCompatActivity(), BleProvisionManager.Listener {
    private lateinit var binding: ActivityBleProvisionBinding
    private lateinit var ble: BleProvisionManager
    private lateinit var wifiManager: WifiManager
    private val deviceAdapter = DeviceAdapter { connect(it) }
    private val wifiList = mutableListOf<WifiNetworkItem>()
    private var wifiSheetAdapter: WifiAdapter? = null
    private var wifiSheetBinding: DialogWifiPickerBinding? = null
    private var wifiSheet: BottomSheetDialog? = null
    private var connectedName: String = ""
    private var wifiReceiverRegistered = false
    private val mainHandler = Handler(Looper.getMainLooper())
    private var statusPollCount = 0

    private val statusPollRunnable = object : Runnable {
        override fun run() {
            if (statusPollCount <= 0) return
            statusPollCount--
            ble.requestStatus()
            if (statusPollCount > 0) {
                mainHandler.postDelayed(this, 1500)
            }
        }
    }

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        if (result.values.all { it }) {
            when (pendingPermissionAction) {
                PendingAction.BLE_SCAN -> startBleScanInternal()
                PendingAction.WIFI_SCAN -> {
                    startWifiScanInternal()
                    showWifiPicker()
                }
                else -> Unit
            }
        } else {
            Toast.makeText(this, "需要蓝牙、定位或附近 WiFi 权限", Toast.LENGTH_LONG).show()
        }
        pendingPermissionAction = PendingAction.NONE
    }

    private enum class PendingAction { NONE, BLE_SCAN, WIFI_SCAN }
    private var pendingPermissionAction = PendingAction.NONE

    private val wifiScanReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != WifiManager.SCAN_RESULTS_AVAILABLE_ACTION) return
            val ok = intent.getBooleanExtra(WifiManager.EXTRA_RESULTS_UPDATED, true)
            renderWifiResults(ok)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityBleProvisionBinding.inflate(layoutInflater)
        setContentView(binding.root)
        binding.toolbarBle.title = "BLE 配网"
        binding.toolbarBle.setNavigationIcon(androidx.appcompat.R.drawable.abc_ic_ab_back_material)
        binding.toolbarBle.setNavigationOnClickListener { finish() }

        wifiManager = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        ble = BleProvisionManager(this)
        ble.listener = this

        binding.rvDevices.layoutManager = LinearLayoutManager(this)
        binding.rvDevices.adapter = deviceAdapter

        binding.btnScan.setOnClickListener { ensurePermissionsAndBleScan() }
        binding.btnStopScan.setOnClickListener { ble.stopScan() }
        binding.btnSendWifi.setOnClickListener { sendWifi() }
        binding.tilSsid.setEndIconOnClickListener { openWifiPicker() }

        showScanPanel()
    }

    override fun onStart() {
        super.onStart()
        if (!wifiReceiverRegistered) {
            registerReceiver(wifiScanReceiver, IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION))
            wifiReceiverRegistered = true
        }
    }

    override fun onStop() {
        mainHandler.removeCallbacks(statusPollRunnable)
        if (wifiReceiverRegistered) {
            unregisterReceiver(wifiScanReceiver)
            wifiReceiverRegistered = false
        }
        super.onStop()
    }

    private fun showScanPanel() {
        binding.panelScan.visibility = View.VISIBLE
        binding.panelWifi.visibility = View.GONE
    }

    private fun showWifiPanel() {
        binding.panelScan.visibility = View.GONE
        binding.panelWifi.visibility = View.VISIBLE
        binding.tvConnectedDevice.text =
            if (connectedName.isNotBlank()) "已连接 $connectedName" else "已连接设备"
        binding.tvBleStatus.text = "蓝牙已就绪，请选择或输入 2.4GHz WiFi"
    }

    private fun blePermissions(): Array<String> {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.ACCESS_FINE_LOCATION
            )
        } else {
            arrayOf(
                Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.ACCESS_COARSE_LOCATION
            )
        }
    }

    private fun wifiPermissions(): Array<String> {
        val list = mutableListOf(
            Manifest.permission.ACCESS_FINE_LOCATION,
            Manifest.permission.ACCESS_COARSE_LOCATION
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            list.add(Manifest.permission.NEARBY_WIFI_DEVICES)
        }
        return list.toTypedArray()
    }

    private fun ensurePermissionsAndBleScan() {
        val need = blePermissions().filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (need.isNotEmpty()) {
            pendingPermissionAction = PendingAction.BLE_SCAN
            permissionLauncher.launch(need.toTypedArray())
        } else {
            startBleScanInternal()
        }
    }

    private fun openWifiPicker() {
        val need = wifiPermissions().filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (need.isNotEmpty()) {
            pendingPermissionAction = PendingAction.WIFI_SCAN
            permissionLauncher.launch(need.toTypedArray())
        } else {
            startWifiScanInternal()
            showWifiPicker()
        }
    }

    @SuppressLint("MissingPermission")
    private fun startBleScanInternal() {
        if (!ble.isBluetoothEnabled()) {
            Toast.makeText(this, "请先打开手机蓝牙", Toast.LENGTH_LONG).show()
            return
        }
        ble.startScan()
    }

    @SuppressLint("MissingPermission")
    private fun startWifiScanInternal() {
        if (!wifiManager.isWifiEnabled) {
            Toast.makeText(this, "请先打开手机 WiFi", Toast.LENGTH_LONG).show()
            return
        }
        wifiSheetBinding?.tvWifiSheetTip?.text = "正在扫描附近的 2.4GHz WiFi…"
        val started = wifiManager.startScan()
        if (!started) {
            renderWifiResults(false)
        }
    }

    private fun showWifiPicker() {
        if (wifiSheet?.isShowing == true) {
            wifiSheetAdapter?.submit(wifiList.toList())
            return
        }
        val sheetBinding = DialogWifiPickerBinding.inflate(layoutInflater)
        wifiSheetBinding = sheetBinding
        val adapter = WifiAdapter { item ->
            pickWifi(item)
            wifiSheet?.dismiss()
        }
        wifiSheetAdapter = adapter
        sheetBinding.rvWifiSheet.layoutManager = LinearLayoutManager(this)
        sheetBinding.rvWifiSheet.adapter = adapter
        adapter.submit(wifiList.toList())
        sheetBinding.btnWifiSheetRefresh.setOnClickListener { startWifiScanInternal() }
        sheetBinding.tvWifiSheetTip.text =
            if (wifiList.isEmpty()) "正在扫描…" else "已发现 ${wifiList.size} 个网络"

        val dialog = BottomSheetDialog(this)
        dialog.setContentView(sheetBinding.root)
        wifiSheet = dialog
        dialog.setOnDismissListener {
            wifiSheet = null
            wifiSheetBinding = null
            wifiSheetAdapter = null
        }
        dialog.show()
    }

    @SuppressLint("MissingPermission")
    private fun renderWifiResults(updated: Boolean) {
        val results = try {
            wifiManager.scanResults
        } catch (_: SecurityException) {
            emptyList()
        }
        val mapped = results
            .asSequence()
            .filter { it.frequency in 2400..2500 }
            .mapNotNull { r ->
                val ssid = r.SSID?.trim().orEmpty()
                if (ssid.isEmpty() || ssid == "<unknown ssid>") return@mapNotNull null
                WifiNetworkItem(
                    ssid = ssid,
                    rssi = r.level,
                    frequency = r.frequency,
                    secured = r.capabilities?.let { cap ->
                        cap.contains("WPA") || cap.contains("WEP") ||
                            cap.contains("PSK") || cap.contains("SAE") ||
                            cap.contains("EAP")
                    } == true
                )
            }
            .groupBy { it.ssid }
            .map { (_, list) -> list.maxBy { it.rssi } }
            .sortedByDescending { it.rssi }
            .take(20)
            .toList()

        wifiList.clear()
        wifiList.addAll(mapped)
        wifiSheetAdapter?.submit(mapped)
        wifiSheetBinding?.tvWifiSheetTip?.text = when {
            mapped.isEmpty() ->
                if (updated) "未发现 2.4GHz WiFi，可手动输入"
                else "暂无结果，请靠近路由器后刷新"
            else -> "已发现 ${mapped.size} 个 2.4GHz 网络"
        }
    }

    @SuppressLint("MissingPermission")
    private fun connect(item: BleDeviceItem) {
        connectedName = item.name
        ble.connect(item.device)
    }

    private fun pickWifi(item: WifiNetworkItem) {
        binding.etSsid.setText(item.ssid)
        binding.etSsid.setSelection(item.ssid.length)
        binding.etWifiPass.requestFocus()
        Toast.makeText(this, "已选择 ${item.ssid}", Toast.LENGTH_SHORT).show()
    }

    private fun sendWifi() {
        val ssid = binding.etSsid.text?.toString()?.trim().orEmpty()
        if (ssid.isEmpty()) {
            Toast.makeText(this, "请选择或填写 WiFi 名称", Toast.LENGTH_SHORT).show()
            return
        }
        val pass = binding.etWifiPass.text?.toString().orEmpty()
        binding.tvBleStatus.text = "正在把「$ssid」发送给设备…"
        if (!ble.writeConfig(JSONObject().put("ssid", ssid).put("password", pass))) {
            binding.tvBleStatus.text = "发送失败，请确认蓝牙仍已连接"
            return
        }
        binding.tvBleStatus.text = "已发送，设备正在连接「$ssid」…"
        // 轮询状态，避免通知丢失时一直卡在“更新中”
        mainHandler.removeCallbacks(statusPollRunnable)
        statusPollCount = 20
        mainHandler.postDelayed(statusPollRunnable, 800)
    }

    override fun onScanUpdate(devices: List<BleDeviceItem>) {
        deviceAdapter.submit(devices)
        binding.tvBleTip.text = "已发现 ${devices.size} 台设备"
    }

    override fun onTip(msg: String) {
        if (binding.panelScan.visibility == View.VISIBLE) {
            binding.tvBleTip.text = msg
        }
    }

    override fun onConnected() {
        showWifiPanel()
        Toast.makeText(this, "蓝牙连接成功", Toast.LENGTH_SHORT).show()
    }

    override fun onStatus(json: String) {
        val text = formatWifiStatus(json)
        binding.tvBleStatus.text = text
        try {
            val wifi = JSONObject(json).optString("wifi")
            if (wifi == "connected") {
                mainHandler.removeCallbacks(statusPollRunnable)
                statusPollCount = 0
                Toast.makeText(this, "配网成功", Toast.LENGTH_SHORT).show()
            } else if (wifi == "failed") {
                mainHandler.removeCallbacks(statusPollRunnable)
                statusPollCount = 0
            }
        } catch (_: Exception) {
        }
    }

    private fun formatWifiStatus(raw: String): String {
        val cleaned = raw.trim().trim('\u0000')
        return try {
            val o = JSONObject(cleaned)
            val wifi = o.optString("wifi")
            val ssid = o.optString("ssid")
            val ip = o.optString("ip")
            val rssi = if (o.has("rssi")) o.optInt("rssi") else Int.MIN_VALUE
            val name = if (ssid.isNotBlank()) "「$ssid」" else "路由器"
            when (wifi) {
                "connected" -> buildString {
                    append("✅ 已成功连上 $name")
                    if (ip.isNotBlank()) append("\n设备 IP：$ip")
                    if (rssi != Int.MIN_VALUE) append("\n信号强度：${rssi} dBm")
                    append("\n可以返回设置页连接 MQTT 了")
                }
                "connecting" -> "⏳ 正在连接 $name，请稍候…"
                "failed" -> "❌ 连接 $name 失败\n请确认是 2.4GHz 网络，并检查密码是否正确"
                "disconnected" -> "已保存 WiFi，但当前未连上\n可点击「立即连接」重试"
                "idle" -> "尚未配置 WiFi"
                else -> {
                    if (ip.isNotBlank()) "✅ 设备已联网\nIP：$ip"
                    else if (ssid.isNotBlank()) "设备当前网络：$ssid"
                    else "等待设备反馈…"
                }
            }
        } catch (_: Exception) {
            if (cleaned.contains("connected", true)) "✅ 配网成功"
            else if (cleaned.contains("connecting", true)) "⏳ 正在连接路由器…"
            else if (cleaned.contains("failed", true)) "❌ 连接失败，请检查密码"
            else "等待设备反馈…"
        }
    }

    override fun onError(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
        if (binding.panelScan.visibility == View.VISIBLE) {
            binding.tvBleTip.text = msg
        } else {
            binding.tvBleStatus.text = msg
        }
    }

    override fun onDestroy() {
        mainHandler.removeCallbacks(statusPollRunnable)
        ble.close()
        super.onDestroy()
    }

    private class DeviceAdapter(
        val onClick: (BleDeviceItem) -> Unit
    ) : RecyclerView.Adapter<DeviceAdapter.VH>() {
        private var data: List<BleDeviceItem> = emptyList()

        fun submit(list: List<BleDeviceItem>) {
            data = list
            notifyDataSetChanged()
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
            val b = ItemBleDeviceBinding.inflate(LayoutInflater.from(parent.context), parent, false)
            return VH(b)
        }

        override fun getItemCount() = data.size

        @SuppressLint("MissingPermission")
        override fun onBindViewHolder(holder: VH, position: Int) {
            val item = data[position]
            holder.b.tvName.text = item.name
            holder.b.tvMeta.text = "信号 ${item.rssi} dBm · ${item.device.address}"
            holder.b.btnConnect.setOnClickListener { onClick(item) }
            holder.b.root.setOnClickListener { onClick(item) }
        }

        class VH(val b: ItemBleDeviceBinding) : RecyclerView.ViewHolder(b.root)
    }

    private class WifiAdapter(
        val onClick: (WifiNetworkItem) -> Unit
    ) : RecyclerView.Adapter<WifiAdapter.VH>() {
        private var data: List<WifiNetworkItem> = emptyList()

        fun submit(list: List<WifiNetworkItem>) {
            data = list
            notifyDataSetChanged()
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
            val b = ItemWifiNetworkBinding.inflate(LayoutInflater.from(parent.context), parent, false)
            return VH(b)
        }

        override fun getItemCount() = data.size

        override fun onBindViewHolder(holder: VH, position: Int) {
            val item = data[position]
            holder.b.tvWifiSsid.text = item.ssid
            val alpha = when {
                item.rssi >= -55 -> 1f
                item.rssi >= -70 -> 0.75f
                else -> 0.5f
            }
            holder.b.ivWifiSignal.alpha = alpha
            holder.b.ivWifiLock.visibility = if (item.secured) View.VISIBLE else View.GONE
            holder.b.rootWifiItem.setOnClickListener { onClick(item) }
        }

        class VH(val b: ItemWifiNetworkBinding) : RecyclerView.ViewHolder(b.root)
    }
}
