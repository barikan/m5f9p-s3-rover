package jp.azukimap.m5f9p

import android.annotation.SuppressLint
import android.os.Handler
import android.os.Looper
import android.view.ViewGroup
import android.webkit.JavascriptInterface
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import java.time.LocalDate
import java.util.Locale

fun fixColor(quality: FixQuality) = when (quality) {
    FixQuality.FIX -> Color(0xFF2E7D32)
    FixQuality.FLOAT -> Color(0xFFEF6C00)
    FixQuality.NONE -> Color(0xFFC62828)
    else -> Color(0xFF546E7A)
}

/** 本体のquality（map.htmlの色分けに使う値） */
private fun qualityCode(quality: FixQuality) = when (quality) {
    FixQuality.FIX -> 4
    FixQuality.FLOAT -> 5
    FixQuality.NONE -> 0
    else -> 1
}

/** map.html（Maps JavaScript API）から呼ばれる。WebViewのスレッドから呼ばれるので、メインスレッドへ渡す */
private class MapBridge(val onReady: () -> Unit, val onFollow: (Boolean) -> Unit, val onAuthFailure: () -> Unit) {
    private val handler = Handler(Looper.getMainLooper())

    @JavascriptInterface
    fun onReady() = handler.post { onReady.invoke() }.let {}

    @JavascriptInterface
    fun onFollow(on: Boolean) = handler.post { onFollow.invoke(on) }.let {}

    @JavascriptInterface
    fun onAuthFailure() = handler.post { onAuthFailure.invoke() }.let {}
}

/**
 * 地図。現在地と、選んだ日の軌跡を表示する。
 *
 * APIキーを実行時に渡せるよう、WebViewでMaps JavaScript APIを使っている。
 * キーは、アプリの設定画面で入力したものを優先し、無ければ本体のINIファイルのものを使う。
 */
