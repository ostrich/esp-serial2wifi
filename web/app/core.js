/* Shared browser primitives. No device values are interpreted as markup. */
export class ApiError extends Error {
  constructor(kind, status = 0, ambiguous = false) {
    const messages = {
      http: "The device rejected the request.",
      timeout: "The device did not reply in time.",
      abort: "The request was cancelled.",
      network: "The device could not be reached.",
      malformed: "The device returned an unreadable response."
    };
    super(messages[kind] || messages.network);
    this.name = "ApiError";
    this.kind = kind;
    this.status = status;
    this.ambiguous = ambiguous;
  }
}

const cancelled = () => new ApiError("abort");
export const delay = (ms, signal) =>
  new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(cancelled());
      return;
    }
    const abort = () => {
      clearTimeout(timer);
      reject(cancelled());
    };
    const timer = setTimeout(() => {
      signal?.removeEventListener("abort", abort);
      resolve();
    }, ms);
    signal?.addEventListener("abort", abort, { once: true });
  });

/* XHR exposes actual browser upload progress. Request lifetime, serialization
 * and mutation retry rules remain owned by ApiClient. */
function uploadResponse(url, options, progress) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const xhr = new XMLHttpRequest(),
      abort = () => xhr.abort();
    const done = (fn) => (value) => {
      if (settled) return;
      settled = true;
      options.signal.removeEventListener("abort", abort);
      fn(value);
    };
    xhr.open(options.method, url);
    for (const [key, value] of Object.entries(options.headers || {}))
      xhr.setRequestHeader(key, value);
    xhr.upload.onprogress = (event) => {
      if (!settled)
        progress({ loaded: event.loaded, total: event.lengthComputable ? event.total : null });
    };
    xhr.onload = done(() =>
      resolve({
        ok: xhr.status >= 200 && xhr.status < 300,
        status: xhr.status,
        text: async () => xhr.responseText
      })
    );
    xhr.onerror = done(() => reject(new TypeError("Upload connection failed")));
    xhr.onabort = done(() => reject(new DOMException("Upload cancelled", "AbortError")));
    options.signal.addEventListener("abort", abort, { once: true });
    if (options.signal.aborted) {
      options.signal.removeEventListener("abort", abort);
      reject(new DOMException("Upload cancelled", "AbortError"));
      return;
    }
    xhr.send(options.body);
  });
}

