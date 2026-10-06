package jp.azukimap.m5f9p

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import androidx.core.content.ContextCompat
import java.io.ByteArrayOutputStream
import java.util.UUID

data class FoundDevice(val address: String, val name: String, val rssi: Int)

/**
 * 本体(M5F9P Rover)とのBLE通信。
 *
 * 本体はNordic UART Serviceと同じ形のサービスで、1行単位のテキストを送受信する。
 *   TX(notify): 状況 {"ev":"status",...}、測位データ(NMEA)、コマンドの応答
 *   RX(write) : コマンド(JSON)
 *
 * 受信した行の解釈は画面(web/src/rover.ts)が行う。ここは通信だけを受け持つ。
 * BluetoothGattの操作は同時に1つしか行えないので、すべてメインスレッドで順に行う。
 * 通知(Listener)もメインスレッドで呼ぶ。
 * 権限(BLUETOOTH_SCAN, BLUETOOTH_CONNECT)は呼び出し側で取得しておく。
 */
@SuppressLint("MissingPermission")
class BleClient(private val context: Context) {

    interface Listener {
        /** state: "disconnected" | "connecting" | "connected" */
        fun onState(state: String, name: String)
        fun onDevices(devices: List<FoundDevice>)
        fun onLine(line: String)
        /** 利用者に見せる文言（ペアリングの失敗など） */
        fun onMessage(text: String)
    }

    var listener: Listener? = null

    var state = STATE_DISCONNECTED
        private set
    var deviceName = ""
        private set

    private val handler = Handler(Looper.getMainLooper())
    private val adapter =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter

    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var mtu = 23
    private var wantAddress: String? = null     // 接続を維持したい相手。nullの時は再接続しない
    private val received = ByteArrayOutputStream()

    private val writeQueue = ArrayDeque<ByteArray>()
    private var writing = false

    private var scanning = false
    private val devices = mutableListOf<FoundDevice>()

    private fun setState(newState: String) {
        state = newState
        listener?.onState(state, deviceName)
    }

