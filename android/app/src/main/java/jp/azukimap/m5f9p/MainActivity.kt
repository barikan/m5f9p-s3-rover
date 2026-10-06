package jp.azukimap.m5f9p

import android.Manifest
import android.annotation.SuppressLint
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.webkit.JavascriptInterface
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.webkit.WebViewAssetLoader
import org.json.JSONArray
import org.json.JSONObject

/**
 * 画面。Windowsアプリと共用の web/ を、全画面のWebViewに表示する。
 *
 * web/ はアセットに取り込んであり、https://m5f9p.azukimap.jp/ から読み込んだ扱いにする
 * （実在のサイトではない）。Google MapsのAPIキーに「ウェブサイトの制限」をかける時に、
 * Windowsアプリと同じURLを登録できるようにするため。
 *
 * 画面とのやり取り
 *   画面 → アプリ: window.AndroidBridge（下の Bridge）
 *   アプリ → 画面: window.onNative({type:'devices'|'state'|'line', ...})
 * 画面側の受け口は web/src/host.ts。
 */
class MainActivity : ComponentActivity(), BleClient.Listener {

    private lateinit var webView: WebView
    private var started = false     // 画面が前面にある間 true。それ以外では画面に通知しない

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {}

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val assetLoader = WebViewAssetLoader.Builder()
            .setDomain(APP_HOST)
            .addPathHandler("/", WebViewAssetLoader.AssetsPathHandler(this))
            .build()

        webView = WebView(this).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true       // 画面がlocalStorageを使う（APIキー、前回の接続先）
            webViewClient = object : WebViewClient() {
                override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest): WebResourceResponse? =
                    if (request.url.host == APP_HOST) assetLoader.shouldInterceptRequest(request.url) else null

                // 画面の外へのリンクは開かない
                override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean =
                    request.url.host != APP_HOST
            }
            addJavascriptInterface(Bridge(), "AndroidBridge")
        }
        setContentView(webView)

        // ステータスバー、ナビゲーションバー、キーボードに重ならないようにする
        ViewCompat.setOnApplyWindowInsetsListener(webView) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.ime())
            view.updatePadding(left = bars.left, top = bars.top, right = bars.right, bottom = bars.bottom)
            WindowInsetsCompat.CONSUMED
        }

        if (!hasBlePermissions()) permissionLauncher.launch(REQUEST_PERMISSIONS)
        webView.loadUrl(Uri.Builder().scheme("https").authority(APP_HOST).path("/index.html").build().toString())
    }

    override fun onStart() {
        super.onStart()
        started = true
        Rover.ble.listener = this
        webView.onResume()
    }

    override fun onStop() {
        started = false
        webView.onPause()
        super.onStop()
    }

    override fun onDestroy() {
        if (Rover.ble.listener === this) Rover.ble.listener = null
        webView.destroy()
        super.onDestroy()
    }

    private fun hasBlePermissions() = BLE_PERMISSIONS.all {
        ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
    }

    // ---------------------------------------------------------------- アプリ → 画面

    private fun notifyPage(message: JSONObject) {
        if (started) webView.evaluateJavascript("window.onNative && window.onNative($message)", null)
    }

    override fun onState(state: String, name: String) =
        notifyPage(JSONObject().put("type", "state").put("state", state).put("name", name))

    override fun onDevices(devices: List<FoundDevice>) {
        val list = JSONArray()
        for (d in devices) {
            list.put(JSONObject().put("id", d.address).put("name", d.name).put("detail", "${d.rssi} dBm"))
        }
        notifyPage(JSONObject().put("type", "devices").put("list", list))
    }

    override fun onLine(line: String) = notifyPage(JSONObject().put("type", "line").put("text", line))

    override fun onMessage(text: String) = notifyPage(JSONObject().put("type", "message").put("text", text))

    // ---------------------------------------------------------------- 画面 → アプリ
    //
    // WebViewのスレッドから呼ばれる。BLEの操作はメインスレッドで行う。

    private inner class Bridge {
        @JavascriptInterface
        fun startScan() = runOnUiThread {
            if (hasBlePermissions()) Rover.ble.startScan() else permissionLauncher.launch(REQUEST_PERMISSIONS)
        }

        @JavascriptInterface
        fun stopScan() = runOnUiThread { Rover.ble.stopScan() }

        @JavascriptInterface
        fun connect(address: String) = runOnUiThread { if (hasBlePermissions()) Rover.connect(address) }

        @JavascriptInterface
        fun disconnect() = runOnUiThread { Rover.disconnect() }

        @JavascriptInterface
        fun sendLine(line: String) = Rover.ble.sendLine(line)

        /** いまの接続の状態。アプリを開き直した時に、画面が接続を引き継ぐのに使う */
        @JavascriptInterface
        fun getState(): String =
            JSONObject().put("state", Rover.ble.state).put("name", Rover.ble.deviceName).toString()

        @JavascriptInterface
        fun storageRead(name: String): String? = Rover.storageRead(name)

        @JavascriptInterface
        fun storageAppend(name: String, text: String) = Rover.storageAppend(name, text)

        @JavascriptInterface
        fun storageList(): String = JSONArray(Rover.storageList()).toString()

        @JavascriptInterface
        fun storageRemove(name: String) = Rover.storageRemove(name)
    }

    companion object {
        /** 画面を読み込んだ事にするホスト名。Windowsアプリ(windows/main.js)と同じにしている */
        private const val APP_HOST = "m5f9p.azukimap.jp"

        private val BLE_PERMISSIONS = arrayOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
        )

        // 通知は、接続中である事を示す常駐の通知に使う。拒否されても動作する
        private val REQUEST_PERMISSIONS =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) BLE_PERMISSIONS + Manifest.permission.POST_NOTIFICATIONS
            else BLE_PERMISSIONS
    }
}
