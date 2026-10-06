// 地図。Google Maps（Maps JavaScript API）に、現在地と選んだ日の軌跡を表示する。
// 地図の上に重ねる表示とボタンは components/MapPage.vue にある。
//
// APIキーは、設定タブで入力したもの（localStorage）を優先し、無ければ本体の設定ファイルの
// ものを使う。キーが決まった時に、Maps JavaScript API を読み込む。

import { shallowReactive } from 'vue';
import * as rover from './rover';
import type { Change, TrackPoint } from './types';

const state = rover.state;

export const userMapsKey = () => localStorage.getItem('mapsKey') || '';
export const mapsKey = () => userMapsKey() || state.deviceMapsKey;

/**
 * 地図を用意する。mapEl は地図を入れる要素。
 * 戻り値の view は、画面に出す状態（追従中か、航空写真か、キーが使えないか）
 */
export function createMap(mapEl: HTMLElement) {
  const view = shallowReactive({ follow: true, satellite: false, authFailed: false });

  let map: google.maps.Map | null = null;
  let marker: google.maps.Marker | null = null;
  let lines: google.maps.Polyline[] = [];           // 測位の状態が変わる毎に線を分ける（色を変えるため）
  let lastLine: google.maps.Polyline | null = null;
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
        map, strokeColor: rover.qualityOf(p.q).color, strokeWeight: 4, strokeOpacity: 1, clickable: false,
      });
      if (previous) lastLine.getPath().push(previous);
      lines.push(lastLine);
      lastQuality = p.q;
    }
    lastLine.getPath().push(position);
  }

  /** 軌跡。増えた分だけ足し、日が変わった時や減った時は描き直す */
  function drawTrack() {
    if (!map) return;
    if (drawnDay !== state.trackDay || state.track.length < drawn) {
      for (const line of lines) line.setMap(null);
      lines = [];
      lastLine = null;
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
      fillColor: rover.qualityOf(state.status.pos.quality).color, fillOpacity: 1,
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
    toggleSatellite() {
      view.satellite = !view.satellite;
      if (map) map.setMapTypeId(view.satellite ? 'hybrid' : 'roadmap');
    },
    /** rover の状態が変わった時に呼ぶ */
    update(what: Change) {
      if (what === 'mapsKey') {
        // 別のキーで読み込み済みの時は、画面を読み込み直さないと切り替えられない
        if (loadedKey && mapsKey() && loadedKey !== mapsKey()) location.reload();
        if (visible) load();
      }
      if (what === 'track') drawTrack();
      if (what === 'status' || what === 'conn') drawCurrent();
    },
  };
}