    // ---------------------------------------------------------------- スキャン

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            // サービスのUUIDで絞り込む。スキャン時のフィルタに任せると、UUIDがスキャン応答の
            // 側に入っている場合に見つからない端末があるので、ここで判定する。
            val uuids = result.scanRecord?.serviceUuids ?: return
            if (ParcelUuid(NUS_SERVICE) !in uuids) return
            val name = result.scanRecord?.deviceName ?: result.device.name ?: "(名称なし)"
            val found = FoundDevice(result.device.address, name, result.rssi)
            devices.removeAll { it.address == found.address }
            devices.add(found)
            devices.sortByDescending { it.rssi }
            listener?.onDevices(devices.toList())
        }

        override fun onScanFailed(errorCode: Int) {
            Log.w(TAG, "scan failed: $errorCode")
            scanning = false
        }
    }

    fun startScan() {
        val scanner = adapter?.bluetoothLeScanner ?: return
        if (scanning) stopScan()
        devices.clear()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanner.startScan(null, settings, scanCallback)
        scanning = true
        handler.postDelayed(::stopScan, SCAN_TOKEN, SCAN_TIMEOUT_MS)
    }

    fun stopScan() {
        handler.removeCallbacksAndMessages(SCAN_TOKEN)
        if (!scanning) return
        adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        scanning = false
    }

    // ---------------------------------------------------------------- 接続

    fun connect(address: String) {
        stopScan()
        wantAddress = address
        failedBonded = 0
        deviceName = devices.find { it.address == address }?.name ?: deviceName
        openGatt(address, autoConnect = false)
    }

    fun disconnect() {
        wantAddress = null
        handler.removeCallbacksAndMessages(RECONNECT_TOKEN)
        closeGatt()
        setState(STATE_DISCONNECTED)
    }

    private fun openGatt(address: String, autoConnect: Boolean) {
        closeGatt()
        val device = try {
            adapter?.getRemoteDevice(address)
        } catch (e: IllegalArgumentException) {
            null
        } ?: return
        setState(STATE_CONNECTING)
        gatt = device.connectGatt(context, autoConnect, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    private fun closeGatt() {
        gatt?.close()
        gatt = null
        rxChar = null
        pendingCccd = null
        cccdRetries = 0
        mtu = 23
        writeQueue.clear()
        writing = false
        received.reset()
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            handler.post {
                if (g != gatt) return@post
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    // アドレスだけで接続した時は名前が取れない事がある。その時は前の名前のままにする
                    // (空の時は、画面側が前回の接続で覚えた名前を表示する)
                    g.device.name?.let { deviceName = it }
                    // 既定のMTU(23)では1回に20バイトしか送れないので、最初に大きくする
                    if (!g.requestMtu(MTU_REQUEST)) g.discoverServices()
                } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    Log.i(TAG, "disconnected status=$status")
                    // ペアリング済みなのに、つながる前に切れる事が続く時は、本体側の記憶が
                    // 消されている（本体はこちらを知らない）可能性が高い
                    if (state == STATE_CONNECTING && g.device.bondState == BluetoothDevice.BOND_BONDED) {
                        if (++failedBonded == STALE_BOND_COUNT) listener?.onMessage(MESSAGE_STALE_BOND)
                    }
                    closeGatt()
                    val address = wantAddress
                    if (address == null) {
                        setState(STATE_DISCONNECTED)
                    } else {
                        // 本体の再起動などで切れた時は、見つかり次第つなぎ直す
                        setState(STATE_CONNECTING)
                        handler.postDelayed({ openGatt(address, autoConnect = true) }, RECONNECT_TOKEN, 1000)
                    }
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            handler.post {
                if (g != gatt) return@post
                if (status == BluetoothGatt.GATT_SUCCESS) mtu = newMtu
                g.discoverServices()
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            handler.post {
                if (g != gatt) return@post
                val service = g.getService(NUS_SERVICE)
                val tx = service?.getCharacteristic(NUS_TX)
                val rx = service?.getCharacteristic(NUS_RX)
                val cccd = tx?.getDescriptor(CCCD)
                if (tx == null || rx == null || cccd == null) {
                    Log.w(TAG, "NUS not found")
                    disconnect()
                    return@post
                }
                rxChar = rx
                g.setCharacteristicNotification(tx, true)
                enableNotification(g, cccd)
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            handler.post {
                if (g != gatt) return@post
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    pendingCccd = null
                    failedBonded = 0
                    setState(STATE_CONNECTED)
                    return@post
                }
                // 本体はペアリング（暗号化）が済むまで受け付けない。ペアリングが終わったら
                // やり直す（bondReceiver）。すでにペアリング済みなら、暗号化を待ってやり直す
                Log.i(TAG, "notification not enabled status=$status bond=${g.device.bondState}")
                pendingCccd = descriptor
                when (g.device.bondState) {
                    // まだペアリングしていない。アプリから始めると、番号を入力する画面が前面に出る
                    // （Androidが自分で始めた時は、通知になる事がある）
                    BluetoothDevice.BOND_NONE -> g.device.createBond()
                    BluetoothDevice.BOND_BONDED ->
                        if (cccdRetries++ < CCCD_RETRY_MAX) {
                            handler.postDelayed({ retryNotification() }, RECONNECT_TOKEN, 1000)
                        }
                    // BOND_BONDING の時は、終わるのを待つ（bondReceiver）
                }
            }
        }

        // Android 13以降
        override fun onCharacteristicChanged(
            g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray
        ) {
            handler.post { if (g == gatt) onReceive(value) }
        }

        // Android 12
        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) return
            @Suppress("DEPRECATION")
            val value = characteristic.value?.copyOf() ?: return
            handler.post { if (g == gatt) onReceive(value) }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int
        ) {
            handler.post {
                if (g != gatt) return@post
                writing = false
                writeNext()
            }
        }
    }

    // ---------------------------------------------------------------- ペアリング
    //
    // 本体は、ペアリング（暗号化）していない相手の読み書きを拒否する。拒否されたら
    // ペアリングを始める。番号（本体の画面に出る）の入力は、Androidの標準の画面が
    // 受け持つ。ここでは、結果を受けて続きを行う。

    private var pendingCccd: BluetoothGattDescriptor? = null    // 通知の有効化がやり直し待ち
    private var cccdRetries = 0
    private var failedBonded = 0        // ペアリング済みなのに、つながる前に切れた回数

    private fun enableNotification(g: BluetoothGatt, cccd: BluetoothGattDescriptor) {
        val enable = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(cccd, enable)
        } else {
            @Suppress("DEPRECATION")
            cccd.value = enable
            @Suppress("DEPRECATION")
            g.writeDescriptor(cccd)
        }
    }

    private fun retryNotification() {
        val g = gatt ?: return
        val cccd = pendingCccd ?: return
        enableNotification(g, cccd)
    }

    private val bondReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
            }
            if (device?.address != gatt?.device?.address) return
            val now = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.BOND_NONE)
            val before = intent.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, BluetoothDevice.BOND_NONE)
            Log.i(TAG, "bond $before -> $now")
            if (now == BluetoothDevice.BOND_BONDED) {
                cccdRetries = 0
                retryNotification()
            } else if (now == BluetoothDevice.BOND_NONE && before == BluetoothDevice.BOND_BONDING) {
                // 番号が違う、やめた、時間切れ
                listener?.onMessage(MESSAGE_PAIRING_FAILED)
                disconnect()
            }
        }
    }

    init {
        ContextCompat.registerReceiver(
            context, bondReceiver, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED),
            ContextCompat.RECEIVER_NOT_EXPORTED
        )
    }

    // ---------------------------------------------------------------- 受信

    /** 受信したバイト列を改行で区切り、1行ずつ渡す */
    private fun onReceive(value: ByteArray) {
        for (b in value) {
            if (b == '\n'.code.toByte()) {
                val line = received.toString(Charsets.UTF_8.name()).trimEnd('\r')
                received.reset()
                if (line.isNotEmpty()) listener?.onLine(line)
            } else if (received.size() < LINE_MAX) {
                received.write(b.toInt())
            }
        }
    }

    // ---------------------------------------------------------------- 送信

    /** 1行を送る。MTUに合わせて分割し、順に書き込む */
    fun sendLine(line: String) {
        handler.post {
            if (state != STATE_CONNECTED) return@post
            val bytes = (line + "\n").toByteArray(Charsets.UTF_8)
            val chunk = (mtu - 3).coerceAtLeast(20)
            var pos = 0
            while (pos < bytes.size) {
                val end = minOf(pos + chunk, bytes.size)
                writeQueue.addLast(bytes.copyOfRange(pos, end))
                pos = end
            }
            writeNext()
        }
    }

    private fun writeNext() {
        if (writing) return
        val g = gatt ?: return
        val rx = rxChar ?: return
        val data = writeQueue.removeFirstOrNull() ?: return
        val type = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(rx, data, type) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            rx.value = data
            rx.writeType = type
            @Suppress("DEPRECATION")
            g.writeCharacteristic(rx)
        }
        if (ok) writing = true
        else {
            Log.w(TAG, "write failed")
            writeQueue.clear()
        }
    }

    companion object {
        const val STATE_DISCONNECTED = "disconnected"
        const val STATE_CONNECTING = "connecting"
        const val STATE_CONNECTED = "connected"

        private const val TAG = "BleClient"
        private const val MTU_REQUEST = 247
        private const val SCAN_TIMEOUT_MS = 30_000L
        private const val LINE_MAX = 16384
        private const val CCCD_RETRY_MAX = 5
        private const val STALE_BOND_COUNT = 3
        private const val MESSAGE_PAIRING_FAILED = "ペアリングできませんでした。本体の画面に表示される番号を入力してください。"
        private const val MESSAGE_STALE_BOND =
            "接続できません。本体のペアリングを消した場合は、この端末の Bluetooth の設定で本体の登録を解除してから、つなぎ直してください。"
        private val RECONNECT_TOKEN = Any()
        private val SCAN_TOKEN = Any()

        val NUS_SERVICE: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
        val NUS_RX: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
        val NUS_TX: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
}
