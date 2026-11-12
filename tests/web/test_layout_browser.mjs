import assert from "node:assert/strict";
import { parseArgs } from "node:util";
import { writeFile } from "node:fs/promises";
import { launch } from "../support/headless_browser.mjs";
const {
  values: { base, browser, output }
} = parseArgs({
  options: { base: { type: "string" }, browser: { type: "string" }, output: { type: "string" } }
});
if (new URL(base).hostname !== "127.0.0.1") throw Error("Loopback fixture only");
const page = await launch(browser, output),
  results = [],
  errors = [];
const run = (fn) => page.evaluate(`(${fn})()`);
page.on("Runtime.exceptionThrown", (e) => errors.push(e.exceptionDetails.text));
const inspect = () => {
  const visible = (e) =>
    e.getClientRects().length &&
    e.checkVisibility({ checkOpacity: true, checkVisibilityCSS: true }) &&
    !e.closest(".xterm");
  const nodes = [
    ...document.querySelectorAll(
      "main button,main input,main select,main textarea,main label,main h1,main h2,main h3,main summary,main p,main pre,main progress,.terminal-host"
    )
  ].filter(visible);
  const describe = (e) =>
    e.id ? "#" + e.id : e.tagName.toLowerCase() + ":" + e.textContent.trim().slice(0, 48);
  const rect = (e) => {
    const r = e.getBoundingClientRect();
    let a = { l: r.left, r: r.right, t: r.top, b: r.bottom };
    for (let p = e.parentElement; p && p !== document.body; p = p.parentElement) {
      const c = getComputedStyle(p),
        b = p.getBoundingClientRect();
      if (["auto", "scroll", "hidden", "clip"].includes(c.overflowX)) {
        a.l = Math.max(a.l, b.left);
        a.r = Math.min(a.r, b.right);
      }
      if (["auto", "scroll", "hidden", "clip"].includes(c.overflowY)) {
        a.t = Math.max(a.t, b.top);
        a.b = Math.min(a.b, b.bottom);
      }
    }
    return a;
  };
  const overlap = [];
  for (let i = 0; i < nodes.length; i++)
    for (let j = i + 1; j < nodes.length; j++) {
      const a = nodes[i],
        b = nodes[j];
      if (a.contains(b) || b.contains(a)) continue;
      const x = rect(a),
        y = rect(b);
      if (
        Math.min(x.r, y.r) - Math.max(x.l, y.l) > 1.5 &&
        Math.min(x.b, y.b) - Math.max(x.t, y.t) > 1.5
      )
        overlap.push([describe(a), describe(b)]);
    }
  const small = nodes
    .filter(
      (e) =>
        e.tagName === "BUTTON" &&
        (e.getBoundingClientRect().height < 43 || e.getBoundingClientRect().width < 43)
    )
    .map(describe);
  const reload = document.querySelector("#management-reload"),
    first = document.querySelector("#management-forms>section");
  const dividerOverlap = !!(
    reload &&
    first &&
    first.getBoundingClientRect().top - reload.getBoundingClientRect().bottom < 20
  );
  const updateReload = document.querySelector("#update-reload"),
    info = document.querySelector("#view>dl.details");
  const updateDividerOverlap = !!(
    updateReload &&
    info &&
    updateReload.getBoundingClientRect().top - info.getBoundingClientRect().bottom < 20
  );
  const brand = document.querySelector(".brand").getBoundingClientRect();
  for (const button of document.querySelectorAll(".topbar button"))
    if (visible(button)) {
      const r = button.getBoundingClientRect();
      if (
        Math.min(brand.right, r.right) > Math.max(brand.left, r.left) + 1 &&
        Math.min(brand.bottom, r.bottom) > Math.max(brand.top, r.top) + 1
      )
        overlap.push(["brand", describe(button)]);
    }
  const exp = document.querySelector("#diagnostics-export"),
    heading = document.querySelector("#diagnostics-log-heading"),
    actions = document.querySelector("#diagnostics-log-actions"),
    log = document.querySelector("#diagnostics-log");
  const diagnosticsSpacing = !!(
    exp &&
    heading &&
    actions &&
    log &&
    (heading.getBoundingClientRect().top - exp.getBoundingClientRect().bottom < 20 ||
      log.getBoundingClientRect().top - actions.getBoundingClientRect().bottom < 20)
  );
  return {
    overflow:
      document.documentElement.scrollWidth > innerWidth ||
      document.querySelector("main").scrollWidth > document.querySelector("main").clientWidth,
    overlap,
    small,
    dividerOverlap,
    updateDividerOverlap,
    diagnosticsSpacing
  };
};
try {
  await page.send("Page.navigate", { url: base + "/index.html" });
  await page.wait("document.body?.dataset.ready==='true'");
  const views = [
    ["dashboard", "document.querySelector('[data-card=wifi]')"],
    ["network", "document.querySelector('#network-current-mode')"],
    [
      "serial",
      "document.querySelector('#serial-uart')?.dataset.state==='clean' && document.querySelector('#serial-terminal textarea')"
    ],
    ["integrations", "document.querySelector('#management-mqtt')?.dataset.state==='clean'"],
    ["settings", "document.querySelector('#management-description')?.dataset.state==='clean'"],
    ["diagnostics", "document.querySelector('#diagnostics-log')"],
    ["firmware", "document.querySelector('#view')?.dataset.updateState==='ready'"],
    ["pages", "document.querySelector('#view')?.dataset.updateState==='ready'"]
  ];
  for (const [width, scale] of [
    [360, 1],
    [768, 1],
    [1440, 1],
    [360, 2]
  ]) {
    await page.send("Emulation.setDeviceMetricsOverride", {
      width,
      height: 1000,
      deviceScaleFactor: 1.25,
      mobile: false
    });
    await page.evaluate(
      `document.documentElement.style.fontSize=${JSON.stringify(scale * 100 + "%")}`
    );
    for (const [route, ready] of views) {
      await page.evaluate(`location.hash=${JSON.stringify("#/" + route)}`);
      await page.wait(ready, 12000);
      for (const expanded of [false, true]) {
        await page.evaluate(
          `(()=>{for(const d of document.querySelectorAll('#view details'))d.open=${expanded};})()`
        );
        await page.evaluate("new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r)))");
        const frame = await page.evaluate(
          `(()=>{const main=document.querySelector('main'),header=document.querySelector('.topbar'),nav=document.querySelector('nav'),before=[header.getBoundingClientRect().top,nav.getBoundingClientRect().top,main.getBoundingClientRect().top];main.scrollTop=100;const moved=main.scrollTop,after=[header.getBoundingClientRect().top,nav.getBoundingClientRect().top,main.getBoundingClientRect().top];main.scrollTop=0;return {before,after,moved,shouldScroll:main.scrollHeight>main.clientHeight,rootScroll:scrollY};})()`
        );
        assert.deepEqual(frame.after, frame.before);
        assert.equal(frame.rootScroll, 0);
        if (frame.shouldScroll) assert(frame.moved > 0);
        const result = await page.evaluate(`(${inspect})()`);
        results.push({ route, width, scale, expanded, ...result });
        await page.screenshot(`${route}-${width}-${scale}-${expanded ? "expanded" : "closed"}.png`);
        if (result.overflow)
          console.log(
            await page.evaluate(
              `(()=>[...document.querySelectorAll('#view *')].filter(e=>e.checkVisibility({checkOpacity:true,checkVisibilityCSS:true})&&e.getBoundingClientRect().right>innerWidth+1&&!e.closest('.xterm')).map(e=>({tag:e.tagName,id:e.id,cls:e.className,width:e.getBoundingClientRect().width,right:e.getBoundingClientRect().right,text:e.textContent.slice(0,55)})))()`
            )
          );
        assert.equal(result.overflow, false, JSON.stringify(results.at(-1)));
        assert.deepEqual(result.overlap, [], JSON.stringify(results.at(-1)));
        assert.deepEqual(result.small, [], JSON.stringify(results.at(-1)));
        assert.equal(result.dividerOverlap, false, JSON.stringify(results.at(-1)));
        assert.equal(result.updateDividerOverlap, false, JSON.stringify(results.at(-1)));
        assert.equal(result.diagnosticsSpacing, false, JSON.stringify(results.at(-1)));
      }
    }
  }
  // A wider fallback font must not let long checkbox labels escape their grid cell.
  await page.evaluate("location.hash='#/integrations'");
  await page.wait("document.querySelector('#management-mqtt')?.dataset.state==='clean'");
  await page.evaluate(
    "document.querySelector('main').style.scrollbarGutter='stable';for(const d of document.querySelectorAll('#view details'))d.open=true;for(const label of document.querySelectorAll('#view label'))label.style.fontFamily='monospace'"
  );
  assert.equal(
    await page.evaluate(
      "[...document.querySelectorAll('#view label')].every(e=>e.scrollWidth<=e.clientWidth+1)"
    ),
    true,
    "Large checkbox labels must wrap with wider fallback fonts"
  );
  assert.equal(
    (await page.evaluate(`(${inspect})()`)).overflow,
    false,
    "Fallback font must not cause page overflow"
  );
  await page.evaluate("location.hash='#/settings'");
  await page.wait("document.querySelector('#management-description')?.dataset.state==='clean'");
  {
    await page.send("Emulation.setDeviceMetricsOverride", {
      width: 360,
      height: 360,
      deviceScaleFactor: 1,
      mobile: false
    });
    await run(() => (document.documentElement.style.fontSize = "200%"));
    const v = await run(() => {
      const h = document.querySelector(".topbar"),
        main = document.querySelector("main"),
        r = main.getBoundingClientRect();
      return {
        visible: Math.max(0, Math.min(r.bottom, innerHeight) - Math.max(0, r.top)),
        scrolls: main.scrollHeight > main.clientHeight,
        headerScrolls: h.scrollHeight > h.clientHeight
      };
    });
    assert(v.visible > 100, JSON.stringify(v));
    assert(v.scrolls);
    assert(v.headerScrolls);
    await run(() => document.querySelector("#theme").focus());
    await run(() => document.querySelector("#menu").click());
    await page.wait("!document.querySelector('#navigation').hidden");
    assert((await run(() => document.querySelector("main").getBoundingClientRect().height)) > 0);
    await page.key("Escape");
    await page.screenshot("short-viewport-fixed.png");
    await page.send("Emulation.setDeviceMetricsOverride", {
      width: 1440,
      height: 900,
      deviceScaleFactor: 1,
      mobile: false
    });
    await run(() => (document.documentElement.style.fontSize = ""));
  }
  assert.deepEqual(errors, []);
  await writeFile(output + "/result.json", JSON.stringify({ pass: true, cases: results }, null, 2));
  console.log(
    "PASS all eight screens: 64 collapsed/expanded layout cases, 360/768/1440px, fractional DPI and 200% text; no overlaps or horizontal page overflow"
  );
} catch (e) {
  await writeFile(
    output + "/result.json",
    JSON.stringify({ pass: false, cases: results, errors }, null, 2)
  );
  await page.screenshot("failure.png");
  throw e;
} finally {
  await page.close();
}
