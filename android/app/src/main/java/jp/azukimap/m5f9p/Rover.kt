package jp.azukimap.m5f9p

import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId

/** 1秒あたりの受信バイト数（状況の差分から求める） */
data class Throughput(val baseBytesPerSec: Int = 0, val clasBytesPerSec: Int = 0)

/** 本体の起動時の設定と、選べる候補（run.getの応答） */
data class RunConfig(
    val wifi: Int,              // INIファイルのWifi接続先番号(1から)。0:使わない
    val source: Int,            // 補正データの取得先。-1:接続しない 0:UART 1以上:INIファイルに書かれた順
    val format: Int,            // 保存形式 0:NMEA 1:RAW 2:RTCM 3:CSV
    val saveAtBoot: Boolean,
    val wifiList: List<String>,
    val sourceList: List<String>,
)

/**
 * 本体との通信と、そこから得たデータ。アプリ全体で1つ。
 *
 * 画面(Activity)が閉じていても接続と軌跡の記録を続けるため、Activityではなく
 * アプリケーションに持たせている。接続中はRoverServiceがプロセスを維持する。
 */
object Rover {

    lateinit var ble: BleClient
        private set

    val status = MutableStateFlow<RoverStatus?>(null)
    val throughput = MutableStateFlow(Throughput())
    val message = MutableStateFlow<String?>(null)   // コマンドの結果等、利用者に見せる文言

    /** 地図に表示している日と、その日の軌跡 */
    val trackDay = MutableStateFlow(LocalDate.now())
    val track = MutableStateFlow<List<TrackPoint>>(emptyList())
    val syncing = MutableStateFlow(false)           // 本体が保持している軌跡を取得している間 true

    val runConfig = MutableStateFlow<RunConfig?>(null)
    val iniText = MutableStateFlow<String?>(null)

    /** Google MapsのAPIキー。利用者がアプリに入力したものと、本体のINIファイルにあるもの */
    val userMapsKey = MutableStateFlow("")
    val deviceMapsKey = MutableStateFlow("")

    private lateinit var app: Context
    private lateinit var prefs: SharedPreferences
    private lateinit var trackStore: TrackStore
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    private var lastPoint: TrackPoint? = null       // 最後に記録した点
    private var syncTimeout: Job? = null

    fun init(context: Context) {
        if (::ble.isInitialized) return
        app = context.applicationContext
        prefs = app.getSharedPreferences("m5f9p", Context.MODE_PRIVATE)
        trackStore = TrackStore(app)
        ble = BleClient(app)

        userMapsKey.value = prefs.getString(KEY_USER_MAPS_KEY, "") ?: ""
        deviceMapsKey.value = prefs.getString(KEY_DEVICE_MAPS_KEY, "") ?: ""
        track.value = trackStore.load(trackDay.value)

        scope.launch { ble.lines.collect(::onLine) }
        scope.launch { ble.state.collect(::onStateChanged) }
    }

    // ---------------------------------------------------------------- 接続

    /** 前回接続した本体があれば接続する。権限を取得した後に呼ぶ */
    fun connectLast() {
        val address = prefs.getString(KEY_LAST_ADDRESS, null) ?: return
        if (ble.state.value == ConnState.DISCONNECTED && ble.isBluetoothEnabled) connect(address)
    }

    fun connect(address: String) {
        prefs.edit().putString(KEY_LAST_ADDRESS, address).apply()
        ContextCompat.startForegroundService(app, Intent(app, RoverService::class.java))
        ble.connect(address)
    }

    fun disconnect() {
        prefs.edit().remove(KEY_LAST_ADDRESS).apply()
        ble.disconnect()
        app.stopService(Intent(app, RoverService::class.java))
    }

    private fun onStateChanged(state: ConnState) {
        if (state != ConnState.CONNECTED) {
            status.value = null
            runConfig.value = null
            endSync()
            return
        }
        // 位置は状況(1秒毎)から取るので、NMEAは止めて無線の占有を減らす
        send(JSONObject().put("cmd", "nmea").put("hz", 0))
        send(JSONObject().put("cmd", "map.key"))
        startSync()
    }

    // ---------------------------------------------------------------- 操作

    fun setSaving(on: Boolean) = send(JSONObject().put("cmd", "save").put("on", on))

    fun setRate(hz: Int) = send(JSONObject().put("cmd", "rate").put("hz", hz))

    fun loadRunConfig() = send(JSONObject().put("cmd", "run.get"))

    /** 起動時の設定を書き換える。本体は再起動する */
    fun applyRunConfig(wifi: Int, source: Int, format: Int, saveAtBoot: Boolean) = send(
        JSONObject().put("cmd", "run.set")
            .put("wifi", wifi).put("source", source).put("format", format).put("saveAtBoot", saveAtBoot)
    )

    fun loadIni() {
        iniText.value = null
        send(JSONObject().put("cmd", "ini.get"))
    }

    /** INIファイルを書き換える。成功したら本体を再起動する */
    fun saveIni(text: String) = send(JSONObject().put("cmd", "ini.put").put("text", text))

    fun restart() = send(JSONObject().put("cmd", "restart"))

    /** 本体を再起動し、本体の画面で設定を選び直す */
    fun requestSetup() = send(JSONObject().put("cmd", "setup"))

    fun setUserMapsKey(key: String) {
        userMapsKey.value = key.trim()
        prefs.edit().putString(KEY_USER_MAPS_KEY, userMapsKey.value).apply()
    }

    private fun send(command: JSONObject) = ble.sendLine(command.toString())

    // ---------------------------------------------------------------- 受信

