// 地図タブ。Google Maps（Maps JavaScript API）に、現在地と選んだ日の軌跡を表示する。
//
// APIキーは、設定タブで入力したもの（localStorage）を優先し、無ければ本体の設定ファイルの
// ものを使う。キーが決まった時に、Maps JavaScript API を読み込む。

import * as rover from './rover.js';
import { h, clear } from './dom.js';

const state = rover.state;

export const userMapsKey = () => localStorage.getItem('mapsKey') || '';
export const mapsKey = () => userMapsKey() || state.deviceMapsKey;

export function initMap(page) {
  const mapEl = page.querySelector('#map');
  const infoEl = page.querySelector('#map-info');
  const buttonsEl = page.querySelector('#map-buttons');

  let map = null;           // google.maps.Map
  let marker = null;
  let lines = [];           // 測位の状態が変わる毎に線を分ける（色を変えるため）
  let lastLine = null;
  let lastQuality = -1;
  let drawn = 0;            // 地図に描いた軌跡の点数
  let drawnDay = '';
  let follow = true;
  let satellite = false;
  let loadedKey = '';       // 読み込みに使ったキー
  let authFailed = false;
  let visible = false;

  // ---------------------------------------------------------------- 読み込み

  function load() {
    const key = mapsKey();
    if (!key || loadedKey) return;
    loadedKey = key;
    window.gm_authFailure = () => { authFailed = true; renderInfo(); };
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
      map.addListener('dragstart', () => { follow = false; renderButtons(); });
      marker = new google.maps.Marker({ map, visible: false, clickable: false, zIndex: 10 });
      drawTrack();
      drawCurrent();
    };
    document.head.append(h('script', {
      src: `https://maps.googleapis.com/maps/api/js?key=${encodeURIComponent(key)}&callback=__mapReady&loading=async&language=ja&region=JP`,
      async: true,
    }));
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

  function addPoint(p) {
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
    if (!map) return;
    const position = currentPosition();
    if (!position) {
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
    if (follow && visible) {
      if (map.getZoom() < 10) map.setZoom(19);
      map.panTo(position);
    }
  }

  function renderInfo() {
    clear(infoEl);
    const s = state.status;
    if (!mapsKey()) {
      infoEl.append(
        h('div', {}, 'Google Maps の API キーが設定されていません。'),
        h('div', { class: 'small' }, '「設定」タブでキーを入力するか、本体の設定の Google Maps のキーに書いてください。'));
    } else if (authFailed) {
      infoEl.append(
        h('div', { class: 'error' }, 'API キーが使えません'),
        h('div', { class: 'small' }, 'キーが正しいか、Maps JavaScript API が有効かを確認してください。'));
    }
    if (s && isToday()) {
      const q = rover.qualityOf(s.pos.quality);
      infoEl.append(h('div', { style: `color:${q.color};font-weight:700` }, q.label));
      if (s.pos.valid) infoEl.append(h('div', { class: 'small mono' }, `${s.pos.lat.toFixed(8)}, ${s.pos.lon.toFixed(8)}`));
    } else if (isToday()) {
      infoEl.append(h('div', {}, '本体に接続していません'));
    }
    infoEl.append(h('div', { class: 'small' },
      `${isToday() ? '今日' : state.trackDay}の軌跡 ${state.track.length} 点${state.syncing ? '（本体から取得中）' : ''}`));
  }

  function renderButtons() {
    clear(buttonsEl).append(
      !follow && currentPosition() ? h('button', { onclick: () => { follow = true; drawCurrent(); renderButtons(); } }, '現在地') : null,
      h('button', {
        onclick: () => {
          satellite = !satellite;
          if (map) map.setMapTypeId(satellite ? 'hybrid' : 'roadmap');
          renderButtons();
        },
      }, satellite ? '地図' : '航空写真'),
      h('button', { onclick: showDays }, '履歴'));
  }

  async function showDays() {
    const days = await rover.trackDays();
    const today = rover.today();
    const all = [today, ...days.filter(d => d !== today)];
    const dialog = h('dialog', {},
      h('h2', {}, '表示する日'),
      h('div', { class: 'list' }, all.map(day => h('div', { class: 'entry' },
        h('div', { class: 'name' }, (day === today ? '今日' : day) + (day === state.trackDay ? '（表示中）' : '')),
        h('button', { onclick: () => { rover.selectTrackDay(day); dialog.close(); } }, '表示'),
        days.includes(day)
          ? h('button', { class: 'danger', onclick: async () => { await rover.deleteTrackDay(day); dialog.close(); } }, '削除')
          : null))),
      h('div', { class: 'row', style: 'justify-content:flex-end' },
        h('button', { class: 'text', onclick: () => dialog.close() }, '閉じる')));
    dialog.addEventListener('close', () => dialog.remove());
    document.body.append(dialog);
    dialog.showModal();
  }

  renderInfo();
  renderButtons();

  return {
    /** 地図タブが表示された時に呼ぶ */
    shown() {
      visible = true;
      load();
      drawCurrent();
      renderInfo();
    },
    /** rover の状態が変わった時に呼ぶ */
    update(what) {
      if (what === 'mapsKey') {
        // 別のキーで読み込み済みの時は、画面を読み込み直さないと切り替えられない
        if (loadedKey && mapsKey() && loadedKey !== mapsKey()) location.reload();
        if (visible) load();
      }
      if (what === 'track') drawTrack();
      if (what === 'status' || what === 'conn') {
        drawCurrent();
        renderButtons();
      }
      renderInfo();
    },
  };
}
