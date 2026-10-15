// 地図。Google Maps（Maps JavaScript API）に、現在地と選んだ日の軌跡を表示する。
// 地図の上に重ねる表示とボタンは components/MapPage.vue にある。
//
// APIキーは、設定タブで入力してこの端末に保存したもの（localStorage）だけを使う（本体には置かない）。
// キーがある時に、Maps JavaScript API を読み込む。キーを変えた時は、設定タブが画面を読み込み直す。

import { shallowReactive } from 'vue';
import * as rover from './rover';
import type { Change, TrackPoint } from './types';

const state = rover.state;

export const userMapsKey = () => localStorage.getItem('mapsKey') || '';

// 地図の配色の彩度（-100〜100）。軌跡と現在地の色が見分けやすいよう、地図だけを淡くする
// （CSS の filter は軌跡と現在地にもかかってしまうので使わない）
const MAP_SATURATION = -90;

/** 凡例（MapPage.vue）に並べる測位の状態。色は rover.ts の QUALITY（バッジと同じ） */
export const TRACK_LEGEND = [4, 5, 2, 1, 6];
const trackColorOf = (q: number) => rover.qualityOf(q).color;

/** 軌跡を表示するか（端末ごとに覚える） */
function loadShowTrack() {
  try { return localStorage.getItem('showTrack') !== '0'; } catch (e) { return true; }
}
function saveShowTrack(on: boolean) {
  try { localStorage.setItem('showTrack', on ? '1' : '0'); } catch (e) { /* 覚えられない時は、次回は既定（表示） */ }
}
export const mapsKey = userMapsKey;

/**
 * 地図を用意する。mapEl は地図を入れる要素。
 * 戻り値の view は、画面に出す状態（追従中か、航空写真か、軌跡を表示しているか、キーが使えないか）
 */
export function createMap(mapEl: HTMLElement) {
  const view = shallowReactive({ follow: true, satellite: false, track: loadShowTrack(), authFailed: false });

  let map: google.maps.Map | null = null;
  let marker: google.maps.Marker | null = null;
  let lines: google.maps.Polyline[] = [];           // 測位の状態が変わる毎に線を分ける（色を変えるため）。白い縁の線も入る
  let lastLine: google.maps.Polyline | null = null;
  let lastOutline: google.maps.Polyline | null = null;   // lastLine の後ろの白い縁
  let lastQuality = -1;
  let drawn = 0;            // 地図に描いた軌跡の点数
  let drawnDay = '';
  let loadedKey = '';       // 読み込みに使ったキー
  let visible = false;

  // ---------------------------------------------------------------- 読み込み

  function load() {
    const key = mapsKey();
    if (!key || loadedKey) return;
    loadedKey = key;
    window.gm_authFailure = () => { view.authFailed = true; };
    window.__mapReady = () => {
      const start = currentPosition() || lastTrackPosition();
      map = new google.maps.Map(mapEl, {
        center: start || { lat: 35.681236, lng: 139.767125 },
        zoom: start ? 19 : 5,
        mapTypeId: 'roadmap',
        disableDefaultUI: true,
        gestureHandling: 'greedy',
        tilt: 0,
        styles: [{ stylers: [{ saturation: MAP_SATURATION }] }],
      });
      // 地図を指で動かしたら追従をやめる
      map.addListener('dragstart', () => { view.follow = false; });
      marker = new google.maps.Marker({ map, visible: false, clickable: false, zIndex: 10 });
      drawTrack();
      drawCurrent();
    };
    const script = document.createElement('script');
    script.src = `https://maps.googleapis.com/maps/api/js?key=${encodeURIComponent(key)}&callback=__mapReady&loading=async&language=ja&region=JP`;
    script.async = true;
    document.head.append(script);
  }

  // ---------------------------------------------------------------- 描画

  const isToday = () => state.trackDay === rover.today();

  function currentPosition() {
    const s = state.status;
    return s && s.pos.valid && isToday() ? { lat: s.pos.lat, lng: s.pos.lon } : null;
  }

  function lastTrackPosition() {
    const p = state.track[state.track.length - 1];
    return p ? { lat: p.lat, lng: p.lon } : null;
  }

  function addPoint(p: TrackPoint) {
    const position = new google.maps.LatLng(p.lat, p.lon);
    if (!lastLine || p.q !== lastQuality) {
      // 線がつながるよう、前の線の最後の点から始める
      const path = lastLine ? lastLine.getPath() : null;
      const previous = path && path.getLength() ? path.getAt(path.getLength() - 1) : null;
      lastLine = new google.maps.Polyline({
        map: view.track ? map : null, strokeColor: trackColorOf(p.q), strokeWeight: 5, strokeOpacity: 1, clickable: false, zIndex: 2,
      });
      // 線の後ろに白い縁を引く（地図の上で見分けやすくする）
      lastOutline = new google.maps.Polyline({
        map: view.track ? map : null, strokeColor: '#FFFFFF', strokeWeight: 11, strokeOpacity: 1, clickable: false, zIndex: 1,
      });
      if (previous) {
        lastLine.getPath().push(previous);
        lastOutline.getPath().push(previous);
      }
      lines.push(lastOutline, lastLine);
      lastQuality = p.q;
    }
    lastLine.getPath().push(position);
    lastOutline!.getPath().push(position);
  }

  /** 軌跡。増えた分だけ足し、日が変わった時や減った時は描き直す */
  function drawTrack() {
    if (!map) return;
    if (drawnDay !== state.trackDay || state.track.length < drawn) {
      for (const line of lines) line.setMap(null);
      lines = [];
      lastLine = null;
      lastOutline = null;
      lastQuality = -1;
      drawn = 0;
      if (drawnDay && drawnDay !== state.trackDay && !isToday()) {
        // 過去の日を選んだ時は、その軌跡の最後の点へ移動する
        const last = lastTrackPosition();
        if (last) map.setCenter(last);
      }
      drawnDay = state.trackDay;
    }
    for (; drawn < state.track.length; drawn++) addPoint(state.track[drawn]);
  }

  function drawCurrent() {
    if (!map || !marker) return;
    const position = currentPosition();
    if (!position || !state.status) {
      marker.setVisible(false);
      return;
    }
    marker.setIcon({
      path: google.maps.SymbolPath.CIRCLE, scale: 8,
      fillColor: trackColorOf(state.status.pos.quality), fillOpacity: 1,
      strokeColor: '#FFFFFF', strokeWeight: 2,
    });
    marker.setPosition(position);
    marker.setVisible(true);
    if (view.follow && visible) {
      if ((map.getZoom() ?? 0) < 10) map.setZoom(19);
      map.panTo(position);
    }
  }

  return {
    view,
    /** 地図タブが表示された時、隠れた時に呼ぶ */
    setVisible(on: boolean) {
      visible = on;
      if (!on) return;
      load();
      drawCurrent();
    },
    /** 現在地を追いかける表示に戻す */
    followCurrent() {
      view.follow = true;
      drawCurrent();
    },
    /** 軌跡の表示を入・切する。切っている間も、線のデータは足していく */
    toggleTrack() {
      view.track = !view.track;
      saveShowTrack(view.track);
      for (const line of lines) line.setMap(view.track ? map : null);
    },
    toggleSatellite() {
      view.satellite = !view.satellite;
      if (map) map.setMapTypeId(view.satellite ? 'hybrid' : 'roadmap');
    },
    /** rover の状態が変わった時に呼ぶ */
    update(what: Change) {
      if (what === 'track') drawTrack();
      if (what === 'status' || what === 'conn') drawCurrent();
    },
  };
}
