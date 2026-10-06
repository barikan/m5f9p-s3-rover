package jp.azukimap.m5f9p

import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat
import java.io.File

/**
 * 本体との接続。アプリ全体で1つ。
 *
 * 画面(Activity)が閉じていても接続を保つため、Activityではなくアプリケーションに
 * 持たせている。接続中はRoverServiceがプロセスを維持する。
 *
 * 状況の解釈やコマンドの組み立ては画面(web/src/rover.ts)が行う。ここは、BLEの通信と
 * ファイルの読み書きを画面に提供するだけ。
 */
object Rover {

    lateinit var ble: BleClient
        private set

    private lateinit var app: Context
    private lateinit var dataDir: File

    fun init(context: Context) {
        if (::ble.isInitialized) return
        app = context.applicationContext
        dataDir = File(app.filesDir, "data")
        ble = BleClient(app)
    }

    fun connect(address: String) {
        ContextCompat.startForegroundService(app, Intent(app, RoverService::class.java))
        ble.connect(address)
    }

    fun disconnect() {
        ble.disconnect()
        app.stopService(Intent(app, RoverService::class.java))
    }

    // ---------------------------------------------------------------- ファイル
    //
    // 画面からは "tracks/2026-10-06.csv" の形の名前で読み書きする（軌跡など）。
    // 置き場所はアプリの内部ストレージ。

    private val namePattern = Regex("""[\w.-]+(/[\w.-]+)*""")

    private fun fileOf(name: String): File? =
        if (namePattern.matches(name) && !name.contains("..")) File(dataDir, name) else null

    fun storageRead(name: String): String? = fileOf(name)?.takeIf { it.exists() }?.readText()

    fun storageAppend(name: String, text: String) {
        val file = fileOf(name) ?: return
        file.parentFile?.mkdirs()
        file.appendText(text)
    }

    fun storageList(): List<String> =
        dataDir.walkTopDown().filter { it.isFile }.map { it.relativeTo(dataDir).invariantSeparatorsPath }.toList()

    fun storageRemove(name: String) {
        fileOf(name)?.delete()
    }
}
