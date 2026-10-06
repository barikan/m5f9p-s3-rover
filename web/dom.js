// DOMを組み立てる小さな道具。

/**
 * 要素を作る。
 *   h('div', {class:'row', onclick: fn}, '文字', 子要素, [子要素の配列])
 * attrs の on〜 はイベント、値が false / null / undefined の属性は付けない。
 */
export function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (value === false || value === null || value === undefined) continue;
    if (key.startsWith('on')) el.addEventListener(key.slice(2), value);
    else if (key === 'value') el.value = value;
    else if (key === 'checked') el.checked = value;
    else if (value === true) el.setAttribute(key, '');
    else el.setAttribute(key, value);
  }
  for (const child of children.flat(Infinity)) {
    if (child === null || child === undefined || child === false) continue;
    el.append(child instanceof Node ? child : String(child));
  }
  return el;
}

export function clear(el) {
  el.replaceChildren();
  return el;
}

let toastTimer = null;

/** 画面の下に短い文言を出す */
export function toast(text) {
  const el = document.getElementById('toast');
  el.textContent = text;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 4000);
}

/**
 * 入力のダイアログを出す。
 *
 * fields: [{key, label, type('text'|'password'|'number'|'bool'|'select'), options, help, required}]
 * values: 初期値。key毎
 *
 * 戻り値＝入力された値（key毎）。やめた時は null
 */
export function formDialog(title, fields, values = {}) {
  return new Promise(resolve => {
    const inputs = {};
    const body = fields.map(f => {
      const value = values[f.key] ?? f.default ?? '';
      if (f.type === 'bool') {
        inputs[f.key] = h('input', { type: 'checkbox', checked: !!value });
        return h('label', { class: 'check' }, inputs[f.key], f.label);
      }
      if (f.type === 'select') {
        inputs[f.key] = h('select', {}, f.options.map(o => h('option', { value: o.value }, o.label)));
        inputs[f.key].value = value;
      } else {
        inputs[f.key] = h('input', {
          type: f.type === 'number' ? 'number' : 'text', value, required: f.required,
          autocomplete: 'off', autocapitalize: 'off', spellcheck: 'false',
        });
      }
      return h('label', { class: 'field' }, h('span', {}, f.label), inputs[f.key],
        f.help ? h('div', { class: 'small' }, f.help) : null);
    });

    const dialog = h('dialog', {},
      h('form', { method: 'dialog' },
        h('h2', {}, title), body,
        h('div', { class: 'row', style: 'justify-content:flex-end;margin-top:16px' },
          h('button', { type: 'button', class: 'text', onclick: () => dialog.close('cancel') }, 'やめる'),
          h('button', { class: 'primary', value: 'ok' }, 'OK'))));
    dialog.addEventListener('close', () => {
      let result = null;
      if (dialog.returnValue === 'ok') {
        result = {};
        for (const f of fields) {
          const input = inputs[f.key];
          result[f.key] = f.type === 'bool' ? input.checked
            : f.type === 'number' ? (input.value === '' ? (f.default ?? 0) : Number(input.value))
              : input.value.trim();
        }
      }
      dialog.remove();
      resolve(result);
    });
    document.body.append(dialog);
    dialog.showModal();
  });
}

/** 確認のダイアログを出す。戻り値＝実行する時 true */
export function confirmDialog(title, text, okLabel = '実行') {
  return new Promise(resolve => {
    const dialog = h('dialog', {},
      h('form', { method: 'dialog' },
        h('h2', {}, title), h('p', {}, text),
        h('div', { class: 'row', style: 'justify-content:flex-end' },
          h('button', { type: 'button', class: 'text', onclick: () => dialog.close('cancel') }, 'やめる'),
          h('button', { class: 'primary', value: 'ok' }, okLabel))));
    dialog.addEventListener('close', () => {
      dialog.remove();
      resolve(dialog.returnValue === 'ok');
    });
    document.body.append(dialog);
    dialog.showModal();
  });
}
