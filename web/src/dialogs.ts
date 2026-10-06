// 確認と入力のダイアログを、関数として呼べるようにする。
// 実際の表示は components/DialogHost.vue が行う。

import { shallowRef } from 'vue';
import type { FormField, FormValues } from './types';

interface ConfirmRequest {
  kind: 'confirm';
  title: string;
  text: string;
  okLabel: string;
}

interface FormRequest {
  kind: 'form';
  title: string;
  fields: FormField[];
  values: Partial<FormValues>;
}

type Request = (ConfirmRequest | FormRequest) & { resolve: (result: unknown) => void };

/** 表示中のダイアログ。{kind:'confirm'|'form', ..., resolve} */
export const current = shallowRef<Request | null>(null);

function open<T>(request: ConfirmRequest | FormRequest) {
  return new Promise<T | null>(resolve => {
    if (current.value) current.value.resolve(null);     // 前のものは、やめた事にする
    current.value = { ...request, resolve: resolve as (result: unknown) => void };
  });
}

/** ダイアログを閉じて結果を返す（DialogHost から呼ぶ） */
export function finish(result: boolean | FormValues | null) {
  const request = current.value;
  current.value = null;
  if (request) request.resolve(result);
}

/** 確認のダイアログを出す。戻り値＝実行する時 true */
export const confirmDialog = async (title: string, text: string, okLabel = '実行') =>
  !!await open<boolean>({ kind: 'confirm', title, text, okLabel });

/**
 * 入力のダイアログを出す。
 *
 * values: 初期値。key毎
 *
 * 戻り値＝入力された値（key毎）。やめた時は null
 */
export const formDialog = (title: string, fields: FormField[], values: Partial<FormValues> = {}) =>
  open<FormValues>({ kind: 'form', title, fields, values });
