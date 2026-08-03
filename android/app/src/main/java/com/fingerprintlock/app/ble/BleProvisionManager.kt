package com.fingerprintlock.app.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import org.json.JSONObject
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap

data class BleDeviceItem(
    val device: BluetoothDevice,
    val name: String,
    val rssi: Int
)

class BleProvisionManager(private val context: Context) {
    interface Listener {
        fun onScanUpdate(devices: List<BleDeviceItem>)
        fun onTip(msg: String)
        fun onConnected()
        fun onStatus(json: String)
        fun onError(msg: String)
    }

    var listener: Listener? = null

    private val adapter: BluetoothAdapter? =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
    private val main = Handler(Looper.getMainLooper())
    private val found = ConcurrentHashMap<String, BleDeviceItem>()
    private var gatt: BluetoothGatt? = null
    private var configChar: BluetoothGattCharacteristic? = null
    private var statusChar: BluetoothGattCharacteristic? = null
    private var scanning = false
    private var pendingReadStatus = false

    private fun decodeUtf8(bytes: ByteArray?): String {
        if (bytes == null || bytes.isEmpty()) return ""
        return bytes.toString(Charsets.UTF_8).trim { it <= ' ' || it == '\u0000' }
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = result.scanRecord?.deviceName ?: result.device.name ?: return
            if (!name.startsWith(BleUuids.NAME_PREFIX)) return
            found[result.device.address] = BleDeviceItem(result.device, name, result.rssi)
            main.post { listener?.onScanUpdate(found.values.sortedByDescending { it.rssi }) }
        }

        override fun onScanFailed(errorCode: Int) {
            main.post { listener?.onError("扫描失败 code=$errorCode") }
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                main.post { listener?.onTip("已连接，发现服务…") }
                g.requestMtu(185)
                g.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                main.post { listener?.onTip("蓝牙已断开") }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val svc = g.getService(UUID.fromString(BleUuids.SERVICE))
            if (svc == null) {
                main.post { listener?.onError("未找到配网服务") }
                return
            }
            configChar = svc.getCharacteristic(UUID.fromString(BleUuids.CONFIG))
            statusChar = svc.getCharacteristic(UUID.fromString(BleUuids.STATUS))
            val ch = statusChar
            if (ch == null) {
                main.post { listener?.onError("未找到状态特征") }
                return
            }
            g.setCharacteristicNotification(ch, true)
            pendingReadStatus = true
            val cccd = ch.getDescriptor(UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"))
            if (cccd != null) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                } else {
                    @Suppress("DEPRECATION")
                    cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    @Suppress("DEPRECATION")
                    g.writeDescriptor(cccd)
                }
            } else {
                readStatus(g)
            }
            main.post {
                listener?.onConnected()
                listener?.onTip("蓝牙已就绪，可下发 WiFi")
            }
        }

        @SuppressLint("MissingPermission")
        override fun onDescriptorWrite(
            g: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int
        ) {
            if (pendingReadStatus) {
                pendingReadStatus = false
                readStatus(g)
            }
        }

        @SuppressLint("MissingPermission")
        private fun readStatus(g: BluetoothGatt) {
            val ch = statusChar ?: return
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.readCharacteristic(ch)
            } else {
                @Suppress("DEPRECATION")
                g.readCharacteristic(ch)
            }
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (characteristic.uuid == UUID.fromString(BleUuids.STATUS)) {
                @Suppress("DEPRECATION")
                val text = decodeUtf8(characteristic.value)
                if (text.isNotBlank()) main.post { listener?.onStatus(text) }
            }
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            if (characteristic.uuid == UUID.fromString(BleUuids.STATUS)) {
                val text = decodeUtf8(value)
                if (text.isNotBlank()) main.post { listener?.onStatus(text) }
            }
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicRead(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (characteristic.uuid == UUID.fromString(BleUuids.STATUS) && status == BluetoothGatt.GATT_SUCCESS) {
                @Suppress("DEPRECATION")
                val text = decodeUtf8(characteristic.value)
                if (text.isNotBlank()) main.post { listener?.onStatus(text) }
            }
        }

        override fun onCharacteristicRead(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int
        ) {
            if (characteristic.uuid == UUID.fromString(BleUuids.STATUS) && status == BluetoothGatt.GATT_SUCCESS) {
                val text = decodeUtf8(value)
                if (text.isNotBlank()) main.post { listener?.onStatus(text) }
            }
        }
    }

    fun isBluetoothEnabled(): Boolean = adapter?.isEnabled == true

    @SuppressLint("MissingPermission")
    fun startScan() {
        val scanner = adapter?.bluetoothLeScanner
        if (scanner == null) {
            listener?.onError("蓝牙不可用")
            return
        }
        found.clear()
        scanning = true
        listener?.onTip("正在扫描…")
        scanner.startScan(scanCallback)
        main.postDelayed({ if (scanning) stopScan() }, 20000)
    }

    @SuppressLint("MissingPermission")
    fun stopScan() {
        scanning = false
        try {
            adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (_: Exception) {
        }
        listener?.onTip(if (found.isEmpty()) "扫描结束，未发现设备" else "扫描结束")
    }

    @SuppressLint("MissingPermission")
    fun connect(device: BluetoothDevice) {
        stopScan()
        gatt?.close()
        listener?.onTip("正在连接 ${device.name ?: device.address}…")
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    @SuppressLint("MissingPermission")
    fun writeConfig(obj: JSONObject): Boolean {
        val g = gatt
        val ch = configChar
        if (g == null || ch == null) {
            listener?.onError("请先连接设备")
            return false
        }
        val bytes = obj.toString().toByteArray(Charsets.UTF_8)
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeCharacteristic(ch, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
                true
            } else {
                @Suppress("DEPRECATION")
                ch.value = bytes
                @Suppress("DEPRECATION")
                ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                @Suppress("DEPRECATION")
                g.writeCharacteristic(ch)
            }
        } catch (e: Exception) {
            listener?.onError(e.message ?: "写入失败")
            false
        }
    }

    fun requestStatus(): Boolean = writeConfig(JSONObject().put("cmd", "status"))

    @SuppressLint("MissingPermission")
    fun close() {
        stopScan()
        gatt?.close()
        gatt = null
        configChar = null
        statusChar = null
    }
}
