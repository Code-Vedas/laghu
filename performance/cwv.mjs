import fs from 'node:fs';
import {createRequire} from 'node:module';

const require = createRequire(import.meta.url);
const lighthouse = (await import('/usr/local/lib/node_modules/lighthouse/core/index.js')).default;
const puppeteer = require('/usr/local/lib/node_modules/puppeteer-core');

const [url, output] = process.argv.slice(2);
if (!url || !output) throw new Error('usage: cwv.mjs URL OUTPUT');
const chrome = '/usr/bin/chromium';
const browser = await puppeteer.launch({executablePath: chrome, headless: true, args: ['--no-sandbox', '--disable-dev-shm-usage']});
try {
  const page = await browser.newPage();
  await page.goto(url, {waitUntil: 'networkidle0'});
  await page.click('#laghu-interaction');
  await new Promise((resolve) => setTimeout(resolve, 250));
  const inp = await page.evaluate(() => window.laghuInteractionDuration);
  if (typeof inp !== 'number' || inp <= 0) throw new Error('CDP interaction duration missing');
  const decisions = await page.evaluate(() => ({
    image_dimensions: [...document.images].filter((image) => image.hasAttribute('width') && image.hasAttribute('height')).length,
    lazy_images: [...document.images].filter((image) => image.loading === 'lazy').length,
    deferred_scripts: [...document.scripts].filter((script) => script.defer).length,
    preload_images: [...document.querySelectorAll('link[rel="preload"][as="image"]')].length,
  }));
  if (!Object.values(decisions).every((value) => Number.isInteger(value) && value >= 0)) throw new Error('browser decision evidence invalid');
  const result = await lighthouse(url, {port: new URL(browser.wsEndpoint()).port, output: 'json', logLevel: 'error',
    onlyCategories: ['performance'], chromeFlags: ['--headless=new', '--no-sandbox', '--disable-dev-shm-usage']});
  if (!result?.lhr?.audits) throw new Error('Lighthouse audits missing');
  fs.writeFileSync(output, JSON.stringify({audits: result.lhr.audits, inp_ms: inp, decisions}));
} finally {
  await browser.close();
}
