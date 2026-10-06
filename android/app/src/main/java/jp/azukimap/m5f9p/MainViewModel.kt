package jp.azukimap.m5f9p

import android.app.Application
import android.content.Context
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import org.json.JSONException
import org.json.JSONObject

/** 1秒あたりの受信バイト数（状況の差分から求める） */
data class Throughput(val baseBytesPerSec: Int = 0, val clasBytesPerSec: Int = 0)

class MainViewModel(app: Application) : AndroidViewModel(app) {

    val ble = BleClient(app)
    val status = MutableStateFlow<RoverStatus?>(null)
    val throughput = MutableStateFlow(Throughput())
    val message = MutableStateFlow<String?>(null)   // コマンドのエラー等、利用者に見せる文言

    private val prefs = app.getSharedPreferences("m5f9p", Context.MODE_PRIVATE)

    init {
        viewModelScope.launch { ble.lines.collect(::onLine) }
        viewModelScope.launch {
            ble.state.collect { if (it != ConnState.CONNECTED) status.value = null }
        }
    }

    /** 前回接続した本体があれば接続する。権限を取得した後に呼ぶ */
    fun connectLast() {
        val address = prefs.getString(KEY_LAST_ADDRESS, null) ?: return
        if (ble.state.value == ConnState.DISCONNECTED && ble.isBluetoothEnabled) ble.connect(address)
    }

    fun connect(address: String) {
        prefs.edit().putString(KEY_LAST_ADDRESS, address).apply()
        ble.connect(address)
    }

    fun disconnect() {
        prefs.edit().remove(KEY_LAST_ADDRESS).apply()
        ble.disconnect()
    }

    fun setSaving(on: Boolean) = send(JSONObject().put("cmd", "save").put("on", on))

    fun setRate(hz: Int) = send(JSONObject().put("cmd", "rate").put("hz", hz))

    private fun send(command: JSONObject) = ble.sendLine(command.toString())

    private fun onLine(line: String) {
        if (!line.startsWith("{")) return      // NMEAは今は使わない
        val json = try {
            JSONObject(line)
        } catch (e: JSONException) {
            return
        }
        if (json.optString("ev") == "status") {
            val new = RoverStatus.parse(json)
            status.value?.let { old -> throughput.value = throughputOf(old, new) }
            status.value = new
        } else if (json.has("re") && !json.optBoolean("ok")) {
            message.value = "${json.optString("re")}: ${json.optString("error", "失敗しました")}"
        }
    }

    private fun throughputOf(old: RoverStatus, new: RoverStatus): Throughput {
        val seconds = new.uptimeSec - old.uptimeSec
        if (seconds <= 0) return throughput.value     // 本体の再起動、または同じ秒内
        fun rate(a: Long, b: Long) = if (b >= a) ((b - a) / seconds).toInt() else 0
        return Throughput(rate(old.baseBytes, new.baseBytes), rate(old.clasBytes, new.clasBytes))
    }

    override fun onCleared() {
        ble.disconnect()
    }

    companion object {
        private const val KEY_LAST_ADDRESS = "lastAddress"
    }
}
