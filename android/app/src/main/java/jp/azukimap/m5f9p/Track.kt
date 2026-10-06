package jp.azukimap.m5f9p

import android.content.Context
import android.util.Log
import java.io.File
import java.time.LocalDate
import java.util.Locale
import kotlin.math.cos
import kotlin.math.hypot

/** 軌跡の1点。timeはUnix時刻（ミリ秒） */
data class TrackPoint(val time: Long, val lat: Double, val lon: Double, val quality: FixQuality) {

    /** 2点間の距離（m）。近距離用の近似 */
    fun distanceTo(other: TrackPoint): Double {
        val north = (other.lat - lat) * METERS_PER_DEGREE
        val east = (other.lon - lon) * METERS_PER_DEGREE * cos(Math.toRadians(lat))
        return hypot(north, east)
    }

    companion object {
        private const val METERS_PER_DEGREE = 111_320.0
    }
}

/**
 * 軌跡の保存と読み出し。
 *
 * 1日1ファイルのCSV（時刻,緯度,経度,quality）で、アプリの内部ストレージに置く。
 * ファイル名は端末の現地時間の日付。
 */
class TrackStore(context: Context) {

    private val dir = File(context.filesDir, "tracks").apply { mkdirs() }

    /** 保存されている日付。新しい順 */
    fun days(): List<LocalDate> = dir.listFiles().orEmpty()
        .mapNotNull { runCatching { LocalDate.parse(it.name.removeSuffix(".csv")) }.getOrNull() }
        .sortedDescending()

    fun load(day: LocalDate): List<TrackPoint> {
        val file = fileOf(day)
        if (!file.exists()) return emptyList()
        return file.useLines { lines ->
            lines.mapNotNull { line ->
                val f = line.split(',')
                if (f.size < 4) return@mapNotNull null
                runCatching {
                    TrackPoint(f[0].toLong(), f[1].toDouble(), f[2].toDouble(), FixQuality.entries[f[3].toInt()])
                }.getOrNull()
            }.toList()
        }
    }

    fun append(day: LocalDate, point: TrackPoint) {
        try {
            fileOf(day).appendText(
                String.format(Locale.US, "%d,%.9f,%.9f,%d\n", point.time, point.lat, point.lon, point.quality.ordinal)
            )
        } catch (e: Exception) {
            Log.w("TrackStore", "append failed", e)
        }
    }

    fun delete(day: LocalDate) {
        fileOf(day).delete()
    }

    private fun fileOf(day: LocalDate) = File(dir, "$day.csv")
}
