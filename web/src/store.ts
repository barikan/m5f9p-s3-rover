// 画面(Vue)と rover.ts の橋渡し。
//
// rover.ts は Vue に依存せず、状態(rover.state)を書き換えては
// subscribe() で知らせてくる。ここでは、その内容を画面用の state に写す。
// 画面の部品は、この state を読むだけで描き直される。

import { shallowReactive } from 'vue';
import { toast as sonner } from 'vue-sonner';
import * as rover from './rover';

// 中身（status や track）は rover.ts が持つものをそのまま指す。深くは監視しない
export const state = shallowReactive({
  ...rover.state,
  trackCount: 0,        // 軌跡は同じ配列に足していくので、点数を別に持つ
});

/** 画面の下に短い文言を出す（表示は App.vue に置いた Sonner） */
export const toast = (text: string) => sonner(text);

rover.subscribe(what => {
  if (what === 'message') {
    toast(rover.state.message);
    return;
  }
  Object.assign(state, rover.state);
  state.trackCount = rover.state.track.length;
});
