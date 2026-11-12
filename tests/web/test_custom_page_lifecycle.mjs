import assert from "node:assert/strict";
import vm from "node:vm";
import { resolve } from "node:path";
import { readFile } from "node:fs/promises";
const asset = (name) =>
  process.argv[2]
    ? resolve(process.argv[2], name)
    : new URL("../../web/custom-pages/" + name, import.meta.url);
const timers = new Map(),
  requests = [],
  listeners = {},
  docListeners = {};
let next = 0;
const spinner = {
  hidden: true,
  removeAttribute() {
    this.hidden = false;
  },
  setAttribute() {
    this.hidden = true;
  }
};
const warning = { hidden: true, textContent: "" },
  form = {};
class XHR {
  open(method, url) {
    this.method = method;
    this.url = url;
  }
  send() {
    requests.push(this);
  }
  abort() {
    this.readyState = 4;
    this.status = 0;
    this.responseText = "";
    this.onreadystatechange?.();
    this.onabort?.();
  }
  complete(status = 200, value = {}) {
    this.readyState = 4;
    this.status = status;
    this.responseText = JSON.stringify(value);
    this.onreadystatechange?.();
  }
}
const c = vm.createContext({
  console,
  XMLHttpRequest: XHR,
  location: { pathname: "/user/probe.html" },
  setTimeout(fn, ms) {
    const id = ++next;
    timers.set(id, { fn, ms });
    return id;
  },
  clearTimeout(id) {
    timers.delete(id);
  },
  document: {
    addEventListener(k, f) {
      docListeners[k] = f;
    },
    getElementsByName() {
      return [];
    },
    getElementById(id) {
      return id === "spinner" ? spinner : id === "warning" ? warning : null;
    },
    getElementsByTagName(k) {
      return k === "form" ? [form] : [];
    }
  }
});
c.window = c;
c.addEventListener = (k, f) => (listeners[k] = f);
vm.runInContext(await readFile(asset("ui.js"), "utf8"), c);
let ok = 0,
  err = 0;
c.ajaxSpin(
  "GET",
  "/one",
  () => ok++,
  () => err++
);
c.ajaxSpin(
  "GET",
  "/two",
  () => ok++,
  () => err++
);
assert.equal(spinner.hidden, false);
requests.shift().complete();
assert.equal(spinner.hidden, false);
requests.shift().complete();
assert.equal(spinner.hidden, true);
assert.equal(ok, 2);
c.ajaxSpin(
  "GET",
  "/timeout",
  () => ok++,
  () => err++
);
const xhr = requests.shift();
const timeout = [...timers.values()].find((t) => t.ms === 9000);
timeout.fn();
xhr.complete();
assert.equal(ok, 2);
assert.equal(err, 1);
assert.equal(spinner.hidden, true);
console.log("PASS custom-page spinner overlap and exactly-once AJAX timeout/abort completion");
timers.clear();
vm.runInContext(await readFile(asset("userpage.js"), "utf8"), c);
docListeners.DOMContentLoaded();
assert.equal(requests.length, 1);
requests.shift().complete();
function fire(ms) {
  const entry = [...timers.entries()].find(([, v]) => v.ms === ms);
  assert(entry, "Missing timer " + ms);
  timers.delete(entry[0]);
  entry[1].fn();
}
for (let burst = 0; burst < 2; burst++) {
  assert.equal(form.onsubmit(), true);
  for (let i = 0; i < 5; i++) {
    fire(250);
    assert.equal(requests.length, 1);
    assert(requests[0].url.includes("reason=refresh"));
    requests.shift().complete();
  }
  assert.equal([...timers.values()].filter((t) => t.ms === 250).length, 0);
}
console.log("PASS custom-page submissions each receive their own five-read refresh budget");
c.refreshFormData();
fire(250);
c.refreshFormData();
fire(250);
assert.equal(requests.length, 1);
requests.shift().complete();
fire(250);
assert.equal(requests.length, 1);
requests.shift().complete();
c.refreshRate = 100;
c.notifyResponse({});
fire(100);
assert.equal(requests.length, 1);
requests.shift().complete(503);
for (let i = 0; i < 2; i++) {
  fire(1000);
  assert.equal(requests.length, 1);
  requests.shift().complete(503);
}
assert.equal(warning.hidden, false);
assert(warning.textContent.includes("unavailable"));
console.log("PASS coalesced refresh reads and bounded recovery after periodic failure");
c.refreshFailures = 0;
c.fetchFormData("refresh");
const active = requests.shift();
listeners.pagehide();
assert.equal(c.pageActive, false);
assert.equal(c.refreshFlight, null);
assert.equal(timers.size, 0);
active.complete();
assert.equal(timers.size, 0);
listeners.pageshow({ persisted: true });
assert.equal(requests.length, 1);
requests.shift().complete();
console.log("PASS custom-page departure cancels timers/reads and BFCache restoration resumes");

XHR.prototype.send = function () {
  throw Error("Synchronous transport failure");
};
c.refreshRate = 0;
c.fetchFormData("refresh");
assert.equal(c.refreshFlight, null);
assert([...timers.values()].some((v) => v.ms === 1000));
console.log("PASS synchronous transport failure releases custom-page refresh ownership");
