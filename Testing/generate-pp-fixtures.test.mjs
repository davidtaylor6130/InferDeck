import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm, rmdir } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { generateAllFixtures, generateScenario, SCENARIOS, writeFixtures } from './generate-pp-fixtures.mjs';
import { loadRequestFixture } from './measure-concurrency.mjs';

const MODEL = 'qwen3.8-27b';

const FROZEN_HASHES = {
  'cold-64k': '59af61cf08000ef71584d38b82afa8e2e05cd5d55e8f79b5451cb9f21f817071',
  'cold-100k': '9dcbd3fafca16e76e4c308ac8794c455f063eabd616b90fb028e63b5a7e21ad9',
  'append-64k-2k': '4c630bf2559b3987984620d9f0e7b8094e713ad9f53cdc080a89f71768ae83ec',
  'append-64k-4k': '558277414bee2139b419177a58162157c420a301ac15eac4f058702e4fab3f3a',
  'append-100k-2k': '0089f0dddb2c1d97c74cd1ee20ce10bdc2bdb468afa628687b6be1118b8c9714',
  'append-100k-4k': 'c58c83aeeeb2d0f53a83684debc112c06c960bf8c0897ff0d594105d26418c8b',
};

test('fixture set covers all required scenarios', () => {
  assert.deepEqual(
    SCENARIOS.map(scenario => scenario.name),
    [
      'cold-64k', 'cold-100k',
      'append-64k-2k', 'append-64k-4k',
      'append-100k-2k', 'append-100k-4k',
    ]
  );
});

test('generation is deterministic within one process', async () => {
  for (const scenario of SCENARIOS) {
    const first = await generateScenario(scenario);
    const second = await generateScenario(scenario);
    assert.equal(second.sha256, first.sha256, `${scenario.name} sha256`);
    assert.equal(second.bytes, first.bytes, `${scenario.name} bytes`);
    assert.deepEqual(second.body, first.body, `${scenario.name} body`);
  }
});

test('frozen fixture hashes match the committed freeze', async () => {
  const fixtures = await generateAllFixtures();
  for (const fixture of fixtures) {
    assert.equal(
      fixture.sha256,
      FROZEN_HASHES[fixture.scenario],
      `${fixture.scenario} hash drifted; review and refreeze deliberately`
    );
  }
});

test('estimated tokens land within ten percent of the target', async () => {
  const fixtures = await generateAllFixtures();
  for (const fixture of fixtures) {
    const target = fixture.tokenTarget ?? fixture.baseTokenTarget + fixture.appendTokenTarget;
    const deviation = Math.abs(fixture.estimatedTokens - target) / target * 100;
    assert.ok(deviation <= 10, `${fixture.scenario} deviation ${deviation.toFixed(1)}%`);
  }
});

test('generated fixtures satisfy harness validation', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'inferdeck-pp-fixtures-test-'));
  const manifest = await writeFixtures(directory);
  t.after(async () => {
    await rm(join(directory, 'manifest.json'), { force: true });
    for (const fixture of manifest.fixtures) {
      await rm(join(directory, `pp-${fixture.scenario}.json`), { force: true });
      if (fixture.prefixFile) await rm(join(directory, fixture.prefixFile), { force: true });
    }
    await rmdir(directory);
  });
  for (const fixture of manifest.fixtures) {
    const evidence = await loadRequestFixture(resolve(directory, `pp-${fixture.scenario}.json`), MODEL);
    assert.equal(evidence.evidence.sha256, fixture.sha256);
    assert.equal(evidence.body.model, MODEL);
    assert.equal(evidence.body.stream, false);
    assert.ok(evidence.body.max_tokens >= 1);
    assert.ok(Array.isArray(evidence.body.messages) && evidence.body.messages.length >= 2);
  }
});

test('on-disk fixtures match the manifest', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'inferdeck-pp-fixtures-test-'));
  const manifest = await writeFixtures(directory);
  t.after(async () => {
    await rm(join(directory, 'manifest.json'), { force: true });
    for (const fixture of manifest.fixtures) {
      await rm(join(directory, `pp-${fixture.scenario}.json`), { force: true });
      if (fixture.prefixFile) await rm(join(directory, fixture.prefixFile), { force: true });
    }
    await rmdir(directory);
  });
  for (const fixture of manifest.fixtures) {
    const raw = await readFile(join(directory, `pp-${fixture.scenario}.json`));
    assert.equal(raw.length, fixture.bytes, `${fixture.scenario} bytes`);
  }
});

test('append fixtures ship a prefix priming body holding every message but the final turn', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'inferdeck-pp-fixtures-test-'));
  const manifest = await writeFixtures(directory);
  t.after(async () => {
    await rm(join(directory, 'manifest.json'), { force: true });
    for (const fixture of manifest.fixtures) {
      await rm(join(directory, `pp-${fixture.scenario}.json`), { force: true });
      if (fixture.prefixFile) await rm(join(directory, fixture.prefixFile), { force: true });
    }
    await rmdir(directory);
  });
  for (const fixture of manifest.fixtures) {
    if (fixture.scenario.startsWith('cold-')) {
      assert.equal(fixture.prefixFile, undefined);
      continue;
    }
    assert.equal(fixture.prefixFile, `pp-${fixture.scenario}-prefix.json`);
    const raw = await readFile(join(directory, fixture.prefixFile));
    assert.equal(raw.length, fixture.prefixBytes);
    const parsed = JSON.parse(raw.toString('utf8'));
    assert.deepEqual(parsed.messages, fixture.body.messages.slice(0, -1));
    assert.equal(parsed.stream, false);
    assert.ok(parsed.max_tokens >= 1);
    // The priming prefix must end before the appended requirement so the
    // measured sample evaluates the delta, not a fully cached replay.
    assert.equal(fixture.body.messages.at(-1).role, 'user');
    assert.match(fixture.body.messages.at(-1).content, /appended requirement/);
  }
});

test('cold fixtures are single-turn; append fixtures include a tool continuation', async () => {
  const fixtures = await generateAllFixtures();
  for (const fixture of fixtures) {
    const roles = fixture.body.messages.map(message => message.role);
    if (fixture.scenario.startsWith('cold-')) {
      assert.deepEqual(roles, ['system', 'user']);
      assert.equal(fixture.baseTokenTarget, null);
    } else {
      assert.ok(roles.includes('tool'), `${fixture.scenario} tool turn`);
      assert.ok(roles.includes('assistant'), `${fixture.scenario} assistant turn`);
      assert.ok(fixture.baseTokenTarget === 64000 || fixture.baseTokenTarget === 100000);
      assert.ok(fixture.appendTokenTarget === 2000 || fixture.appendTokenTarget === 4000);
    }
  }
});
test('fixture metadata distinguishes full input depth from uncached append work', async () => {
  for (const fixture of await generateAllFixtures()) {
    assert.equal(fixture.promptTokenTarget, fixture.baseTokenTarget ? fixture.baseTokenTarget + fixture.appendTokenTarget : fixture.tokenTarget);
    assert.equal(fixture.cachedTokenTarget, fixture.baseTokenTarget ?? 0);
  }
});
