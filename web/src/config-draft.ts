// 設定タブで本体の設定(設定ファイルの内容)を編集する仕組み。
//
//   ・useConfigDraft(paths): グループの下書き。選択と入・切を変えると下書きに溜め、「保存」で
//     変えた項目だけを config.put で送る(本体は深くマージする。src/config.cpp の configMerge)。
//   ・editGroupDialog(def): 自由入力の項目を formDialog で編集し、OK でそのまま送る。
//   ・afterConfigSave(): 保存のあとの共通処理(再起動が要る時は確認して再起動する)。
//
// 本体との通信は rover.ts(saveConfig)に任せる。ここは画面の都合(Vue)だけ。

import { computed, reactive, watch } from 'vue';
import * as rover from './rover';
import { state, toast } from './store';
import { confirmDialog, formDialog } from './dialogs';
import { CLEAR_LABEL, CSV_COLUMNS, CSV_COLUMNS_PATH, KEEP_HELP, KEPT_GROUPS, csvColumnLabel, csvColumnsOf, getPath, pathId, setPath, withNote } from './config-fields';
import type { GroupDef } from './config-fields';
import type { ConfigPath, ConfigValue, DeviceConfig, FormField, FormValues } from './types';

/**
 * 保存のあとの共通処理。
 * 再起動が要る項目を変えた時だけ、本体の再起動を求める(本体はファイルを書いただけで、
 * 再起動するまで config.get / config.put を受け付けない。rover.state.configLocked)
 */
export async function afterConfigSave(restartNeeded: boolean) {
  if (!restartNeeded) {
    toast('設定を保存しました');
    return;
  }
  if (await confirmDialog('本体を再起動しますか？',
    '設定を保存しました。変更した項目は、本体を再起動すると反映されます。再起動の間、測位と記録が数秒止まります。', '再起動')) rover.restart();
  else toast('変更は、次に本体を再起動した時に反映されます');
}

const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));

/**
 * グループの下書き。paths の項目だけを持つ。
 * 本体の設定(state.config)が読み直された時は、まだ変えていない項目だけ写し直す
 * (ダイアログの保存で読み直しても、変えた途中の値は消さない)
 */
export function useConfigDraft(paths: ConfigPath[]) {
  const draft = reactive<DeviceConfig>({});
  const touched = reactive(new Set<string>());

  function copyFromDevice() {
    for (const p of paths) {
      if (touched.has(pathId(p))) continue;
      const v = getPath(state.config, p);
      if (v !== undefined) setPath(draft, p, v);
    }
  }
  watch(() => state.config, copyFromDevice, { immediate: true, deep: true });

  const dirty = computed(() => paths.some(p => touched.has(pathId(p)) && getPath(draft, p) !== getPath(state.config, p)));
  const saving = reactive({ on: false });

  function get(path: ConfigPath) {
    return getPath(draft, path);
  }
  function set(path: ConfigPath, value: ConfigValue) {
    setPath(draft, path, value);
    touched.add(pathId(path));
  }

  /** 変えた項目だけを本体に送る。失敗した時は下書きを残す */
  async function save() {
    const partial: DeviceConfig = {};
    for (const p of paths) if (touched.has(pathId(p))) setPath(partial, p, getPath(draft, p) as ConfigValue);
    saving.on = true;
    try {
      const restartNeeded = await rover.saveConfig(partial);
      touched.clear();
      await afterConfigSave(restartNeeded);
    } catch (e) {
      toast(`保存できません: ${errorText(e)}`);
    } finally {
      saving.on = false;
    }
  }

  return { get, set, dirty, saving, save };
}

/**
 * 自由入力のグループを formDialog で編集して、OK でそのまま本体に送る。
 * パスワードの節(rtk2go、mqtt)は、設定済みなら欄を空にして「消す」の入・切を足す
 * (空のままなら送らず、本体が元の値を保つ)。失敗した時は、入力した値のまま開き直す
 */
export async function editGroupDialog(def: GroupDef) {
  const config = state.config;
  if (!config) return;
  const fields: FormField[] = [];
  const initial: FormValues = {};
  const keptGroups = new Set<string>();
  for (const f of def.fields) {
    const key = pathId(f.path);
    const group = f.path[0];
    const keeps = f.path.length === 2 && f.path[1] === 'password' && (KEPT_GROUPS as readonly string[]).includes(group)
      && !!(config[group] as Record<string, ConfigValue> | undefined)?.hasPassword;
    if (keeps) {
      keptGroups.add(group);
      fields.push({ ...withNote(f), key, help: KEEP_HELP }, { key: `${key}.clear`, label: CLEAR_LABEL, type: 'bool' });
      continue;
    }
    fields.push({ ...withNote(f), key });
    const v = getPath(config, f.path);
    if (v !== undefined) initial[key] = v;
  }

  let values: FormValues | null = initial;
  for (;;) {
    values = await formDialog(def.title, fields, values);
    if (!values) return;
    const partial: DeviceConfig = {};
    for (const f of def.fields) {
      const key = pathId(f.path);
      if (keptGroups.has(f.path[0]) && f.path[1] === 'password') {
        // 空のままなら送らない(本体が保つ)。「消す」なら空を送る
        if (values[`${key}.clear`]) setPath(partial, f.path, '');
        else if (values[key] !== '' && values[key] !== undefined) setPath(partial, f.path, values[key]);
        continue;
      }
      if (values[key] !== undefined) setPath(partial, f.path, values[key]);
    }
    try {
      const restartNeeded = await rover.saveConfig(partial);
      await afterConfigSave(restartNeeded);
      return;
    } catch (e) {
      toast(`保存できません: ${errorText(e)}`);
      // 入力した値を失わないよう、同じ値で開き直す(PEM の貼り付けなど)
    }
  }
}

/**
 * カスタム CSV（保存形式「CSV（カスタム）」）で出す項目を選ぶ。項目ごとの入・切のダイアログで、
 * OK で log.columns（コンマ区切り。順は本体の表の順）を本体に送る
 */
export async function editCsvColumnsDialog() {
  const config = state.config;
  if (!config) return;
  const selected = csvColumnsOf(getPath(config, CSV_COLUMNS_PATH));
  const fields: FormField[] = CSV_COLUMNS.map(c => ({ key: c.key, label: csvColumnLabel(c), type: 'bool' as const }));
  const initial: FormValues = Object.fromEntries(CSV_COLUMNS.map(c => [c.key, selected.has(c.key)]));
  let values: FormValues | null = initial;
  for (;;) {
    values = await formDialog('CSV (カスタム) で出す項目', fields, values);
    if (!values) return;
    const columns = CSV_COLUMNS.filter(c => values![c.key] === true).map(c => c.key).join(',');
    if (!columns) {
      toast('項目を 1 つ以上選んでください');
      continue;
    }
    const partial: DeviceConfig = {};
    setPath(partial, CSV_COLUMNS_PATH, columns);
    try {
      const restartNeeded = await rover.saveConfig(partial);
      await afterConfigSave(restartNeeded);
      return;
    } catch (e) {
      toast(`保存できません: ${errorText(e)}`);
    }
  }
}
