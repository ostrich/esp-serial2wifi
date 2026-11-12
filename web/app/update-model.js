import { hex } from "./core.js";
const encoder = new TextEncoder();
export function validFirmware(v) {
  return (
    v?.schema === 1 &&
    typeof v.version === "string" &&
    typeof v.sdk === "string" &&
    typeof v.image_abi === "string" &&
    Number.isSafeInteger(v.flash_bytes) &&
    typeof v.ota_supported === "boolean" &&
    (v.external_ui === undefined || typeof v.external_ui === "boolean") &&
    (v.active_slot === null || [1, 2].includes(v.active_slot)) &&
    (v.target_slot === null || [1, 2].includes(v.target_slot)) &&
    Number.isSafeInteger(v.max_image_bytes) &&
    (v.reason === null || typeof v.reason === "string")
  );
}
export function validPages(v) {
  return (
    v?.schema === 1 &&
    v.supported === true &&
    Number.isSafeInteger(v.capacity_bytes) &&
    v.capacity_bytes >= 0 &&
    Number.isSafeInteger(v.html_prefix_bytes) &&
    v.html_prefix_bytes >= 0 &&
    typeof v.valid === "boolean" &&
    (v.used_bytes === null || Number.isSafeInteger(v.used_bytes)) &&
    Number.isSafeInteger(v.count) &&
    Number.isSafeInteger(v.index) &&
    (v.next === null || (Number.isSafeInteger(v.next) && v.next > v.index)) &&
    Array.isArray(v.files) &&
    v.files.length <= 1 &&
    v.files.every(
      (f) => typeof f.name === "string" && Number.isSafeInteger(f.bytes) && f.bytes >= 0
    )
  );
}
export function checkFirmware(bytes, info) {
  if (info.external_ui) {
    if (
      !(bytes instanceof Uint8Array) ||
      bytes.length < 1024 ||
      bytes.length > info.max_image_bytes ||
      bytes.length % 1024
    )
      throw new Error("Choose a complete firmware/UI update package (.elu).");
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength),
      magic = new TextDecoder().decode(bytes.subarray(0, 8)),
      app = view.getUint32(8, true),
      ui = view.getUint32(12, true),
      slot = view.getUint32(16, true),
      uiAt = 1024 + Math.ceil(app / 1024) * 1024;
    if (
      magic !== "ELUPD001" ||
      slot !== info.target_slot - 1 ||
      app < 1024 ||
      app > 503808 ||
      ui < 16 ||
      ui > 258048 ||
      ui % 4 ||
      bytes.length !== uiAt + Math.ceil(ui / 1024) * 1024 ||
      bytes.subarray(52, 1024).some(Boolean) ||
      bytes.subarray(1024 + app, uiAt).some(Boolean) ||
      bytes.subarray(uiAt + ui).some(Boolean)
    )
      throw new Error("The update package is incomplete or is for another slot.");
    checkFirmware(bytes.subarray(1024, 1024 + app), {
      ...info,
      external_ui: false,
      max_image_bytes: 503808
    });
    const digest = hex(bytes.subarray(20, 52));
    if (
      !new TextDecoder().decode(bytes.subarray(1024, 1024 + app)).includes("ELUI-SHA256:" + digest)
    )
      throw new Error("The firmware and web interface do not match.");
    return { target: info.target_slot, bytes: bytes.length };
  }
  if (
    !(bytes instanceof Uint8Array) ||
    bytes.length < 1024 ||
    bytes.length > info.max_image_bytes ||
    (bytes.length & 15) !== 4
  )
    throw new Error("Choose a complete application image within the target slot size.");
  if (bytes[0] !== 0xea || bytes[1] !== 4 || bytes[2] !== 0 || bytes[3] !== info.target_slot - 1)
    throw new Error(
      "This image is for the wrong firmware slot. Choose the image for the target slot shown above."
    );
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength),
    entry = view.getUint32(4, true),
    irom = view.getUint32(12, true);
  if (
    entry < 0x40100000 ||
    entry >= 0x40108000 ||
    entry & 3 ||
    view.getUint32(8, true) !== 0 ||
    irom < 16 ||
    irom & 15 ||
    irom > bytes.length - 64
  )
    throw new Error("Unsupported application image header.");
  const marker = encoder.encode(info.image_abi);
  let found = false;
  outer: for (let i = 0; i + marker.length <= bytes.length; i++) {
    for (let j = 0; j < marker.length; j++) if (bytes[i + j] !== marker[j]) continue outer;
    found = true;
    break;
  }
  if (!found)
    throw new Error(
      "This image doesn’t match the device’s SDK and flash layout. Use Serial recovery to install it over USB."
    );
  return { target: info.target_slot, bytes: bytes.length };
}
const aligned = (n) => (n + 3) & ~3;
export function checkPages(files, info) {
  if (!files.length) throw new Error("Select at least one custom-page file.");
  let bytes = 16;
  const names = new Set();
  for (const file of files) {
    const size = encoder.encode(file.name).length;
    if (
      !size ||
      size > 255 ||
      /[\x00-\x1f\x7f"\\/]/.test(file.name) ||
      [".", ".."].includes(file.name)
    )
      throw new Error(
        "Use filenames of at most 255 UTF-8 bytes, without quotes, controls or path separators."
      );
    if (names.has(file.name)) throw new Error("Choose unique filenames.");
    names.add(file.name);
    bytes +=
      16 +
      aligned(size + 1) +
      aligned(file.size + (file.name.endsWith(".html") ? info.html_prefix_bytes : 0));
    if (!Number.isSafeInteger(bytes) || bytes > info.capacity_bytes)
      throw new Error("Files and HTML wrappers exceed custom-page capacity.");
  }
  return { bytes, count: files.length };
}
export function bootOutcome(status, before, target, expectedVersion) {
  const slot = status.firmware.slot,
    actual = slot === "user1" ? 1 : slot === "user2" ? 2 : null;
  if (actual !== target || actual === before)
    return {
      state: "waiting",
      message: "ESP Serial2WiFi is responding, but the new firmware hasn’t started yet."
    };
  if (expectedVersion && status.firmware.version !== expectedVersion)
    return {
      state: "mismatch",
      message: "The new slot is running a different version than expected."
    };
  return expectedVersion
    ? { state: "verified", message: "Update complete. The expected firmware version is running." }
    : { state: "unverified", message: "The new slot is running. Check the version shown above." };
}
