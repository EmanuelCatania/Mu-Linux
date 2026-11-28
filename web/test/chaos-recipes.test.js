const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const recipes = require('../lib/chaos-recipes');

const REPO_FILE = path.join(__dirname, '..', '..', 'server', 'MuServer', 'Data', 'ChaosMix', 'Recipes.toml');
const read = (file) => fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n');

test('chaos-recipes: el Recipes.toml del repo es el que arma el panel', () => {
  const text = read(REPO_FILE);
  const { header, recipes: list } = recipes.parse(text);
  assert.equal(list.length, 1);
  assert.equal(list[0].id, 20);
  assert.deepEqual(list[0].result, { section: 13, index: 400, level: 0 });
  assert.equal(recipes.build(list, header), text);
});

test('chaos-recipes: sin archivo no hay recetas y queda el encabezado por defecto', () => {
  const { header, recipes: list } = recipes.parse('');
  assert.deepEqual(list, []);
  assert.equal(header, recipes.DEFAULT_HEADER);
  assert.equal(recipes.build([], header), recipes.DEFAULT_HEADER);
});

test('chaos-recipes: lo que manda el panel se guarda y se vuelve a leer igual', () => {
  const sent = [{
    id: 21,
    name: { Eng: 'Test Mix', Spn: 'Mezcla de prueba', Por: '' },
    help: { Eng: ['Two souls'], Spn: ['Dos almas'], Por: [] },
    rate: 50,
    money: 0,
    ingredients: [{ section: 14, index: 14, count: 2, minLevel: 0, maxLevel: 15 }, { section: 0, index: 1, count: 1, minLevel: 9, maxLevel: 15 }],
    result: { section: 14, index: 16, level: 0 }
  }];
  const list = recipes.validate({ recipes: sent });
  // Portugués sin nombre usa el inglés; sin ayuda queda vacío (no copia el inglés).
  assert.equal(list[0].name.Por, 'Test Mix');
  assert.deepEqual(list[0].help.Por, []);
  const text = recipes.build(list);
  assert.match(text, /name = \{ Eng = "Test Mix", Spn = "Mezcla de prueba", Por = "Test Mix" \}/);
  assert.match(text, /\{ item = \[14, 14\], count = 2 \}/);
  assert.match(text, /\{ item = \[0, 1\], level = \[9, 15\] \}/);
  assert.doesNotMatch(text, /money/);
  assert.deepEqual(recipes.parse(text).recipes, list);
});

test('chaos-recipes: las mismas reglas que el server', () => {
  const base = () => ({
    id: 30, name: { Eng: 'A', Spn: '', Por: '' }, help: { Eng: [], Spn: [], Por: [] }, rate: 10, money: 0,
    ingredients: [{ section: 14, index: 13, count: 1, minLevel: 0, maxLevel: 15 }], result: { section: 14, index: 14, level: 0 }
  });
  const bad = (change, pattern) => {
    const r = base();
    change(r);
    assert.throws(() => recipes.validate({ recipes: [r] }), pattern);
  };
  bad((r) => { r.id = 11; }, /'id'/);
  bad((r) => { r.rate = 101; }, /'rate'/);
  bad((r) => { r.name.Eng = ''; }, /inglés/);
  bad((r) => { r.name.Eng = 'á'.repeat(24); }, /47 bytes/);
  bad((r) => { r.help.Eng = ['a', 'b', 'c', 'd']; }, /hasta 3/);
  bad((r) => { r.ingredients = []; }, /1 a 8/);
  bad((r) => { r.ingredients[0].count = 33; }, /'count'/);
  bad((r) => { r.ingredients[0].count = 20; r.ingredients.push({ ...r.ingredients[0] }); }, /32 lugares/);
  bad((r) => { r.ingredients[0].minLevel = 9; r.ingredients[0].maxLevel = 3; }, /mínimo, máximo/);
  bad((r) => { r.result.level = 16; }, /'level'/);
  assert.throws(() => recipes.validate({ recipes: [base(), base()] }), /dos veces/);
  assert.throws(() => recipes.parse('[recipes]\n'), /sección desconocida/);
});
