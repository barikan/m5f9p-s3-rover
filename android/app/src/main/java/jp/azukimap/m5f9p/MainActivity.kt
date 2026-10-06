package jp.azukimap.m5f9p

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import java.util.Locale

private val BLE_PERMISSIONS = arrayOf(
    Manifest.permission.BLUETOOTH_SCAN,
    Manifest.permission.BLUETOOTH_CONNECT,
)

class MainActivity : ComponentActivity() {

    private val viewModel: MainViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            // 端末のダークテーマの設定に合わせる（ステータスバーの文字色もこれに従う）
            MaterialTheme(colorScheme = if (isSystemInDarkTheme()) darkColorScheme() else lightColorScheme()) {
                App(viewModel, hasPermissions = ::hasPermissions)
            }
        }
    }

    private fun hasPermissions() = BLE_PERMISSIONS.all {
        ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun App(viewModel: MainViewModel, hasPermissions: () -> Boolean) {
    var granted by remember { mutableStateOf(hasPermissions()) }
    val launcher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result -> granted = result.values.all { it } }

    val state by viewModel.ble.state.collectAsStateWithLifecycle()
    val deviceName by viewModel.ble.deviceName.collectAsStateWithLifecycle()
    val message by viewModel.message.collectAsStateWithLifecycle()
    val snackbar = remember { SnackbarHostState() }
    var tab by rememberSaveable { mutableStateOf(0) }     // 0:状況 1:地図

    LaunchedEffect(granted) {
        if (granted) viewModel.connectLast() else launcher.launch(BLE_PERMISSIONS)
    }
    LaunchedEffect(message) {
        message?.let {
            snackbar.showSnackbar(it)
            viewModel.message.value = null
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(if (state == ConnState.DISCONNECTED) "M5F9P Rover" else deviceName) },
                actions = {
                    if (state != ConnState.DISCONNECTED) {
                        TextButton(onClick = viewModel::disconnect) { Text("切断") }
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
        bottomBar = {
            if (granted) {
                NavigationBar {
                    NavigationBarItem(
                        selected = tab == 0, onClick = { tab = 0 },
                        icon = {}, label = { Text("状況", style = MaterialTheme.typography.titleSmall) },
                    )
                    NavigationBarItem(
                        selected = tab == 1, onClick = { tab = 1 },
                        icon = {}, label = { Text("地図", style = MaterialTheme.typography.titleSmall) },
                    )
                }
            }
        },
    ) { padding ->
        Column(
            Modifier
                .padding(padding)
                .fillMaxSize()
        ) {
            when {
                !granted -> PermissionScreen { launcher.launch(BLE_PERMISSIONS) }
                tab == 1 -> MapScreen(viewModel)
                state == ConnState.DISCONNECTED -> ScanScreen(viewModel)
                else -> StatusScreen(viewModel, connecting = state == ConnState.CONNECTING)
            }
        }
    }
}

@Composable
private fun PermissionScreen(onRequest: () -> Unit) {
    Column(Modifier.padding(24.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Text("本体と通信するために、「付近のデバイス」の権限が必要です。")
        Button(onClick = onRequest) { Text("権限を許可する") }
        Text(
            "許可のダイアログが出ない場合は、Androidの設定からこのアプリの権限を変更してください。",
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

@Composable
private fun ScanScreen(viewModel: MainViewModel) {
    val scanning by viewModel.ble.scanning.collectAsStateWithLifecycle()
    val devices by viewModel.ble.devices.collectAsStateWithLifecycle()

    LaunchedEffect(Unit) { viewModel.ble.startScan() }

    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("接続する本体を選んでください", Modifier.weight(1f))
            if (scanning) CircularProgressIndicator(Modifier.width(24.dp), strokeWidth = 3.dp)
            else OutlinedButton(onClick = viewModel.ble::startScan) { Text("再スキャン") }
        }
        if (!viewModel.ble.isBluetoothEnabled) {
            Text("Bluetoothがオフになっています。", color = MaterialTheme.colorScheme.error)
        }
        if (devices.isEmpty() && !scanning) {
            Text("本体が見つかりません。電源が入っているか確認してください。")
        }
        for (device in devices) {
            Card(
                Modifier
                    .fillMaxWidth()
                    .clickable { viewModel.connect(device.address) }
            ) {
                Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(device.name, style = MaterialTheme.typography.titleMedium)
                        Text(device.address, style = MaterialTheme.typography.bodySmall)
                    }
                    Text("${device.rssi} dBm", style = MaterialTheme.typography.bodySmall)
                }
            }
        }
    }
}

@Composable
private fun StatusScreen(viewModel: MainViewModel, connecting: Boolean) {
    val status by viewModel.status.collectAsStateWithLifecycle()
    val throughput by viewModel.throughput.collectAsStateWithLifecycle()
    val s = status

    if (s == null) {
        Row(
            Modifier.padding(24.dp),
            horizontalArrangement = Arrangement.spacedBy(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            CircularProgressIndicator(Modifier.width(24.dp), strokeWidth = 3.dp)
            Text(if (connecting) "接続しています…" else "本体からの状況を待っています…")
        }
        return
    }

    Column(
        Modifier
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        PositionCard(s)
        CorrectionCard(s, throughput)
        ControlCard(s, onSave = viewModel::setSaving, onRate = viewModel::setRate)
        DeviceCard(s)
    }
}

@Composable
private fun Section(title: String, content: @Composable () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(title, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary)
            content()
        }
    }
}

@Composable
private fun Item(label: String, value: String, mono: Boolean = false) {
    Row {
        Text(label, Modifier.width(112.dp), color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, fontFamily = if (mono) FontFamily.Monospace else null)
    }
}

@Composable
private fun PositionCard(s: RoverStatus) {
    Section("測位") {
        Card(colors = CardDefaults.cardColors(containerColor = fixColor(s.quality))) {
            Text(
                s.quality.label,
                Modifier.padding(horizontal = 12.dp, vertical = 4.dp),
                color = Color.White,
                fontWeight = FontWeight.Bold,
            )
        }
        if (s.posValid) {
            Item("緯度", String.format(Locale.US, "%.9f°", s.lat), mono = true)
            Item("経度", String.format(Locale.US, "%.9f°", s.lon), mono = true)
            Item("楕円体高", String.format(Locale.US, "%.3f m", s.height), mono = true)
        } else {
            Text("測位データがありません")
        }
        Item("衛星数", "${s.sats}")
        Item("測位レート", "${s.rateHz} Hz")
    }
}

@Composable
private fun CorrectionCard(s: RoverStatus, throughput: Throughput) {
    Section("補正データ") {
        when {
            !s.baseValid -> Item("取得先", "なし")
            s.baseIsUart -> Item("取得先", "UART（PHコネクタ）")
            else -> Item("取得先", "${s.baseAddress} / ${s.baseMount}")
        }
        if (s.baseValid) {
            Item("状態", if (s.baseReady) "受信中" else "接続待ち")
            Item("受信量", "${throughput.baseBytesPerSec} バイト/秒")
            Item("エラー率", "${s.rtcmErrPercent} %")
            Item("遅れ", String.format(Locale.US, "%.1f 秒", s.rtcmAgeMs / 1000.0))
            Item("再接続", "${s.baseReconnects} 回")
        }
        if (s.clasBytes >= 0) Item("CLAS", "${throughput.clasBytesPerSec} バイト/秒")
    }
}

@Composable
private fun ControlCard(s: RoverStatus, onSave: (Boolean) -> Unit, onRate: (Int) -> Unit) {
    Section("操作") {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(if (s.saving) "ログを保存中（${s.saveFormatName}）" else "ログ保存は停止中")
                Text(
                    if (s.saveReady) "書き込み ${s.saveCount} 回" else "SDカードが使えません",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            if (s.saving) OutlinedButton(onClick = { onSave(false) }) { Text("停止") }
            else Button(onClick = { onSave(true) }, enabled = s.saveReady) { Text("保存開始") }
        }
        Spacer(Modifier.padding(top = 4.dp))
        Text("測位レート")
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            for (hz in listOf(1, 2, 5, 10)) {
                if (hz == s.rateHz) Button(onClick = {}) { Text("$hz Hz") }
                else FilledTonalButton(onClick = { onRate(hz) }) { Text("$hz Hz") }
            }
        }
    }
}

@Composable
private fun DeviceCard(s: RoverStatus) {
    Section("本体") {
        if (s.wifiSsid.isEmpty()) {
            Item("Wi-Fi", "使用しない")
        } else if (s.wifiConnected) {
            Item("Wi-Fi", "${s.wifiSsid}（${s.wifiRssi} dBm）")
            Item("IPアドレス", s.wifiIp)
        } else {
            Item("Wi-Fi", "${s.wifiSsid}（接続中）")
        }
        Item("SDカード", "${s.sdMB} MB")
        Item("バージョン", s.version)
        Item("稼働時間", "${s.uptimeSec / 60} 分 ${s.uptimeSec % 60} 秒")
    }
}
