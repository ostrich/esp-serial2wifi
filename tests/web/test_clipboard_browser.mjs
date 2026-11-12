// Real copy/paste on an insecure HTTP document; every request is fulfilled locally.
import assert from "node:assert/strict";
import { readFile, mkdir, writeFile } from "node:fs/promises";
import { parseArgs } from "node:util";
import { launch } from "../support/headless_browser.mjs";
const {
  values: { browser, output = ".build-tests/clipboard" }
} = parseArgs({ options: { browser: { type: "string" }, output: { type: "string" } } });
await mkdir(output, { recursive: true });
const page = await launch(browser, output),
  results = [];
const core = await readFile(new URL("../../web/app/core.js", import.meta.url), "utf8");
const html =
  '<!doctype html><pre id="source"></pre><button id="copy">Copy</button><p id="status"></p><textarea id="paste"></textarea><script type="module">import {copy} from "/core.js";document.querySelector("#source").textContent="stty rows 40 cols 120\\n雪 & <value>";document.querySelector("#copy").onclick=async()=>{document.querySelector("#status").textContent=await copy(document.querySelector("#source"));};document.body.dataset.ready="true";</script>';
const run = (fn) => page.evaluate(`(${fn})()`),
  errors = [];
page.on("Runtime.exceptionThrown", (p) => errors.push(p.exceptionDetails.text));
const shortcut = async (key) => {
  const code = key.toUpperCase().charCodeAt(0);
  await page.send("Input.dispatchKeyEvent", {
    type: "keyDown",
    key,
    code: "Key" + key.toUpperCase(),
    windowsVirtualKeyCode: code,
    modifiers: 2
  });
  await page.send("Input.dispatchKeyEvent", {
    type: "keyUp",
    key,
    code: "Key" + key.toUpperCase(),
    windowsVirtualKeyCode: code,
    modifiers: 2
  });
};
async function seed() {
  await run(() => {
    const input = document.querySelector("#paste");
    input.value = "clipboard sentinel";
    input.focus();
    input.select();
  });
  await shortcut("c");
  await run(() => {
    document.querySelector("#paste").value = "";
  });
}
async function click() {
  await run(() => {
    document.querySelector("#status").textContent = "";
    document.querySelector("#copy").focus();
  });
  await page.key("Enter");
  await page.wait("document.querySelector('#status').textContent!==''");
}
async function paste() {
  await run(() => document.querySelector("#paste").focus());
  await shortcut("v");
  await page.wait("document.querySelector('#paste').value!==''");
  return run(() => document.querySelector("#paste").value);
}
try {
  await page.send("Fetch.enable", { patterns: [{ urlPattern: "*", requestStage: "Request" }] });
  page.on("Fetch.requestPaused", (p) => {
    const u = new URL(p.request.url),
      body = u.pathname === "/core.js" ? core : html;
    assert.equal(u.origin, "http://esp-clipboard.invalid");
    page
      .send("Fetch.fulfillRequest", {
        requestId: p.requestId,
        responseCode: 200,
        responseHeaders: [
          {
            name: "Content-Type",
            value: u.pathname === "/core.js" ? "text/javascript" : "text/html"
          }
        ],
        body: Buffer.from(body).toString("base64")
      })
      .catch(() => {});
  });
  await page.send("Page.navigate", { url: "http://esp-clipboard.invalid/" });
  await page.wait("document.body?.dataset.ready==='true'");
  assert.equal(await run(() => isSecureContext), false);
  assert.equal(await run(() => !!navigator.clipboard), false);
  const expected = await run(() => document.querySelector("#source").textContent);
  await seed();
  await click();
  assert.equal(
    await run(() => document.querySelector("#status").textContent),
    "Copied to clipboard."
  );
  assert.equal(await run(() => document.activeElement.id), "copy");
  assert.equal(await run(() => document.querySelectorAll("textarea").length), 1);
  assert.equal(await paste(), expected);
  results.push("Insecure HTTP: real keyboard paste matches copied multiline Unicode text");
  await run(() =>
    Object.defineProperty(navigator, "clipboard", {
      configurable: true,
      value: {
        writeText: async () => {
          throw new Error("Denied");
        }
      }
    })
  );
  await seed();
  await click();
  assert.equal(
    await run(() => document.querySelector("#status").textContent),
    "Copied to clipboard."
  );
  assert.equal(await paste(), expected);
  results.push("Rejected Clipboard API: real fallback copy/paste and focus cleanup");
  await seed();
  await run(() => {
    document.execCommand = () => false;
  });
  await click();
  assert.equal(
    await run(() => document.querySelector("#status").textContent),
    "Text selected. Use your browser’s Copy command."
  );
  assert.equal(await run(() => getSelection().toString()), expected);
  assert.equal(await paste(), "clipboard sentinel");
  results.push("Blocked fallback: truthful manual-selection message and clipboard unchanged");
  assert.deepEqual(errors, []);
  await writeFile(output + "/result.json", JSON.stringify({ pass: true, results }, null, 2));
  for (const result of results) console.log("PASS " + result);
} catch (error) {
  await page.screenshot("failure.png");
  await writeFile(
    output + "/failure-dom.html",
    await page.evaluate("document.documentElement.outerHTML")
  );
  throw error;
} finally {
  await page.close();
}
