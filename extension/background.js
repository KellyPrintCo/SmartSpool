/* background.js - service worker for the Print Wizard.
 *
 * Centralises all HTTP traffic to SSO-Lite and handles firmware OTA updates.
 */

const DEFAULTS = {
  ssoHost: "ssolite.local",
  ssoPort: 80,
  enableWizard: true,
};

/* URL of the version manifest on GitHub.
 * Update this constant to point at your own repo after forking. */
const VERSION_JSON_URL =
  "https://raw.githubusercontent.com/KellyPrintCo/SmartSpool/main/version.json";

/* Minimum firmware version the extension requires.
 * If the device reports a version older than this, the update step is REQUIRED
 * (user cannot skip). Set to "0.0.0" to always treat updates as optional. */
const MIN_REQUIRED_FW = "1.3.0";

async function getSettings() {
  const stored = await chrome.storage.sync.get(DEFAULTS);
  return { ...DEFAULTS, ...stored };
}

async function ssoUrl(path) {
  const s = await getSettings();
  const host = s.ssoHost.replace(/^https?:\/\//, "").replace(/\/$/, "");
  return `http://${host}:${s.ssoPort}${path}`;
}

async function ssoFetch(path, init = {}) {
  const url = await ssoUrl(path);
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 6000);
  try {
    const r = await fetch(url, { ...init, signal: ctrl.signal });
    const text = await r.text();
    let data;
    try { data = JSON.parse(text); } catch { data = text; }
    return { ok: r.ok, status: r.status, data };
  } catch (e) {
    return { ok: false, status: 0, error: e.message || String(e) };
  } finally {
    clearTimeout(timer);
  }
}

/* Content script and wizard talk to SSO-Lite through us via messages. */
chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
  (async () => {
    try {
      switch (msg.type) {
        case "sso:health":
          sendResponse(await ssoFetch("/api/health"));
          break;
        case "sso:status":
          sendResponse(await ssoFetch("/api/status"));
          break;
        case "sso:profiles":
          sendResponse(await ssoFetch("/api/profiles"));
          break;
        case "sso:assign":
          sendResponse(await ssoFetch("/api/assign", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ name: msg.name }),
          }));
          break;
        case "sso:queueWrite":
          sendResponse(await ssoFetch("/api/write", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ name: msg.name }),
          }));
          break;
        case "sso:cancelWrite":
          sendResponse(await ssoFetch("/api/write/cancel", { method: "POST" }));
          break;
        case "sso:toggleRewrite":
          sendResponse(await ssoFetch("/api/rewrite", { method: "POST" }));
          break;
        case "sso:cancelRewrite":
          sendResponse(await ssoFetch("/api/rewrite/cancel", { method: "POST" }));
          break;

        /* ---- Firmware update ---- */
        case "sso:checkUpdate":
          sendResponse(await checkForUpdate());
          break;
        case "sso:performUpdate":
          /* Long-running — downloads from GitHub, uploads to device.
           * Returns {ok, error} when fully done; connection closes as device reboots. */
          sendResponse(await performUpdate(msg.firmwareUrl));
          break;
        case "sso:pollReconnect":
          /* Poll /api/health until the device is back (after reboot).
           * Returns {ok, fw} when online, {ok:false} after timeout. */
          sendResponse(await pollReconnect(msg.expectedVersion));
          break;

        case "settings:get":
          sendResponse({ ok: true, data: await getSettings() });
          break;
        case "settings:set":
          await chrome.storage.sync.set(msg.values || {});
          sendResponse({ ok: true });
          break;
        default:
          sendResponse({ ok: false, error: "unknown message: " + msg.type });
      }
    } catch (e) {
      sendResponse({ ok: false, error: e.message || String(e) });
    }
  })();
  return true;   // keep the channel open for async sendResponse
});

chrome.runtime.onInstalled.addListener(async () => {
  const cur = await chrome.storage.sync.get(DEFAULTS);
  if (!cur.ssoHost) await chrome.storage.sync.set(DEFAULTS);
});

/* =========================================================================
   FIRMWARE UPDATE HELPERS
   ========================================================================= */

/* Compare semver strings.  Returns true if `latest` is newer than `current`. */
function isFwNewer(latest, current) {
  const p = s => (s || "0.0.0").split(".").map(Number);
  const [la, lb, lc] = p(latest);
  const [ca, cb, cc] = p(current);
  return la > ca || (la === ca && lb > cb) || (la === ca && lb === cb && lc > cc);
}

async function checkForUpdate() {
  try {
    /* Fetch the version manifest, bypassing cache. */
    const r = await fetch(VERSION_JSON_URL + "?t=" + Date.now());
    if (!r.ok) throw new Error(`version.json fetch failed: ${r.status}`);
    const manifest = await r.json();

    /* Get current device firmware version. */
    const health = await ssoFetch("/api/health");
    const currentFw = health?.data?.fw || "0.0.0";

    const updateAvailable = isFwNewer(manifest.version, currentFw);
    const required = !isFwNewer(currentFw, MIN_REQUIRED_FW) && MIN_REQUIRED_FW !== "0.0.0";

    return {
      ok: true,
      data: {
        currentFw,
        latestFw:       manifest.version,
        updateAvailable,
        required,                         // true  = must update, cannot skip
        firmwareUrl:    manifest.firmware_url,
        releaseNotes:   manifest.release_notes || "",
        minCompatible:  manifest.min_compatible || "0.0.0",
      },
    };
  } catch (e) {
    return { ok: false, error: e.message || String(e) };
  }
}

async function performUpdate(firmwareUrl) {
  try {
    if (!firmwareUrl) throw new Error("no firmwareUrl provided");

    /* 1. Download firmware binary from GitHub. */
    const binRes = await fetch(firmwareUrl, { redirect: "follow" });
    if (!binRes.ok) throw new Error(`firmware download failed: ${binRes.status}`);
    const contentType = binRes.headers.get("content-type") || "";
    /* GitHub may redirect to CDN — accept binary or octet-stream. */
    if (contentType.includes("text/html")) {
      throw new Error("Firmware URL returned HTML — check your GitHub release URL.");
    }
    const blob = await binRes.blob();
    if (blob.size < 100000) {
      throw new Error(`Downloaded file is too small (${blob.size} bytes) — not a valid firmware binary.`);
    }

    /* 2. POST to device as multipart/form-data. */
    const baseUrl = await ssoUrl("/api/update");
    const fd = new FormData();
    fd.append("firmware", blob, "firmware.bin");

    const upRes = await fetch(baseUrl, { method: "POST", body: fd });
    if (!upRes.ok) {
      const errJson = await upRes.json().catch(() => ({}));
      throw new Error(errJson.error || `upload failed: ${upRes.status}`);
    }
    /* Device is now rebooting — the response has been sent before restart. */
    return { ok: true };
  } catch (e) {
    return { ok: false, error: e.message || String(e) };
  }
}

async function pollReconnect(expectedVersion, maxAttempts = 20, intervalMs = 2500) {
  for (let i = 0; i < maxAttempts; i++) {
    await new Promise(r => setTimeout(r, intervalMs));
    try {
      const r = await ssoFetch("/api/health");
      if (r?.ok && r.data?.ok) {
        const newFw = r.data.fw || "?";
        /* Verify the version actually updated if we know what to expect. */
        if (expectedVersion && newFw !== expectedVersion) {
          console.warn(`[OTA] device is back but fw=${newFw}, expected ${expectedVersion}`);
        }
        return { ok: true, fw: newFw };
      }
    } catch { /* device still rebooting */ }
  }
  return { ok: false, error: "Device did not come back online within the expected time." };
}