    private fun onLine(line: String) {
        if (!line.startsWith("{")) return      // NMEAは使わない
        val json = try {
            JSONObject(line)
        } catch (e: JSONException) {
            return
        }
        if (json.optString("ev") == "status") {
            val new = RoverStatus.parse(json)
            status.value?.let { old -> throughput.value = throughputOf(old, new) }
            status.value = new
            record(new)
            return
        }
        val ok = json.optBoolean("ok")
        when (json.optString("re")) {
            "run.get" -> if (ok) runConfig.value = parseRunConfig(json)
            "run.set" -> if (ok) message.value = "設定を保存しました。本体を再起動します"
            "ini.get" -> if (ok) iniText.value = json.optString("text")
            "ini.put" -> if (ok) {
                message.value = "INIファイルを保存しました。本体を再起動します"
                restart()
            }
            "map.key" -> if (ok) {
                deviceMapsKey.value = json.optString("key")
                prefs.edit().putString(KEY_DEVICE_MAPS_KEY, deviceMapsKey.value).apply()
            }
            "track.get" -> if (ok) onTrackReply(json) else endSync()
        }
        if (!ok && json.has("re")) {
            message.value = "${json.optString("re")}: ${json.optString("error", "失敗しました")}"
        }
    }

    private fun parseRunConfig(json: JSONObject) = RunConfig(
        wifi = json.optInt("wifi"),
        source = json.optInt("source", -1),
        format = json.optInt("format"),
        saveAtBoot = json.optBoolean("saveAtBoot"),
        wifiList = json.optJSONArray("wifiList").toStringList(),
        sourceList = json.optJSONArray("sourceList").toStringList(),
    )

    private fun JSONArray?.toStringList(): List<String> =
        if (this == null) emptyList() else List(length()) { optString(it) }

    private fun throughputOf(old: RoverStatus, new: RoverStatus): Throughput {
        val seconds = new.uptimeSec - old.uptimeSec
        if (seconds <= 0) return throughput.value     // 本体の再起動、または同じ秒内
        fun rate(a: Long, b: Long) = if (b >= a) ((b - a) / seconds).toInt() else 0
        return Throughput(rate(old.baseBytes, new.baseBytes), rate(old.clasBytes, new.clasBytes))
    }

    // ---------------------------------------------------------------- 軌跡

    fun trackDays(): List<LocalDate> = trackStore.days()

    fun selectTrackDay(day: LocalDate) {
        trackDay.value = day
        track.value = trackStore.load(day)
    }

    fun deleteTrackDay(day: LocalDate) {
        trackStore.delete(day)
        if (day == LocalDate.now()) lastPoint = null
        if (day == trackDay.value) track.value = emptyList()
    }

    /**
     * 接続していなかった間の軌跡を本体から取得する。
     *
     * 本体は電源が入っている間の軌跡を保持している。最後に記録した時刻より後の分を、
     * 少しずつ取り出して保存する。取得している間は、状況からの記録を止める（同じ区間が
     * 本体の軌跡にも入っていて、順序が前後するため）。
     */
    private fun startSync() {
        syncing.value = true
        requestTrack(prefs.getLong(KEY_LAST_TRACK_TIME, 0) / 1000)
    }

    private fun requestTrack(sinceSec: Long) {
        send(JSONObject().put("cmd", "track.get").put("since", sinceSec))
        // 応答が無い時（古いファームウェア等）は、あきらめて通常の記録に戻る
        syncTimeout?.cancel()
        syncTimeout = scope.launch {
            delay(SYNC_TIMEOUT_MS)
            endSync()
        }
    }

    private fun onTrackReply(json: JSONObject) {
        val pts = json.optJSONArray("pts") ?: JSONArray()
        var lastSec = 0L
        val points = List(pts.length()) {
            val p = pts.getJSONArray(it)
            lastSec = p.getLong(0)
            TrackPoint(lastSec * 1000, p.getDouble(1), p.getDouble(2), FixQuality.of(p.getInt(3)))
        }
        store(points)
        if (json.optBoolean("more") && points.isNotEmpty()) requestTrack(lastSec) else endSync()
    }

    private fun endSync() {
        syncTimeout?.cancel()
        syncing.value = false
    }

    /** 測位結果を軌跡に加える。止まっている間は増やさない */
    private fun record(s: RoverStatus) {
        if (syncing.value || !s.posValid || s.quality == FixQuality.NONE) return
        val point = TrackPoint(System.currentTimeMillis(), s.lat, s.lon, s.quality)
        val last = lastPoint
        if (last != null && last.quality == point.quality && last.distanceTo(point) < TRACK_MIN_DISTANCE) return
        store(listOf(point))
    }

    /** 軌跡の点を、時刻（端末の現地時間）の日付のファイルに保存する */
    private fun store(points: List<TrackPoint>) {
        if (points.isEmpty()) return
        val zone = ZoneId.systemDefault()
        val shown = mutableListOf<TrackPoint>()
        for (point in points) {
            val day = Instant.ofEpochMilli(point.time).atZone(zone).toLocalDate()
            trackStore.append(day, point)
            if (day == trackDay.value) shown.add(point)
        }
        lastPoint = points.last()
        prefs.edit().putLong(KEY_LAST_TRACK_TIME, points.last().time).apply()
        if (shown.isNotEmpty()) track.value = track.value + shown
    }

    private const val KEY_LAST_ADDRESS = "lastAddress"
    private const val KEY_LAST_TRACK_TIME = "lastTrackTime"
    private const val KEY_USER_MAPS_KEY = "userMapsKey"
    private const val KEY_DEVICE_MAPS_KEY = "deviceMapsKey"
    private const val TRACK_MIN_DISTANCE = 0.05     // m。これ以上動いた時に軌跡の点を増やす
    private const val SYNC_TIMEOUT_MS = 5000L
}
