package jp.azukimap.m5f9p

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle

private val SAVE_FORMATS = listOf("NMEA", "RAW", "RTCM", "CSV")

/**
 * 設定。
 *   ・Google MapsのAPIキー（アプリに保存）
 *   ・本体の起動時の設定（Wi-Fi、補正データの取得先、保存形式）。書き換えると本体は再起動する
 *   ・本体のINIファイルの編集
 */
@Composable
fun SettingsScreen(connected: Boolean) {
    var editingIni by rememberSaveable { mutableStateOf(false) }

    if (editingIni) {
        IniEditor(onClose = { editingIni = false })
        return
    }

    Column(
        Modifier
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        MapsKeyCard()
        if (connected) {
            RunConfigCard()
            DeviceCard(onEditIni = {
                Rover.loadIni()
                editingIni = true
            })
        } else {
            Text("本体の設定は、本体に接続している時に変更できます。", style = MaterialTheme.typography.bodyMedium)
        }
    }
}

@Composable
private fun SettingsSection(title: String, content: @Composable () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(title, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary)
            content()
        }
    }
}

@Composable
private fun MapsKeyCard() {
    val userKey by Rover.userMapsKey.collectAsStateWithLifecycle()
    val deviceKey by Rover.deviceMapsKey.collectAsStateWithLifecycle()
    var text by rememberSaveable(userKey) { mutableStateOf(userKey) }

    SettingsSection("Google Maps の API キー") {
        OutlinedTextField(
            value = text,
            onValueChange = { text = it },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            label = { Text("API キー") },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            Button(onClick = { Rover.setUserMapsKey(text) }, enabled = text.trim() != userKey) { Text("保存") }
            if (userKey.isNotEmpty()) {
                TextButton(onClick = { Rover.setUserMapsKey("") }) { Text("消去") }
            }
        }
        Text(
            when {
                userKey.isNotEmpty() -> "ここに保存したキーを使います。"
                deviceKey.isNotEmpty() -> "未入力のため、本体の INI ファイル（[google] key）のキーを使います。"
                else -> "未入力です。本体の INI ファイルにもキーがありません。地図は表示されません。"
            },
            style = MaterialTheme.typography.bodySmall,
        )
        Text(
            "Maps JavaScript API を有効にしたキーが必要です。キーに「ウェブサイトの制限」をかける場合は " +
                "${MAP_BASE_URL}* を登録してください。",
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

/** 一覧から1つ選ぶ。labels[i]の値がvalues[i] */
@Composable
private fun Choice(label: String, labels: List<String>, values: List<Int>, selected: Int, onSelect: (Int) -> Unit) {
    var open by remember { mutableStateOf(false) }
    val index = values.indexOf(selected)
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.weight(1f))
        Box {
            OutlinedButton(onClick = { open = true }) { Text(labels.getOrElse(index) { "（不明）" }) }
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                labels.forEachIndexed { i, text ->
                    DropdownMenuItem(text = { Text(text) }, onClick = {
                        onSelect(values[i])
                        open = false
                    })
                }
            }
        }
    }
}

@Composable
private fun RunConfigCard() {
    val config by Rover.runConfig.collectAsStateWithLifecycle()
    LaunchedEffect(Unit) { Rover.loadRunConfig() }

    SettingsSection("本体の起動時の設定") {
        val c = config
        if (c == null) {
            Text("本体から読み込んでいます…")
            return@SettingsSection
        }
        var wifi by remember(c) { mutableIntStateOf(c.wifi) }
        var source by remember(c) { mutableIntStateOf(c.source) }
        var format by remember(c) { mutableIntStateOf(c.format) }
        var saveAtBoot by remember(c) { mutableStateOf(c.saveAtBoot) }

        Choice(
            "Wi-Fi",
            labels = listOf("使わない") + c.wifiList.map { it.ifEmpty { "（名称なし）" } },
            values = (0..c.wifiList.size).toList(),
            selected = wifi, onSelect = { wifi = it },
        )
        Choice(
            "補正データ",
            labels = listOf("なし") + c.sourceList.mapIndexed { i, s -> if (i == 0) "UART（PHコネクタ）" else s },
            values = listOf(-1) + c.sourceList.indices.toList(),
            selected = source, onSelect = { source = it },
        )
        Choice("保存形式", labels = SAVE_FORMATS, values = SAVE_FORMATS.indices.toList(), selected = format, onSelect = { format = it })
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("起動時からログを保存する", Modifier.weight(1f))
            Switch(checked = saveAtBoot, onCheckedChange = { saveAtBoot = it })
        }

        val changed = wifi != c.wifi || source != c.source || format != c.format || saveAtBoot != c.saveAtBoot
        Button(onClick = { Rover.applyRunConfig(wifi, source, format, saveAtBoot) }, enabled = changed) {
            Text("保存して本体を再起動")
        }
        Text(
            "Wi-Fi や補正データの接続先そのものを追加・変更するには、INI ファイルを編集します。",
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

@Composable
private fun DeviceCard(onEditIni: () -> Unit) {
    var confirm by remember { mutableStateOf<String?>(null) }    // 確認中の操作

    SettingsSection("本体") {
        OutlinedButton(onClick = onEditIni, modifier = Modifier.fillMaxWidth()) { Text("INI ファイルを編集") }
        OutlinedButton(onClick = { confirm = "restart" }, modifier = Modifier.fillMaxWidth()) { Text("本体を再起動") }
        OutlinedButton(onClick = { confirm = "setup" }, modifier = Modifier.fillMaxWidth()) {
            Text("本体の画面で設定をやり直す")
        }
    }

    confirm?.let { action ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = { Text(if (action == "setup") "設定をやり直しますか？" else "本体を再起動しますか？") },
            text = {
                Text(
                    if (action == "setup") "本体が再起動し、本体の画面に設定の選択が表示されます。選び終わるまで測位は始まりません。"
                    else "再起動の間、測位と記録が数秒止まります。"
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    if (action == "setup") Rover.requestSetup() else Rover.restart()
                    confirm = null
                }) { Text("実行") }
            },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text("やめる") } },
        )
    }
}

@Composable
private fun IniEditor(onClose: () -> Unit) {
    val loaded by Rover.iniText.collectAsStateWithLifecycle()
    var text by rememberSaveable { mutableStateOf<String?>(null) }

    // 本体から届いた内容を、編集用に1回だけ取り込む
    LaunchedEffect(loaded) {
        if (text == null && loaded != null) text = loaded
    }

    Column(Modifier.fillMaxSize().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("INI ファイル", Modifier.weight(1f), style = MaterialTheme.typography.titleMedium)
            TextButton(onClick = onClose) { Text("やめる") }
            Button(
                onClick = {
                    text?.let { Rover.saveIni(it) }
                    onClose()
                },
                enabled = text != null && text != loaded,
            ) { Text("保存して再起動") }
        }
        val current = text
        if (current == null) {
            Text("本体から読み込んでいます…")
        } else {
            OutlinedTextField(
                value = current,
                onValueChange = { text = it },
                modifier = Modifier.fillMaxWidth().weight(1f),
                textStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace, fontSize = 12.sp),
            )
        }
        Text(
            "; から行末まではコメントです。保存すると本体が再起動します。",
            style = MaterialTheme.typography.bodySmall,
        )
    }
}
