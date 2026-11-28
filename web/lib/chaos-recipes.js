// Recetas del Chaos agregadas: Data/ChaosMix/Recipes.toml.
//
// El panel las muestra y las guarda en un formulario. Las reglas son las mismas
// que aplica el server al leer el archivo (Common/ChaosRecipes.cpp): si el panel
// acepta una receta, el server también. Lo único que el panel no revisa es que
// los items existan en Item.txt; eso lo hace `mu-data validate` y el server al
// cargar (descarta la receta y lo avisa en el log).
//
// Al guardar se reescribe el archivo entero, conservando el bloque de
// comentarios del principio.

const TOML = require('smol-toml');

const RECIPES_FILE = 'Data/ChaosMix/Recipes.toml';
const FIRST_ID = 20;          // 1..11 son las recetas del 0.97k
const LAST_ID = 127;          // el cliente guarda el número en un byte con signo
const LANGUAGES = ['Eng', 'Spn', 'Por'];
const TEXT_BYTES = 47;        // nombre y cada línea de ayuda (más el 0 del cliente)
const HELP_LINES = 3;
const MAX_INGREDIENTS = 8;
const CHAOS_BOX_SLOTS = 32;
const MAX_MONEY = 2000000000;

const DEFAULT_HEADER = `# Recetas del Chaos agregadas.
#
# Las nueve recetas del 0.97k (Chaos Item, Devil Square, +10, +11, Dinorant,
# frutas, alas de 1.º y 2.º nivel, Blood Castle) siguen en el server; sus
# porcentajes y costos están en GameServerInfo - ChaosMix.dat. Acá se agregan
# recetas nuevas. Si el chaos box coincide con una del 0.97k, gana esa.
#
# Este archivo lo puede escribir el panel web: se conserva este bloque de
# comentarios y el resto se reescribe.

`;

class RecipeError extends Error {}

const intIn = (v, min, max) => typeof v === 'number' && Number.isInteger(v) && v >= min && v <= max;
const plain = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);
const sameLines = (a, b) => a.length === b.length && a.every((line, n) => line === b[n]);

function checkKeys(table, allowed, where) {
  for (const key of Object.keys(table)) {
    if (!allowed.includes(key)) throw new RecipeError(`${where}: campo desconocido '${key}'`);
  }
}

function checkText(text, where, what) {
  const bytes = Buffer.byteLength(text, 'utf8');
  if (bytes < 1 || bytes > TEXT_BYTES) {
    throw new RecipeError(`${where}: ${what} tiene que tener entre 1 y ${TEXT_BYTES} bytes (mide ${bytes}; una letra con acento ocupa 2)`);
  }
  if (/[\u0000-\u001f]/.test(text)) throw new RecipeError(`${where}: ${what} no puede llevar saltos de línea ni caracteres de control`);
  return text;
}

function readInt(table, key, min, max, def, where) {
  if (table[key] === undefined) return def;
  if (!intIn(table[key], min, max)) throw new RecipeError(`${where}: '${key}' tiene que ser un entero entre ${min} y ${max}`);
  return table[key];
}

function readPair(table, key, required, min, max, def, where) {
  const v = table[key];
  if (v === undefined) {
    if (required) throw new RecipeError(`${where}: falta '${key}'`);
    return def;
  }
  if (!Array.isArray(v) || v.length !== 2 || !v.every((x) => intIn(x, min, max))) {
    throw new RecipeError(`${where}: '${key}' tiene que ser [a, b] con enteros entre ${min} y ${max}`);
  }
  return [v[0], v[1]];
}

// "texto" o { Eng, Spn, Por }. Devuelve los tres idiomas ya completos.
function readName(node, where) {
  if (node === undefined) throw new RecipeError(`${where}: falta 'name'`);
  const out = {};
  if (typeof node === 'string') {
    for (const lang of LANGUAGES) out[lang] = node;
  } else if (plain(node)) {
    checkKeys(node, LANGUAGES, where);
    for (const lang of LANGUAGES) {
      if (node[lang] !== undefined && typeof node[lang] !== 'string') throw new RecipeError(`${where}: 'name.${lang}' tiene que ser un texto`);
      out[lang] = node[lang] || '';
    }
    if (!out.Eng) throw new RecipeError(`${where}: 'name' tiene que traer al menos el inglés (Eng)`);
  } else {
    throw new RecipeError(`${where}: 'name' es un texto o { Eng = "...", Spn = "...", Por = "..." }`);
  }
  for (const lang of LANGUAGES) out[lang] = checkText(out[lang] || out.Eng, where, 'el nombre');
  return out;
}