@SuppressLint("SetJavaScriptEnabled")
@Composable
fun MapScreen() {
    val status by Rover.status.collectAsStateWithLifecycle()
    val track by Rover.track.collectAsStateWithLifecycle()
    val day by Rover.trackDay.collectAsStateWithLifecycle()
    val syncing by Rover.syncing.collectAsStateWithLifecycle()
    val userKey by Rover.userMapsKey.collectAsStateWithLifecycle()
    val deviceKey by Rover.deviceMapsKey.collectAsStateWithLifecycle()
    val key = userKey.ifBlank { deviceKey }

    var follow by rememberSaveable { mutableStateOf(true) }
    var satellite by rememberSaveable { mutableStateOf(false) }
    var showDays by remember { mutableStateOf(false) }
    var ready by remember(key) { mutableStateOf(false) }
    var authFailed by remember(key) { mutableStateOf(false) }
    var drawn by remember(key) { mutableIntStateOf(-1) }     // 地図に描いた軌跡の点数。-1:未描画

    val isToday = day == LocalDate.now()
    val current = status?.takeIf { it.posValid && isToday }

    if (key.isBlank()) {
        Column(Modifier.padding(24.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Google Maps の API キーが設定されていません。")
            Text(
                "「設定」タブでキーを入力するか、本体の INI ファイルの [google] key に書いてください。" +
                    "Google Cloud で Maps JavaScript API を有効にしたキーが必要です。",
                style = MaterialTheme.typography.bodySmall,
            )
        }
        return
    }

    val context = LocalContext.current
    val webView = remember(key) {
        WebView(context).apply {
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT
            )
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            webViewClient = WebViewClient()
            addJavascriptInterface(
                MapBridge(
                    onReady = { ready = true },
                    onFollow = { follow = it },
                    onAuthFailure = { authFailed = true },
                ),
                "Android",
            )
            val start = Rover.status.value?.takeIf { it.posValid }?.let { it.lat to it.lon }
                ?: Rover.track.value.lastOrNull()?.let { it.lat to it.lon }
            val html = context.assets.open("map.html").bufferedReader().use { it.readText() }
                .replace("%KEY%", key)
                .replace("%LAT%", (start?.first ?: 35.681236).toString())
                .replace("%LON%", (start?.second ?: 139.767125).toString())
                .replace("%ZOOM%", if (start != null) "19" else "5")
            // キーの「ウェブサイトの制限」に登録できるよう、固定のURLから読み込んだ扱いにする
            loadDataWithBaseURL(MAP_BASE_URL, html, "text/html", "utf-8", null)
        }
    }
    DisposableEffect(webView) { onDispose { webView.destroy() } }

    fun js(script: String) = webView.evaluateJavascript(script, null)

    // 軌跡。1点増えただけの時は追加し、それ以外（日の切り替え、本体からの取得）は描き直す
    LaunchedEffect(ready, track) {
        if (!ready) return@LaunchedEffect
        if (drawn >= 0 && track.size == drawn + 1) {
            val p = track.last()
            js(String.format(Locale.US, "addPoint(%.9f,%.9f,%d)", p.lat, p.lon, qualityCode(p.quality)))
        } else if (track.size != drawn) {
            val points = track.joinToString(",", "[", "]") {
                String.format(Locale.US, "[%.9f,%.9f,%d]", it.lat, it.lon, qualityCode(it.quality))
            }
            js("setTrack($points)")
        }
        drawn = track.size
    }
    LaunchedEffect(ready, current) {
        if (!ready) return@LaunchedEffect
        if (current == null) js("hideCurrent()")
        else js(String.format(Locale.US, "setCurrent(%.9f,%.9f,%d)", current.lat, current.lon, qualityCode(current.quality)))
    }
    LaunchedEffect(ready, satellite) {
        if (ready) js("setMapType('${if (satellite) "hybrid" else "roadmap"}')")
    }
    LaunchedEffect(ready, follow) {
        if (ready) js("setFollow($follow)")
    }
    // 過去の日を選んだ時は、その軌跡の最後の点へ移動する
    LaunchedEffect(ready, day) {
        if (ready && !isToday) {
            Rover.track.value.lastOrNull()?.let { js(String.format(Locale.US, "moveTo(%.9f,%.9f,18)", it.lat, it.lon)) }
        }
    }

    Box(Modifier.fillMaxSize()) {
        AndroidView(factory = { webView }, modifier = Modifier.fillMaxSize())

        Surface(
            Modifier.padding(12.dp),
            shape = MaterialTheme.shapes.medium, tonalElevation = 2.dp, shadowElevation = 2.dp,
        ) {
            Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
                val s = status
                if (authFailed) {
                    Text("API キーが使えません", color = MaterialTheme.colorScheme.error)
                    Text(
                        "キーが正しいか、Maps JavaScript API が有効かを確認してください。",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                if (s != null && isToday) {
                    Text(s.quality.label, color = fixColor(s.quality), style = MaterialTheme.typography.titleMedium)
                    if (s.posValid) {
                        Text(String.format(Locale.US, "%.8f, %.8f", s.lat, s.lon), style = MaterialTheme.typography.bodySmall)
                    }
                } else if (isToday) {
                    Text("本体に接続していません", style = MaterialTheme.typography.bodyMedium)
                }
                Text(
                    "${if (isToday) "今日" else day.toString()}の軌跡 ${track.size} 点" + if (syncing) "（本体から取得中）" else "",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        Row(
            Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .padding(12.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
        ) {
            if (!follow && current != null) FilledTonalButton(onClick = { follow = true }) { Text("現在地") }
            FilledTonalButton(onClick = { satellite = !satellite }) { Text(if (satellite) "地図" else "航空写真") }
            FilledTonalButton(onClick = { showDays = true }) { Text("履歴") }
        }
    }

    if (showDays) {
        DayDialog(
            days = Rover.trackDays(),
            selected = day,
            onSelect = {
                Rover.selectTrackDay(it)
                showDays = false
            },
            onDelete = Rover::deleteTrackDay,
            onDismiss = { showDays = false },
        )
    }
}

@Composable
private fun DayDialog(
    days: List<LocalDate>,
    selected: LocalDate,
    onSelect: (LocalDate) -> Unit,
    onDelete: (LocalDate) -> Unit,
    onDismiss: () -> Unit,
) {
    var list by remember { mutableStateOf(days) }
    val today = LocalDate.now()

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("表示する日") },
        text = {
            Column {
                // 今日は軌跡がまだ無くても選べるようにする
                for (d in (listOf(today) + list).distinct()) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        TextButton(onClick = { onSelect(d) }, modifier = Modifier.weight(1f)) {
                            Text(
                                (if (d == today) "今日" else d.toString()) + (if (d == selected) "（表示中）" else ""),
                                Modifier.fillMaxWidth(),
                            )
                        }
                        if (d in list) {
                            TextButton(onClick = {
                                onDelete(d)
                                list = list - d
                            }) { Text("削除", color = MaterialTheme.colorScheme.error) }
                        }
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("閉じる") } },
    )
}

/** 地図を読み込んだ事にするURL。APIキーの「ウェブサイトの制限」にはこれを登録する */
const val MAP_BASE_URL = "https://m5f9p.azukimap.jp/"
