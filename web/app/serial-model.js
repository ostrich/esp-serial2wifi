import { hex } from "./core.js";
/* Plain-text capture, not a terminal emulator. Device cursors describe raw bytes. */
const integer = (v) => Number.isSafeInteger(v) && v >= 0 && v <= 0xffffffff;
const object = (v) => v !== null && typeof v === "object";
const encoder = new TextEncoder();
export function validSerial(v) {
  return (
    object(v) &&
    v.schema === 1 &&
    Number.isInteger(v.baud) &&
    v.baud >= 300 &&
    v.baud <= 1000000 &&
    Number.isInteger(v.data_bits) &&
    v.data_bits >= 5 &&
    v.data_bits <= 8 &&
    ["N", "O", "E"].includes(v.parity) &&
    ["1", "1.5", "2"].includes(v.stop_bits) &&
    integer(v.clients) &&
    integer(v.queued_bytes) &&
    integer(v.cancelled_bytes) &&
    typeof v.programming === "boolean" &&
    (v.reset_pin === null ||
      (Number.isInteger(v.reset_pin) && v.reset_pin >= 0 && v.reset_pin <= 16)) &&
    integer(v.capture_bytes) &&
    v.capture_bytes <= 1023 &&
    Array.isArray(v.ports) &&
    v.ports.every((p) => [23, 2323].includes(p))
  );
}
export function decodeCapture(v) {
  if (
    !object(v) ||
    v.schema !== 1 ||
    !["boot", "epoch", "start", "end"].every((k) => integer(v[k])) ||
    ![null, "gap", "cleared", "restart", "cursor_unavailable"].includes(v.reason) ||
    v.encoding !== "base64" ||
    typeof v.data !== "string" ||
    v.data.length > 1364 ||
    !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(v.data) ||
    !(v.reason === null || v.reason === "gap" ? integer(v.lost) : v.lost === null)
  )
    throw new Error("Invalid capture response");
  const bytes = Uint8Array.from(atob(v.data), (c) => c.charCodeAt(0));
  if (bytes.length !== (v.end - v.start) >>> 0 || bytes.length > 1023)
    throw new Error("Capture byte count mismatch");
  return bytes;
}
export function validCapture(v) {
  try {
    decodeCapture(v);
    return true;
  } catch (_) {
    return false;
  }
}
export class CaptureLog {
  constructor({ maxBytes = 1048576, maxLines = 10000 } = {}) {
    this.maxBytes = maxBytes;
    this.maxLines = maxLines;
    this.cursor = null;
    this.clear();
  }
  clear() {
    this.hexChunks = [];
    this.hexBytes = 0;
    this.hexLines = 0;
    this.hexTrimmedBytes = 0;
    this.chunks = [];
    this.bytes = 0;
    this.lines = 0;
    this.trimmedBytes = 0;
    this.trimmedLines = 0;
    this.decoder = new TextDecoder();
    this.skipLF = false;
  }
  get hex() {
    return this.hexChunks.map((c) => c.text).join("");
  }
  get text() {
    return this.chunks.map((c) => c.text).join("");
  }
  get lineCount() {
    return this.lines + (this.bytes && !this.chunks.at(-1).text.endsWith("\n") ? 1 : 0);
  }
  get path() {
    const c = this.cursor;
    return "/api/v1/serial/capture" + (c ? `/${c.boot}/${c.epoch}/${c.end}` : "");
  }
  append(text) {
    if (!text) return;
    const bytes = encoder.encode(text).length,
      lines = (text.match(/\n/g) || []).length;
    this.chunks.push({ text, bytes, lines });
    this.bytes += bytes;
    this.lines += lines;
    while (this.bytes > this.maxBytes || this.lineCount > this.maxLines) {
      const c = this.chunks.shift();
      this.bytes -= c.bytes;
      this.lines -= c.lines;
      this.trimmedBytes += c.bytes;
      this.trimmedLines += c.lines;
    }
  }
  ingest(v) {
    const bytes = decodeCapture(v),
      prior = this.cursor;
    const reset = prior && (prior.boot !== v.boot || prior.epoch !== v.epoch);
    const discontinuity = reset || v.reason !== null || (prior && prior.end !== v.start);
    if (discontinuity) {
      this.decoder = new TextDecoder();
      this.skipLF = false;
      const message =
        v.reason === "gap"
          ? `Capture gap: ${v.lost} bytes no longer available`
          : v.reason === "cleared"
            ? "Device capture was cleared"
            : v.reason === "restart" || (prior && prior.boot !== v.boot)
              ? "ESP Serial2WiFi restarted"
              : "Capture cursor changed; intervening bytes are unavailable";
      this.append("\n[" + message + "]\n");
      this.hexChunks.push({ text: "\n[" + message + "]\n", bytes: 0, lines: 3 });
      this.hexLines += 3;
    }
    if (bytes.length) {
      let hex = "";
      for (let i = 0; i < bytes.length; i += 16)
        hex +=
          Array.from(bytes.slice(i, i + 16), (b) => b.toString(16).padStart(2, "0")).join(" ") +
          "\n";
      const lines = Math.ceil(bytes.length / 16);
      this.hexChunks.push({ text: hex, bytes: bytes.length, lines });
      this.hexBytes += bytes.length;
      this.hexLines += lines;
    }
    while (
      this.hexChunks.length &&
      (this.hexBytes > this.maxBytes || this.hexLines > this.maxLines)
    ) {
      const c = this.hexChunks.shift();
      this.hexBytes -= c.bytes;
      this.hexLines -= c.lines;
      this.hexTrimmedBytes += c.bytes;
    }
    let text = "";
    for (const c of this.decoder.decode(bytes, { stream: true })) {
      if (c === "\n" && this.skipLF) {
        this.skipLF = false;
        continue;
      }
      this.skipLF = c === "\r";
      const n = c.codePointAt(0);
      text +=
        c === "\r"
          ? "\n"
          : (n < 32 && c !== "\n" && c !== "\t") || n === 127
            ? "[0x" + n.toString(16).padStart(2, "0") + "]"
            : c;
    }
    this.append(text);
    this.cursor = { boot: v.boot, epoch: v.epoch, end: v.end };
    return bytes.length;
  }
}
export function commandBody(text, suffix = "none", format = "text") {
  const endings = { none: "", cr: "\r", lf: "\n", crlf: "\r\n" };
  if (!(suffix in endings)) throw new Error("Choose a command suffix");
  let content;
  if (format === "hex") {
    const hex = text.replace(/\s/g, "");
    if (!/^(?:[0-9a-fA-F]{2})*$/.test(hex))
      throw new Error("Enter complete hex byte pairs, for example 00 FF 1B.");
    content = Uint8Array.from(hex.match(/../g) || [], (v) => parseInt(v, 16));
  } else if (format === "text") content = encoder.encode(text);
  else throw new Error("Choose text or hex.");
  const bytes = new Uint8Array([...content, ...encoder.encode(endings[suffix])]);
  if (!bytes.length || bytes.length > 256)
    throw new Error("Send 1–256 bytes including the line ending.");
  return new URLSearchParams({ hex: hex(bytes) });
}
export class CommandHistory {
  constructor() {
    this.items = [];
    this.index = 0;
    this.draft = "";
  }
  sent(text) {
    if (text && this.items.at(-1) !== text) this.items.push(text);
    if (this.items.length > 100) this.items.shift();
    this.index = this.items.length;
    this.draft = "";
  }
  move(direction, current) {
    if (this.index === this.items.length) this.draft = current;
    this.index = Math.max(0, Math.min(this.items.length, this.index + direction));
    return this.index === this.items.length ? this.draft : this.items[this.index];
  }
}
export function validateUART(v) {
  const errors = {};
  if (!/^\d+$/.test(v.baud) || Number(v.baud) < 300 || Number(v.baud) > 1000000)
    errors.baud = "Use 300–1,000,000 baud.";
  if (!["5", "6", "7", "8"].includes(v.data_bits)) errors.data_bits = "Choose 5–8 data bits.";
  if (!["N", "O", "E"].includes(v.parity)) errors.parity = "Choose None, Odd or Even parity.";
  if (!["1", "1.5", "2"].includes(v.stop_bits))
    errors.stop_bits = "Choose one, one and a half, or two stop bits.";
  return errors;
}

// A retained emulator resumes only unread bytes. A new/truncated view starts
// after the oldest partial line and any orphaned UTF-8 continuation bytes.
export function terminalReplay(value, cursor) {
  const bytes = decodeCapture(value),
    reset = !cursor || cursor.boot !== value.boot || cursor.epoch !== value.epoch;
  const unread = reset ? 0 : (value.end - cursor.end) >>> 0;
  if (!reset && unread <= bytes.length)
    return { bytes: bytes.slice(bytes.length - unread), reset: false, gap: false };
  let start = value.start ? bytes.indexOf(10) + 1 : 0;
  while (start < bytes.length && (bytes[start] & 192) === 128) start++;
  return { bytes: bytes.slice(start), reset, gap: !reset };
}