function readLines(node, where) {
  if (!Array.isArray(node) || node.length > HELP_LINES) throw new RecipeError(`${where}: 'help' es una lista de hasta ${HELP_LINES} líneas`);
  return node.map((line) => {
    if (typeof line !== 'string') throw new RecipeError(`${where}: cada línea de 'help' es un texto`);
    return checkText(line, where, 'cada línea de ayuda');
  });
}

// ["..."] o { Eng = [...], Spn = [...], Por = [...] }. Un idioma que falta
// usa el inglés; uno presente y vacío queda sin ayuda.
function readHelp(node, where) {
  const out = { Eng: [], Spn: [], Por: [] };
  if (node === undefined) return out;
  if (plain(node)) {
    checkKeys(node, LANGUAGES, where);
    for (const lang of LANGUAGES) if (node[lang] !== undefined) out[lang] = readLines(node[lang], where);
    for (const lang of LANGUAGES.slice(1)) if (node[lang] === undefined) out[lang] = [...out.Eng];
    return out;
  }
  out.Eng = readLines(node, where);
  out.Spn = [...out.Eng];
  out.Por = [...out.Eng];
  return out;
}

function readIngredient(node, where) {
  if (!plain(node)) throw new RecipeError(`${where}: cada ingrediente es { item = [sección, índice], count = N, level = [mín, máx] }`);
  checkKeys(node, ['item', 'count', 'level'], where);
  const [section, index] = readPair(node, 'item', true, 0, 511, null, where);
  const count = readInt(node, 'count', 1, CHAOS_BOX_SLOTS, 1, where);
  const [minLevel, maxLevel] = readPair(node, 'level', false, 0, 15, [0, 15], where);
  if (minLevel > maxLevel) throw new RecipeError(`${where}: 'level' va [mínimo, máximo]`);
  return { section, index, count, minLevel, maxLevel };
}

// Una receta ya leída del TOML (o mandada por el panel con la misma forma).
function readRecipe(table, where) {
  if (!plain(table)) throw new RecipeError(`${where}: cada receta es una tabla [[recipe]]`);
  checkKeys(table, ['id', 'name', 'help', 'rate', 'money', 'ingredients', 'result'], where);
  if (table.id === undefined) throw new RecipeError(`${where}: falta 'id' (${FIRST_ID} a ${LAST_ID})`);
  const id = readInt(table, 'id', FIRST_ID, LAST_ID, -1, where);
  const name = readName(table.name, where);
  const help = readHelp(table.help, where);
  if (table.rate === undefined) throw new RecipeError(`${where}: falta 'rate' (0 a 100)`);
  const rate = readInt(table, 'rate', 0, 100, -1, where);
  const money = readInt(table, 'money', 0, MAX_MONEY, 0, where);
  const list = table.ingredients;
  if (!Array.isArray(list) || list.length < 1 || list.length > MAX_INGREDIENTS) {
    throw new RecipeError(`${where}: 'ingredients' es una lista de 1 a ${MAX_INGREDIENTS} ingredientes`);
  }
  const ingredients = list.map((ing, n) => readIngredient(ing, `${where}: ingrediente ${n + 1}`));
  const total = ingredients.reduce((sum, ing) => sum + ing.count, 0);
  if (total > CHAOS_BOX_SLOTS) throw new RecipeError(`${where}: los ingredientes suman ${total} items y el chaos box tiene ${CHAOS_BOX_SLOTS} lugares`);
  if (!plain(table.result)) throw new RecipeError(`${where}: falta 'result' = { item = [sección, índice], level = N }`);
  checkKeys(table.result, ['item', 'level'], `${where}: result`);
  const [section, index] = readPair(table.result, 'item', true, 0, 511, null, `${where}: result`);
  const level = readInt(table.result, 'level', 0, 15, 0, `${where}: result`);
  return { id, name, help, rate, money, ingredients, result: { section, index, level } };
}

function checkUnique(recipes) {
  const seen = new Map();
  for (const r of recipes) {
    if (seen.has(r.id)) throw new RecipeError(`la receta ${r.id} está dos veces (${seen.get(r.id)} y ${r.where})`);
    seen.set(r.id, r.where);
  }
}

// Bloque de comentarios del principio (con la línea en blanco que lo cierra).
function header(content) {
  const all = String(content || '').replace(/\r\n/g, '\n').split('\n');
  const kept = [];
  for (const line of all) {
    if (!line.trimStart().startsWith('#') && line.trim() !== '') break;
    kept.push(line.trimEnd());
  }
  while (kept.length && kept[kept.length - 1] === '') kept.pop();
  return kept.length ? `${kept.join('\n')}\n\n` : DEFAULT_HEADER;
}

