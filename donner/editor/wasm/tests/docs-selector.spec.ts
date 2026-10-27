import {test, expect} from '@playwright/test';
import fs from 'node:fs';

const files: Record<string, string> = JSON.parse(fs.readFileSync(process.env.DONNER_DOCS_FIXTURE!, 'utf8')).files;

test.beforeEach(async ({page}) => {
  await page.route('http://localhost/donner/**', async route => {
    const key = new URL(route.request().url()).pathname.replace('/donner/', '');
    const body = files[key];
    const contentType = key.endsWith('.js') ? 'application/javascript' : key.endsWith('.css') ? 'text/css'
      : key.endsWith('.json') ? 'application/json' : 'text/html';
    await route.fulfill({status: body === undefined ? 404 : 200, contentType, body: body ?? 'Missing'});
  });
});

test('switches the same nested page and preserves a valid section', async ({page}) => {
  await page.goto('http://localhost/donner/main/api/type.html#member');
  const selector = page.getByLabel('Documentation version');
  await expect(selector).toBeEnabled();
  await selector.selectOption('0.8.0');
  await expect(page).toHaveURL('http://localhost/donner/0.8.0/api/type.html#member');
  await expect(selector).toHaveValue('0.8.0');
});

test('missing pages fall back to the target index with a dismissible notice', async ({page}) => {
  await page.goto('http://localhost/donner/main/main-only.html');
  await page.getByLabel('Documentation version').selectOption('0.8.0');
  await expect(page).toHaveURL(/\/0\.8\.0\/index.html\?docs-missing=main-only.html$/);
  await expect(page.getByRole('status')).toContainText('main-only.html');
  await page.getByRole('button', {name: 'Dismiss'}).click();
  await expect(page.getByRole('status')).toHaveCount(0);
});

test('missing sections do not leave a stale fragment', async ({page}) => {
  await page.goto('http://localhost/donner/main/api/type.html#new-section');
  await page.getByLabel('Documentation version').selectOption('0.8.0');
  await expect(page).toHaveURL('http://localhost/donner/0.8.0/api/type.html?docs-anchor-missing=1');
  await expect(page.getByRole('status')).toContainText('does not contain the section');
});

test('legacy root pages select the stable channel and switch into main', async ({page}) => {
  await page.goto('http://localhost/donner/api/type.html#member');
  await expect(page.getByLabel('Documentation version')).toHaveValue('0.8.0');
  await page.getByLabel('Documentation version').selectOption('main');
  await expect(page).toHaveURL('http://localhost/donner/main/api/type.html#member');
});

test('failed metadata keeps usable index navigation', async ({page}) => {
  await page.route('**/_docs/versions.json', route => route.fulfill({status: 503, body: 'Unavailable'}));
  await page.goto('http://localhost/donner/main/index.html');
  await expect(page.getByLabel('Documentation version')).toBeDisabled();
  await expect(page.locator('.docs-version-links').getByRole('link', {name: '0.8.0', exact: true})).toHaveAttribute('href', '/donner/0.8.0/index.html');
});

test('changed page inventories fall back safely', async ({page}) => {
  await page.route('**/0.8.0/_docs/pages.json', route => route.fulfill({contentType: 'application/json', body: '{"schema":1,"pages":{}}'}));
  await page.goto('http://localhost/donner/main/api/type.html');
  await page.getByLabel('Documentation version').selectOption('0.8.0');
  await expect(page).toHaveURL(/\/0\.8\.0\/index.html\?docs-missing=api%2Ftype.html$/);
});

test('keyboard and narrow dark layout keep version and search controls separate', async ({page}) => {
  await page.setViewportSize({width: 320, height: 640});
  await page.emulateMedia({colorScheme: 'dark'});
  await page.goto('http://localhost/donner/main/index.html');
  const selector = page.getByLabel('Documentation version');
  await expect(selector).toBeEnabled();
  await selector.focus();
  await expect(selector).toBeFocused();
  const box = await selector.boundingBox();
  const search = await page.getByLabel('Search', {exact: true}).boundingBox();
  expect(box).not.toBeNull(); expect(search).not.toBeNull();
  expect(box!.x + box!.width).toBeLessThanOrEqual(320);
  expect(box!.y + box!.height).toBeLessThanOrEqual(search!.y);
});