export class ApiClient extends EventTarget {
  constructor({
    fetcher = globalThis.fetch.bind(globalThis),
    visibility = () => !globalThis.document?.hidden,
    concurrency = 2
  } = {}) {
    super();
    this.fetcher = fetcher;
    this.visible = visibility;
    if (!Number.isInteger(concurrency) || concurrency < 1 || concurrency > 2)
      throw new TypeError("Device request concurrency must be one or two.");
    this.limit = concurrency;
    this.active = 0;
    this.queue = [];
    this.pending = new Map();
  }
  activity(operation, delta) {
    const count = (this.pending.get(operation) || 0) + delta;
    if (count) this.pending.set(operation, count);
    else this.pending.delete(operation);
    this.dispatchEvent(new CustomEvent("activity", { detail: { operation, pending: count } }));
  }
  acquire(signal) {
    return new Promise((resolve, reject) => {
      if (signal.aborted) {
        reject(cancelled());
        return;
      }
      const entry = { resolve, reject, signal };
      entry.abort = () => {
        this.queue = this.queue.filter((item) => item !== entry);
        reject(cancelled());
      };
      if (this.active < this.limit) {
        this.active++;
        resolve();
      } else {
        this.queue.push(entry);
        signal.addEventListener("abort", entry.abort, { once: true });
      }
    });
  }
  release() {
    const entry = this.queue.shift();
    if (entry) {
      entry.signal.removeEventListener("abort", entry.abort);
      entry.resolve();
    } else this.active--;
  }
  async request(
    path,
    {
      method = "GET",
      query,
      body,
      signal,
      timeout,
      retries,
      operation = path,
      validate,
      responseType = "json",
      onUploadProgress
    } = {}
  ) {
    method = method.toUpperCase();
    const read = method === "GET";
    if (!["json", "text", "bytes"].includes(responseType))
      throw new TypeError("Choose JSON, text or bytes.");
    if (!path.startsWith("/") || path.startsWith("//") || /[?#]/.test(path))
      throw new TypeError("Use a local path and the query serializer.");
    if (!read && query && Object.keys(query).length)
      throw new TypeError("Send changes in the request body.");
    if (query && Object.keys(query).some((key) => /pass|secret|token|api.?key/i.test(key)))
      throw new TypeError("Secrets must not appear in a URL.");
    const url = path + (query ? "?" + new URLSearchParams(query) : "");
    const budget = timeout ?? (read ? 5000 : 30000);
    const attempts = read ? (retries ?? 1) + 1 : 1;
    if (
      !Number.isFinite(budget) ||
      budget <= 0 ||
      !Number.isInteger(attempts) ||
      attempts < 1 ||
      attempts > 4
    )
      throw new TypeError("Invalid request budget.");
    let encoded,
      headers = read ? undefined : { "X-Serial2WiFi-Request": "1" };
    if (body !== undefined) {
      if (read) throw new TypeError("GET requests cannot change configuration.");
      if (body instanceof URLSearchParams) {
        encoded = body.toString();
        headers = { ...headers, "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8" };
      } else if (typeof FormData !== "undefined" && body instanceof FormData) {
        encoded = body; // The browser supplies the matching multipart boundary.
      } else if (body instanceof Blob || body instanceof ArrayBuffer || ArrayBuffer.isView(body)) {
        encoded = body;
        headers = { ...headers, "Content-Type": "application/octet-stream" };
      } else {
        encoded = JSON.stringify(body);
        headers = { ...headers, "Content-Type": "application/json" };
      }
    }
    this.activity(operation, 1);
    try {
      let lastError;
      for (let attempt = 0; attempt < attempts; attempt++) {
        if (attempt && !this.visible()) throw lastError;
        const controller = new AbortController();
        let timedOut = false,
          acquired = false;
        const abort = () => controller.abort();
        if (signal?.aborted) throw cancelled();
        signal?.addEventListener("abort", abort, { once: true });
        const timer = setTimeout(() => {
          timedOut = true;
          controller.abort();
        }, budget);
        try {
          await this.acquire(controller.signal);
          acquired = true;
          if (controller.signal.aborted) throw cancelled();
          const response = await (
            onUploadProgress && !read
              ? (url, opts) => uploadResponse(url, opts, onUploadProgress)
              : this.fetcher
          )(url, {
            method,
            headers,
            body: encoded,
            signal: controller.signal,
            cache: "no-store",
            credentials: "same-origin",
            redirect: "error"
          });
          if (!response.ok)
            throw new ApiError(
              "http",
              response.status,
              !read && (response.status >= 500 || response.status === 408)
            );
          let data;
          try {
            if (responseType === "bytes") data = new Uint8Array(await response.arrayBuffer());
            else {
              const text = await response.text();
              data = responseType === "text" ? text : JSON.parse(text);
            }
          } catch (error) {
            if (controller.signal.aborted) throw error;
            throw new ApiError("malformed", 0, !read);
          }
          if (validate) {
            let valid = false;
            try {
              valid = validate(data);
            } catch (_) {
              /* Treat invalid shapes like invalid JSON. */
            }
            if (!valid) throw new ApiError("malformed", 0, !read);
          }
          return data;
        } catch (cause) {
          let error;
          if (controller.signal.aborted)
            error = new ApiError(timedOut ? "timeout" : "abort", 0, !read && acquired);
          else
            error =
              cause instanceof ApiError ? cause : new ApiError("network", 0, !read && acquired);
          lastError = error;
          const retryable =
            error.kind === "network" ||
            error.kind === "timeout" ||
            (error.kind === "http" && [408, 429, 502, 503, 504].includes(error.status));
          if (!read || !retryable || attempt + 1 === attempts || !this.visible() || signal?.aborted)
            throw error;
        } finally {
          clearTimeout(timer);
          signal?.removeEventListener("abort", abort);
          if (acquired) this.release();
        }
        await delay(Math.min(250 * 2 ** attempt, 2000), signal);
      }
    } finally {
      this.activity(operation, -1);
    }
  }
}

/* One in-flight poll, scheduled from completion; no interval catch-up bursts. */
export function createPoll(
  client,
  path,
  {
    onData,
    onError = () => {},
    validate,
    interval = 12000,
    maxFailures = 3,
    document: page = globalThis.document
  } = {}
) {
  if (typeof onData !== "function") throw new TypeError("A poll requires a data handler.");
  let running = false,
    flight = null,
    timer = null,
    failures = 0,
    resume = false;
  const clear = () => {
    clearTimeout(timer);
    timer = null;
  };
  const visible = () => !page?.hidden;
  async function tick() {
    clear();
    if (!running || !visible() || flight) return;
    const current = new AbortController();
    flight = current;
    try {
      const data = await client.request(path, { signal: current.signal, retries: 0, validate });
      if (running && !current.signal.aborted) {
        failures = 0;
        onData(data);
      }
    } catch (error) {
      if (running && !current.signal.aborted) {
        failures++;
        onError(error, failures >= maxFailures);
      }
    } finally {
      if (flight === current) flight = null;
      if (running && visible() && failures < maxFailures) {
        timer = setTimeout(tick, resume ? 0 : Math.min(interval * 2 ** failures, 60000));
        resume = false;
      }
    }
  }
  function visibility() {
    clear();
    if (!visible()) flight?.abort();
    else if (flight) resume = true;
    else if (failures < maxFailures) tick();
  }
  return {
    start() {
      if (running) return;
      running = true;
      failures = 0;
      resume = Boolean(flight);
      page?.addEventListener("visibilitychange", visibility);
      tick();
    },
    refresh() {
      if (!running) return;
      failures = 0;
      tick();
    },
    stop() {
      running = false;
      resume = false;
      clear();
      flight?.abort();
      page?.removeEventListener("visibilitychange", visibility);
    }
  };
}

export class Notice {
  constructor(element) {
    this.element = element;
    this.generation = 0;
    this.timer = null;
  }
  show(message, { kind = "info", duration = 7000 } = {}) {
    clearTimeout(this.timer);
    const generation = ++this.generation;
    this.element.textContent = message;
    this.element.dataset.kind = kind;
    this.element.hidden = false;
    if (duration > 0) this.timer = setTimeout(() => this.clear(generation), duration);
    return generation;
  }
  clear(generation = this.generation) {
    if (generation !== this.generation) return;
    clearTimeout(this.timer);
    this.element.hidden = true;
    this.element.textContent = "";
  }
}

const secretField = (field) => field.type === "password" || field.dataset.secret === "true";
const clone = (values) => ({ ...values });
const equal = (a, b) =>
  Object.keys(a).length === Object.keys(b).length &&
  Object.keys(a).every((key) => a[key] === b[key]);

/* Explicit Save/Cancel, one mutation per intent, authoritative read-back. */
export class DraftForm extends EventTarget {
  constructor(
    form,
    {
      read,
      write,
      validate = () => ({}),
      verify,
      message,
      notice,
      describe = () => "Saved.",
      describeError = () =>
        "We couldn’t confirm the save. Your edits are still here. Check the connection before trying again.",
      allowCleanSave = false
    } = {}
  ) {
    super();
    this.form = form;
    this.read = read;
    this.write = write;
    this.allowCleanSave = allowCleanSave;
    this.validate = validate;
    this.message = message;
    this.notice = notice;
    this.describe = describe;
    this.describeError = describeError;
    this.verify =
      verify ||
      ((draft, committed) =>
        this.fields().every((field) =>
          secretField(field)
            ? !draft[field.name]
            : draft[field.name] ===
              (field.type === "checkbox"
                ? Boolean(committed[field.name])
                : String(committed[field.name] ?? ""))
        ));
    this.baseline = null;
    this.state = "loading";
    this.busy = false;
    this.locked = false;
    this.generation = 0;
    this.destroyed = false;
    this.controller = null;
    this.errors = {};
    this.disabled = new WeakMap(this.fields().map((field) => [field, field.disabled]));
    this.input = () => {
      if (!this.busy && !this.locked && this.baseline) {
        this.errors = this.validate(this.values());
        this.change(this.dirty ? "dirty" : "clean");
      }
    };
    this.submit = (event) => {
      event.preventDefault();
      this.save();
    };
    this.cancel = (event) => {
      event.preventDefault();
      this.reset();
    };
    form.addEventListener("input", this.input);
    form.addEventListener("change", this.input);
    form.addEventListener("submit", this.submit);
    this.cancelButton = form.querySelector("[data-cancel]");
    this.cancelButton?.addEventListener("click", this.cancel);
    this.unload = (event) => {
      if (this.dirty || this.busy) {
        event.preventDefault();
        event.returnValue = "";
      }
    };
    globalThis.window?.addEventListener("beforeunload", this.unload);
    this.change("loading");
  }
  fields() {
    return [...this.form.querySelectorAll("input[name],select[name],textarea[name]")];
  }
  values() {
    return Object.fromEntries(
      this.fields().map((field) => [
        field.name,
        field.type === "checkbox" ? field.checked : field.value
      ])
    );
  }
  set(values) {
    for (const field of this.fields()) {
      const value = secretField(field) ? "" : (values[field.name] ?? "");
      if (field.type === "checkbox") field.checked = Boolean(value);
      else field.value = value;
    }
  }
  /* Update a baseline only after an adapter has verified authoritative metadata.
   * Inputs, including a failed transaction's secret draft, stay untouched. */
  rebase(values) {
    if (this.destroyed) return;
    this.baseline = Object.fromEntries(
      this.fields().map((field) => [
        field.name,
        secretField(field)
          ? ""
          : field.type === "checkbox"
            ? Boolean(values[field.name])
            : String(values[field.name] ?? "")
      ])
    );
  }
  get dirty() {
    return this.baseline !== null && !equal(this.baseline, this.values());
  }
  change(state) {
    if (this.destroyed) return;
    this.state = state;
    this.form.dataset.state = state;
    const busy = this.busy || this.locked || state === "loading";
    this.form.setAttribute("aria-busy", String(busy));
    for (const field of this.fields()) {
      field.disabled = busy || this.baseline === null || this.disabled.get(field) === true;
      field.setAttribute("aria-invalid", String(Boolean(this.errors[field.name])));
      const hint = this.form.querySelector(`[data-error="${CSS.escape(field.name)}"]`);
      if (hint) {
        hint.textContent = this.errors[field.name] || "";
        hint.hidden = !this.errors[field.name];
      }
    }
    for (const button of this.form.querySelectorAll("[type=submit]"))
      button.disabled =
        busy || (!this.dirty && !this.allowCleanSave) || Object.keys(this.errors).length > 0;
    if (this.cancelButton) this.cancelButton.disabled = busy || !this.dirty;
    this.dispatchEvent(new CustomEvent("state", { detail: { state, dirty: this.dirty } }));
  }
  async load() {
    if (this.destroyed || this.busy || this.locked || this.dirty) return;
    this.controller?.abort();
    const controller = new AbortController();
    this.controller = controller;
    const generation = ++this.generation;
    this.change("loading");
    try {
      const values = await this.read(controller.signal);
      if (generation !== this.generation) return;
      this.set(values);
      this.baseline = this.values();
      this.errors = {};
      this.change("clean");
    } catch (error) {
      if (generation !== this.generation || controller.signal.aborted) return;
      if (this.message)
        this.message.textContent = "Couldn’t load settings. Check the connection and try again.";
      this.change("error");
    }
  }
  async save() {
    if (
      this.destroyed ||
      this.busy ||
      this.locked ||
      (!this.dirty && !this.allowCleanSave) ||
      this.state === "loading"
    )
      return;
    this.change("validating");
    this.errors = this.validate(this.values());
    if (Object.keys(this.errors).length) {
      if (this.message) this.message.textContent = "Check the highlighted fields.";
      this.change("dirty");
      this.fields()
        .find((field) => this.errors[field.name])
        ?.focus();
      return;
    }
    this.busy = true;
    const generation = ++this.generation;
    const controller = new AbortController();
    this.controller = controller;
    const draft = this.values();
    this.change("saving");
    if (this.message) this.message.textContent = "Saving changes…";
    try {
      const receipt = await this.write(clone(draft), controller.signal);
      if (generation !== this.generation || controller.signal.aborted) return;
      const committed = await this.read(controller.signal);
      if (generation !== this.generation) return;
      if (!this.verify(draft, committed, receipt))
        throw new Error("Changes could not be verified.");
      this.set(committed);
      this.baseline = this.values();
      this.errors = {};
      this.busy = false;
      this.change("saved");
      const description = this.describe(committed);
      if (this.message) this.message.textContent = description;
      this.notice?.show(description);
    } catch (error) {
      if (generation !== this.generation) return;
      this.busy = false;
      this.change("error");
      if (this.message) this.message.textContent = this.describeError(error);
      this.change("dirty");
    }
  }
  reset() {
    if (this.destroyed || this.busy || this.locked || !this.baseline) return;
    this.set(this.baseline);
    this.errors = {};
    if (this.message) this.message.textContent = "";
    this.change("clean");
  }
  setLocked(value) {
    this.locked = value;
    this.change(this.state);
  }
  canLeave(confirm = globalThis.confirm) {
    return (
      !(this.busy || this.dirty) ||
      confirm(
        "Leave this page? You’ll lose unsaved changes. Any save already sent to ESP Serial2WiFi will continue."
      )
    );
  }
  destroy() {
    this.destroyed = true;
    this.generation++;
    this.controller?.abort();
    this.form.removeEventListener("input", this.input);
    this.form.removeEventListener("change", this.input);
    this.form.removeEventListener("submit", this.submit);
    this.cancelButton?.removeEventListener("click", this.cancel);
    globalThis.window?.removeEventListener("beforeunload", this.unload);
    for (const field of this.fields()) if (secretField(field)) field.value = "";
    this.baseline = null;
  }
}

export const el = (tag, text = "", className = "") => {
  const n = document.createElement(tag);
  n.textContent = text;
  if (className) n.className = className;
  return n;
};
export const hex = (bytes) => Array.from(bytes, (n) => n.toString(16).padStart(2, "0")).join("");
export function savePreference(name, value) {
  try {
    localStorage.setItem("esp-serial2wifi-" + name, value);
  } catch (_) {}
}
export function readPreference(name, allowed, fallback) {
  try {
    const value = localStorage.getItem("esp-serial2wifi-" + name);
    if (allowed.includes(value)) return value;
  } catch (_) {}
  return fallback;
}
export async function copy(element) {
  const text = element.textContent;
  try {
    await navigator.clipboard.writeText(text);
    return "Copied to clipboard.";
  } catch (_) {}
  // The device UI uses HTTP, where the Clipboard API is unavailable. Keep this
  // fallback inside the button's user gesture and check whether copying worked.
  const active = document.activeElement,
    selection = getSelection(),
    ranges = [];
  if (selection)
    for (let i = 0; i < selection.rangeCount; i++)
      ranges.push(selection.getRangeAt(i).cloneRange());
  const input = el("textarea");
  input.value = text;
  input.readOnly = true;
  input.tabIndex = -1;
  input.setAttribute("aria-hidden", "true");
  input.style.cssText = "position:fixed;left:-9999px;top:0";
  document.body.append(input);
  let copied = false;
  try {
    input.focus({ preventScroll: true });
    input.select();
    input.setSelectionRange(0, text.length);
    copied = document.execCommand("copy");
  } catch (_) {
  } finally {
    input.remove();
    active?.focus({ preventScroll: true });
  }
  if (copied) {
    if (selection) {
      selection.removeAllRanges();
      for (const range of ranges) selection.addRange(range);
    }
    return "Copied to clipboard.";
  }
  if (selection) {
    const range = document.createRange();
    range.selectNodeContents(element);
    selection.removeAllRanges();
    selection.addRange(range);
    return "Text selected. Use your browser’s Copy command.";
  }
  return "Select the text and use your browser’s Copy command.";
}
export function download(value, name, type) {
  const url = URL.createObjectURL(new Blob([value], { type })),
    a = el("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