// Texto del archivo -> { header, recipes }. Sin archivo (o vacío) no hay recetas.
function parse(content) {
  let doc;
  try {
    doc = TOML.parse(String(content || ''));
  } catch (err) {
    throw new RecipeError(`Recipes.toml: ${err.message}`);
  }
  for (const key of Object.keys(doc)) {
    if (key !== 'recipe') throw new RecipeError(`Recipes.toml: sección desconocida '${key}' (vale [[recipe]])`);
  }
  if (doc.recipe !== undefined && !Array.isArray(doc.recipe)) throw new RecipeError("Recipes.toml: 'recipe' va como [[recipe]]");
  const recipes = (doc.recipe || []).map((table, n) => {
    const where = `receta ${n + 1}`;
    return { ...readRecipe(table, where), where };
  });
  checkUnique(recipes);
  return { header: header(content), recipes: recipes.map(({ where, ...r }) => r) };
}

// Lo que manda el panel al guardar: misma forma que devuelve parse(), pero
// los idiomas vacíos usan el inglés y las ayudas vacías se descartan.
function validate(body) {
  const list = body && Array.isArray(body.recipes) ? body.recipes : null;
  if (!list) throw new RecipeError('falta la lista de recetas');
  const recipes = list.map((r, n) => {
    const where = `receta ${n + 1}${r && r.id !== undefined ? ` (id ${r.id})` : ''}`;
    if (!plain(r)) throw new RecipeError(`${where}: no es una receta`);
    const name = plain(r.name) ? Object.fromEntries(LANGUAGES.map((l) => [l, String(r.name[l] || '').trim()])) : r.name;
    let help;
    if (plain(r.help)) {
      help = {};
      for (const l of LANGUAGES) help[l] = (Array.isArray(r.help[l]) ? r.help[l] : []).map((s) => String(s).trim()).filter(Boolean);
    }
    const ingredients = Array.isArray(r.ingredients) ? r.ingredients.map((ing) => (plain(ing) ? {
      item: [ing.section, ing.index], count: ing.count, level: [ing.minLevel, ing.maxLevel]
    } : ing)) : r.ingredients;
    const result = plain(r.result) ? { item: [r.result.section, r.result.index], level: r.result.level } : r.result;
    const table = { id: r.id, name, rate: r.rate, money: r.money, ingredients, result };
    if (help) table.help = help;
    return { ...readRecipe(table, where), where };
  });
  checkUnique(recipes);
  return recipes.map(({ where, ...r }) => r);
}

//**********************************************//
//******************* Escritura ****************//
//**********************************************//

const str = (text) => `"${String(text).replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;
const lines = (list) => `[${list.map(str).join(', ')}]`;

function nameText(name) {
  if (name.Spn === name.Eng && name.Por === name.Eng) return str(name.Eng);
  return `{ ${LANGUAGES.map((l) => `${l} = ${str(name[l])}`).join(', ')} }`;
}

// null si no hay ayuda en ningún idioma.
function helpText(help) {
  if (LANGUAGES.every((l) => !help[l].length)) return null;
  if (sameLines(help.Spn, help.Eng) && sameLines(help.Por, help.Eng)) return lines(help.Eng);
  const parts = [`Eng = ${lines(help.Eng)}`];
  for (const l of LANGUAGES.slice(1)) if (!sameLines(help[l], help.Eng)) parts.push(`${l} = ${lines(help[l])}`);
  return `{ ${parts.join(', ')} }`;
}

function ingredientText(ing) {
  let out = `{ item = [${ing.section}, ${ing.index}]`;
  if (ing.count !== 1) out += `, count = ${ing.count}`;
  if (ing.minLevel !== 0 || ing.maxLevel !== 15) out += `, level = [${ing.minLevel}, ${ing.maxLevel}]`;
  return `${out} }`;
}

function build(recipes, headerText = DEFAULT_HEADER) {
  let out = headerText;
  [...recipes].sort((a, b) => a.id - b.id).forEach((r, n) => {
    if (n > 0) out += '\n';
    out += '[[recipe]]\n';
    out += `id = ${r.id}\n`;
    out += `name = ${nameText(r.name)}\n`;
    const help = helpText(r.help);
    if (help) out += `help = ${help}\n`;
    out += `rate = ${r.rate}\n`;
    if (r.money) out += `money = ${r.money}\n`;
    out += 'ingredients = [\n';
    for (const ing of r.ingredients) out += `  ${ingredientText(ing)},\n`;
    out += ']\n';
    out += `result = { item = [${r.result.section}, ${r.result.index}]${r.result.level ? `, level = ${r.result.level}` : ''} }\n`;
  });
  return out;
}

module.exports = {
  RECIPES_FILE,
  FIRST_ID,
  LAST_ID,
  LANGUAGES,
  TEXT_BYTES,
  HELP_LINES,
  MAX_INGREDIENTS,
  DEFAULT_HEADER,
  RecipeError,
  parse,
  validate,
  build
};
