(function () {
  const root = document.getElementById('chaos-recipes-editor');
  if (!root) return;
  const T = (s, p) => window.MuText.t(s, p);
  const LANGS = ['Eng', 'Spn', 'Por'];
  const LANG_NAMES = { Eng: 'Inglés', Spn: 'Español', Por: 'Portugués' };

  const state = { recipes: [], items: [], limits: {}, open: new Set() };
  const esc = (s) => String(s ?? '').replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const bytes = (s) => new TextEncoder().encode(String(s || '')).length;

  function setMessage(text, type) {
    const box = root.querySelector('.editor-message');
    if (!box) return;
    box.textContent = text || '';
    box.classList.toggle('hidden', !text);
    box.classList.toggle('success', type === 'success');
    box.classList.toggle('error', type === 'error');
  }

  function itemName(section, index) {
    const item = state.items.find((i) => i.section === section && i.index === index);
    return item ? item.name : null;
  }

  function itemLabel(section, index) {
    const name = itemName(section, index);
    return `${section},${index}${name ? ` — ${name}` : ''}`;
  }

  // "13,2 — Uniria" o "13 2" -> [13, 2]
  function parseItem(text) {
    const m = String(text || '').match(/^\s*(\d+)\s*[, ]\s*(\d+)/);
    return m ? [Number(m[1]), Number(m[2])] : null;
  }

  function nextId() {
    const used = new Set(state.recipes.map((r) => r.id));
    for (let id = state.limits.firstId; id <= state.limits.lastId; id++) if (!used.has(id)) return id;
    return state.limits.firstId;
  }

  function newRecipe() {
    return {
      id: nextId(),
      name: { Eng: 'New Mix', Spn: '', Por: '' },
      help: { Eng: [], Spn: [], Por: [] },
      rate: 50,
      money: 0,
      ingredients: [{ section: 12, index: 15, count: 1, minLevel: 0, maxLevel: 15 }],
      result: { section: 14, index: 13, level: 0 }
    };
  }

  function counter(text) {
    const n = bytes(text);
    const over = n > state.limits.textBytes;
    return `<small class="${over ? 'chaos-recipe-over' : 'muted'}" data-counter>${n}/${state.limits.textBytes}</small>`;
  }

  function itemInput(path, section, index) {
    const unknown = itemName(section, index) === null;
    return `<input type="text" list="chaos-recipe-items" data-item="${path}" value="${esc(itemLabel(section, index))}" style="min-width:16em">`
      + (unknown ? ` <small class="chaos-recipe-over">${esc(T('No está en Item.txt'))}</small>` : '');
  }

  function recipeCard(r, n) {
    const open = state.open.has(n);
    const total = r.ingredients.reduce((s, i) => s + (Number(i.count) || 0), 0);
    const names = LANGS.map((l) => `
      <label class="field">${esc(T(LANG_NAMES[l]))}
        <input type="text" data-r="${n}" data-name="${l}" value="${esc(r.name[l])}"
          placeholder="${l === 'Eng' ? '' : esc(T('(usa el inglés)'))}"> ${counter(r.name[l])}
      </label>`).join('');
    const helpRows = [];
    for (let line = 0; line < state.limits.helpLines; line++) {
      helpRows.push(`<tr><td>${line + 1}</td>${LANGS.map((l) => `
        <td><input type="text" data-r="${n}" data-help="${l}" data-line="${line}" value="${esc(r.help[l][line] || '')}"> ${counter(r.help[l][line] || '')}</td>`).join('')}</tr>`);
    }
    const ingRows = r.ingredients.map((ing, k) => `
      <tr>
        <td>${itemInput(`${n}.${k}`, ing.section, ing.index)}</td>
        <td><input type="number" min="1" max="32" data-r="${n}" data-ing="${k}" data-field="count" value="${ing.count}" style="width:5em"></td>
        <td><input type="number" min="0" max="15" data-r="${n}" data-ing="${k}" data-field="minLevel" value="${ing.minLevel}" style="width:4em">
          – <input type="number" min="0" max="15" data-r="${n}" data-ing="${k}" data-field="maxLevel" value="${ing.maxLevel}" style="width:4em"></td>
        <td><button type="button" class="link-button" data-action="del-ing" data-r="${n}" data-ing="${k}">✕</button></td>
      </tr>`).join('');
    const body = !open ? '' : `
      <div class="custom-form-grid">
        <label class="field">${esc(T('Número'))}
          <input type="number" min="${state.limits.firstId}" max="${state.limits.lastId}" data-r="${n}" data-field="id" value="${r.id}"></label>
        <label class="field">${esc(T('Éxito (%)'))}
          <input type="number" min="0" max="100" data-r="${n}" data-field="rate" value="${r.rate}"></label>
        <label class="field">${esc(T('Costo (zen)'))}
          <input type="number" min="0" max="2000000000" data-r="${n}" data-field="money" value="${r.money}"></label>
      </div>
      <h4>${esc(T('Nombre en el panel del Chaos'))}</h4>
      <div class="custom-form-grid">${names}</div>
      <h4>${esc(T('Ayuda debajo del panel'))}</h4>
      <p class="muted">${esc(T('Un idioma sin ninguna línea muestra la ayuda en inglés.'))}</p>
      <table class="table">
        <thead><tr><th>#</th>${LANGS.map((l) => `<th>${esc(T(LANG_NAMES[l]))}</th>`).join('')}</tr></thead>
        <tbody>${helpRows.join('')}</tbody>
      </table>
      <h4>${esc(T('Ingredientes'))} <small class="muted">(${total}/32)</small></h4>
      <p class="muted">${esc(T('El chaos box tiene que tener exactamente estos items, ni más ni menos. El nivel es el +N del item.'))}</p>
      <table class="table">
        <thead><tr><th>${esc(T('Item'))}</th><th>${esc(T('Cantidad'))}</th><th>${esc(T('Nivel (+N)'))}</th><th></th></tr></thead>
        <tbody>${ingRows}</tbody>
      </table>
      ${r.ingredients.length < state.limits.maxIngredients
        ? `<button type="button" data-action="add-ing" data-r="${n}">${esc(T('Agregar ingrediente'))}</button>` : ''}
      <h4>${esc(T('Resultado'))}</h4>
      <div class="custom-form-grid">
        <label class="field">${esc(T('Item'))} ${itemInput(`${n}.result`, r.result.section, r.result.index)}</label>
        <label class="field">${esc(T('Nivel (+N)'))}
          <input type="number" min="0" max="15" data-r="${n}" data-result="level" value="${r.result.level}"></label>
      </div>
      <div class="spawn-actions">
        <button type="button" class="btn-danger" data-action="del-recipe" data-r="${n}">${esc(T('Eliminar receta'))}</button>
      </div>`;
    return `
      <div class="card-sub">
        <div class="row-between">
          <h3>#${r.id} ${esc(r.name.Eng)} <small class="muted">— ${r.rate}% · ${Number(r.money).toLocaleString()} zen → ${esc(itemLabel(r.result.section, r.result.index))}</small></h3>
          <button type="button" data-action="toggle" data-r="${n}">${esc(T(open ? 'Cerrar' : 'Editar'))}</button>
        </div>
        ${body}
      </div>`;
  }

  function render() {
    const options = state.items.map((i) => `<option value="${esc(`${i.section},${i.index} — ${i.name}`)}"></option>`).join('');
    root.innerHTML = `
      <div class="editor-message alert hidden"></div>
      <datalist id="chaos-recipe-items">${options}</datalist>
      ${state.recipes.length ? state.recipes.map(recipeCard).join('') : `<p class="muted">${esc(T('Todavía no hay recetas agregadas.'))}</p>`}
      <div class="spawn-actions">
        <button type="button" data-action="add-recipe">${esc(T('Nueva receta'))}</button>
        <button type="button" data-action="save">${esc(T('Guardar'))}</button>
      </div>`;
  }

  function onField(el) {
    const r = state.recipes[Number(el.dataset.r)];
    if (el.dataset.item) {
      const [n, part] = el.dataset.item.split('.');
      const target = state.recipes[Number(n)];
      const key = parseItem(el.value);
      if (!key) return false;
      const dest = part === 'result' ? target.result : target.ingredients[Number(part)];
      [dest.section, dest.index] = key;
      return true;
    }
    if (!r) return false;
    if (el.dataset.name) r.name[el.dataset.name] = el.value;
    else if (el.dataset.help) {
      const lines = r.help[el.dataset.help];
      lines[Number(el.dataset.line)] = el.value;
    } else if (el.dataset.ing !== undefined) r.ingredients[Number(el.dataset.ing)][el.dataset.field] = Number(el.value);
    else if (el.dataset.result) r.result[el.dataset.result] = Number(el.value);
    else if (el.dataset.field) r[el.dataset.field] = Number(el.value);
    return false;
  }

  root.addEventListener('input', (ev) => {
    const el = ev.target;
    if (el.dataset.name !== undefined || el.dataset.help !== undefined) {
      onField(el);
      const c = el.parentElement.querySelector('[data-counter]');
      if (c) c.outerHTML = counter(el.value);
    }
  });

  root.addEventListener('change', (ev) => {
    // El item se re-dibuja para mostrar su nombre (o el aviso si no existe).
    if (onField(ev.target)) render();
  });

  // Lo que se manda: las líneas de ayuda sin los huecos del formulario.
  function payload() {
    return state.recipes.map((r) => ({
      ...r,
      help: Object.fromEntries(LANGS.map((l) => [l, r.help[l].map((s) => String(s || '').trim()).filter(Boolean)]))
    }));
  }

  async function save() {
    try {
      const res = await fetch('/admin/server-editor/api/chaos-recipes', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ recipes: payload() })
      });
      const data = await res.json();
      if (!res.ok) throw new Error(data.error || T('No se pudo guardar'));
      const reload = await fetch('/admin/server-editor/api/reload', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ target: 'chaosmix' })
      });
      setMessage(T(reload.ok ? 'Guardado y recargado en el servidor.' : 'Guardado, pero no se pudo recargar en el server.'),
        reload.ok ? 'success' : 'error');
    } catch (err) {
      setMessage(err.message, 'error');
    }
  }

  root.addEventListener('click', (ev) => {
    const el = ev.target.closest('[data-action]');
    if (!el) return;
    const n = Number(el.dataset.r);
    switch (el.dataset.action) {
      case 'toggle':
        if (state.open.has(n)) state.open.delete(n); else state.open.add(n);
        break;
      case 'add-recipe':
        state.recipes.push(newRecipe());
        state.open.add(state.recipes.length - 1);
        break;
      case 'del-recipe':
        if (!window.confirm(T('¿Eliminar esta receta?'))) return;
        state.recipes.splice(n, 1);
        state.open = new Set([...state.open].filter((i) => i !== n).map((i) => (i > n ? i - 1 : i)));
        break;
      case 'add-ing':
        state.recipes[n].ingredients.push({ section: 14, index: 13, count: 1, minLevel: 0, maxLevel: 15 });
        break;
      case 'del-ing':
        state.recipes[n].ingredients.splice(Number(el.dataset.ing), 1);
        break;
      case 'save':
        save();
        return;
      default:
        return;
    }
    render();
  });

  async function init() {
    root.innerHTML = '<div class="editor-message alert hidden"></div>';
    try {
      const res = await fetch('/admin/server-editor/api/chaos-recipes');
      const data = await res.json();
      if (!res.ok) throw new Error(data.error || T('No se pudo cargar'));
      state.recipes = data.recipes;
      state.items = data.items;
      state.limits = data.limits;
      render();
    } catch (err) {
      setMessage(err.message, 'error');
    }
  }

  init();
})();
