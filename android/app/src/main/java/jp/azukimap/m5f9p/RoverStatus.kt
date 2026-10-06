package jp.azukimap.m5f9p

import org.json.JSONObject

/** 測位の状態。値は本体のquality(NMEA GGAのFix quality)に対応する */
enum class FixQuality(val label: String) {
    NONE("測位不能"),
    SINGLE("単独測位"),
    DGPS("DGPS"),
    FIX("RTK Fix"),
    FLOAT("RTK Float"),
    ESTIMATED("推測");

    companion object {
        fun of(quality: Int) = when (quality) {
            1 -> SINGLE
            2 -> DGPS
            4 -> FIX
            5 -> FLOAT
            6 -> ESTIMATED
            else -> NONE
        }
    }
}

/** 本体が1秒毎に送ってくる状況（{"ev":"status",...}） */
data class RoverStatus(
    val version: String,
    val uptimeSec: Long,
    val posValid: Boolean,
    val quality: FixQuality,
    val lat: Double,
    val lon: Double,
    val height: Double,
    val sats: Int,
    val rateHz: Int,
    val baseValid: Boolean,
    val baseIsUart: Boolean,
    val baseAddress: String,
    val baseMount: String,
    val baseReady: Boolean,
    val baseBytes: Long,
    val baseReconnects: Int,
    val rtcmErrPercent: Int,
    val rtcmAgeMs: Long,
    val clasBytes: Long,          // NEO-D9Cが無い時は-1
    val saveReady: Boolean,
    val saving: Boolean,
    val saveCount: Int,
    val saveFormat: Int,
    val wifiConnected: Boolean,
    val wifiSsid: String,
    val wifiIp: String,
    val wifiRssi: Int,
    val sdMB: Int,
) {
    val saveFormatName: String
        get() = listOf("NMEA", "RAW", "RTCM", "CSV").getOrElse(saveFormat) { "?" }

    companion object {
        private const val BASE_TYPE_UART = 4

        fun parse(json: JSONObject): RoverStatus {
            val pos = json.optJSONObject("pos") ?: JSONObject()
            val base = json.optJSONObject("base") ?: JSONObject()
            val save = json.optJSONObject("save") ?: JSONObject()
            val wifi = json.optJSONObject("wifi") ?: JSONObject()
            return RoverStatus(
                version = json.optString("ver"),
                uptimeSec = json.optLong("uptime"),
                posValid = pos.optBoolean("valid"),
                quality = FixQuality.of(pos.optInt("quality")),
                lat = pos.optDouble("lat", 0.0),
                lon = pos.optDouble("lon", 0.0),
                height = pos.optDouble("height", 0.0),
                sats = pos.optInt("sats"),
                rateHz = json.optInt("rate", 1),
                baseValid = base.optBoolean("valid"),
                baseIsUart = base.optInt("type") == BASE_TYPE_UART,
                baseAddress = base.optString("address"),
                baseMount = base.optString("mount"),
                baseReady = base.optBoolean("ready"),
                baseBytes = base.optLong("bytes"),
                baseReconnects = base.optInt("reconnects"),
                rtcmErrPercent = base.optInt("rtcmErr"),
                rtcmAgeMs = base.optLong("rtcmAge"),
                clasBytes = json.optLong("clas", -1),
                saveReady = save.optBoolean("ready"),
                saving = save.optBoolean("on"),
                saveCount = save.optInt("count"),
                saveFormat = save.optInt("format"),
                wifiConnected = wifi.optBoolean("connected"),
                wifiSsid = wifi.optString("ssid"),
                wifiIp = wifi.optString("ip"),
                wifiRssi = wifi.optInt("rssi"),
                sdMB = json.optInt("sdMB"),
            )
        }
    }
}
