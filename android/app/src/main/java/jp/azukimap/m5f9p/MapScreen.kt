package jp.azukimap.m5f9p

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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.google.android.gms.maps.CameraUpdateFactory
import com.google.android.gms.maps.model.CameraPosition
import com.google.android.gms.maps.model.LatLng
import com.google.maps.android.compose.Circle
import com.google.maps.android.compose.GoogleMap
import com.google.maps.android.compose.MapProperties
import com.google.maps.android.compose.MapType
import com.google.maps.android.compose.MapUiSettings
import com.google.maps.android.compose.Polyline
import com.google.maps.android.compose.rememberCameraPositionState
import java.time.LocalDate
import java.util.Locale
import kotlin.math.cos
import kotlin.math.pow

private const val CURRENT_MARK_RADIUS_PX = 7       // 現在地の円の半径。地図の拡大率1段あたりの基準ピクセルで、実際の画面ではこの2～3倍になる

fun fixColor(quality: FixQuality) = when (quality) {
    FixQuality.FIX -> Color(0xFF2E7D32)
    FixQuality.FLOAT -> Color(0xFFEF6C00)
    FixQuality.NONE -> Color(0xFFC62828)
    else -> Color(0xFF546E7A)
}

/** 測位の状態が同じ点の並びごとに線を分ける。色を変えて描くため。線がつながるよう、境目の点は両方に入れる */
private fun segmentsOf(points: List<TrackPoint>): List<Pair<FixQuality, List<LatLng>>> {
    val segments = mutableListOf<Pair<FixQuality, MutableList<LatLng>>>()
    for (p in points) {
        val position = LatLng(p.lat, p.lon)
        val last = segments.lastOrNull()
        if (last != null && last.first == p.quality) {
            last.second.add(position)
        } else {
            last?.second?.add(position)
            segments.add(p.quality to mutableListOf(position))
        }
    }
    return segments
}

@Composable
fun MapScreen(viewModel: MainViewModel) {
    val status by viewModel.status.collectAsStateWithLifecycle()
    val track by viewModel.track.collectAsStateWithLifecycle()
    val day by viewModel.trackDay.collectAsStateWithLifecycle()

    var follow by rememberSaveable { mutableStateOf(true) }
    var satellite by rememberSaveable { mutableStateOf(false) }
    var showDays by remember { mutableStateOf(false) }

    val isToday = day == LocalDate.now()
    val current = status?.takeIf { it.posValid && isToday }?.let { LatLng(it.lat, it.lon) }
    val start = current ?: track.lastOrNull()?.let { LatLng(it.lat, it.lon) }

    val camera = rememberCameraPositionState {
        position = CameraPosition.fromLatLngZoom(start ?: LatLng(35.681236, 139.767125), if (start != null) 19f else 5f)
    }

    // 現在地に追従する。地図を指で動かしたら追従をやめる
    LaunchedEffect(camera.isMoving) {
        if (camera.isMoving && camera.cameraMoveStartedReason == com.google.maps.android.compose.CameraMoveStartedReason.GESTURE) {
            follow = false
        }
    }
    LaunchedEffect(current, follow) {
        if (follow && current != null) {
            val zoom = if (camera.position.zoom < 10f) 19f else camera.position.zoom
            camera.animate(CameraUpdateFactory.newLatLngZoom(current, zoom), 300)
        }
    }
    // 過去の日を選んだ時は、その軌跡の最後の点へ移動する
    LaunchedEffect(day) {
        if (!isToday) {
            track.lastOrNull()?.let { camera.move(CameraUpdateFactory.newLatLngZoom(LatLng(it.lat, it.lon), 18f)) }
        }
    }

    Box(Modifier.fillMaxSize()) {
        GoogleMap(
            modifier = Modifier.fillMaxSize(),
            cameraPositionState = camera,
            properties = MapProperties(mapType = if (satellite) MapType.HYBRID else MapType.NORMAL),
            uiSettings = MapUiSettings(zoomControlsEnabled = false, mapToolbarEnabled = false),
        ) {
            for ((quality, positions) in segmentsOf(track)) {
                Polyline(points = positions, color = fixColor(quality), width = 8f)
            }
            if (current != null) {
                val quality = status?.quality ?: FixQuality.NONE
                // Circleの半径はm単位なので、拡大率が変わっても画面上で同じ大きさになるように求める
                val metersPerPixel = 156543.03 * cos(Math.toRadians(current.latitude)) / 2.0.pow(camera.position.zoom.toDouble())
                Circle(
                    center = current,
                    radius = CURRENT_MARK_RADIUS_PX * metersPerPixel,
                    fillColor = fixColor(quality),
                    strokeColor = Color.White,
                    strokeWidth = 3f,
                    zIndex = 1f,
                )
            }
        }

        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Surface(shape = MaterialTheme.shapes.medium, tonalElevation = 2.dp, shadowElevation = 2.dp) {
                Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
                    val s = status
                    if (s != null && isToday) {
                        Text(s.quality.label, color = fixColor(s.quality), style = MaterialTheme.typography.titleMedium)
                        if (s.posValid) {
                            Text(
                                String.format(Locale.US, "%.8f, %.8f", s.lat, s.lon),
                                style = MaterialTheme.typography.bodySmall,
                            )
                        }
                    } else if (isToday) {
                        Text("本体に接続していません", style = MaterialTheme.typography.bodyMedium)
                    }
                    Text(
                        "${if (isToday) "今日" else day.toString()}の軌跡 ${track.size} 点",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        }

        Row(
            Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .padding(12.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
        ) {
            FilledTonalButton(onClick = { follow = true }, enabled = !follow && current != null) { Text("現在地") }
            FilledTonalButton(onClick = { satellite = !satellite }) { Text(if (satellite) "地図" else "航空写真") }
            FilledTonalButton(onClick = { showDays = true }) { Text("履歴") }
        }
    }

    if (showDays) {
        DayDialog(
            days = viewModel.trackDays(),
            selected = day,
            onSelect = {
                viewModel.selectTrackDay(it)
                showDays = false
            },
            onDelete = viewModel::deleteTrackDay,
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
