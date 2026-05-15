// background.js -- Print Wizard service worker
// Pure ASCII source. No top-level await. No optional chaining on storage.

console.log("[PrintWizard] service worker starting...");

const DEFAULTS = {
  ssoHost: "ssolite.local",
  ssoPort: 80,
  ssoFallbackHost: "",
  enableWizard: true
};

const VERSION_JSON_URL = "https://raw.githubusercontent.com/KellyPrintCo/SmartSpool/main/version.json";
const MIN_REQUIRED_FW  = "1.3.0";

function getSettings() {
  return chrome.storage.sync.get(DEFAULTS).then(function (stored) {
    return Object.assign({}, DEFAULTS, stored);
  });
}

function buildUrl(host, port, path) {
  var clean = String(host || "").replace(/^https?:\/\//, "").replace(/\/$/, "");
  return "http://" + clean + ":" + port + path;
}

function fetchOnce(url, init, timeoutMs) {
  var ctrl = new AbortController();
  var t = setTimeout(function () { ctrl.abort(); }, timeoutMs || 6000);
  var opts = Object.assign({}, init || {}, { signal: ctrl.signal });
  return fetch(url, opts).then(function (r) {
    return r.text().then(function (text) {
      var data;
      try { data = JSON.parse(text); } catch (e) { data = text; }
      return { ok: r.ok, status: r.status, data: data, url: url };
    });
  }).catch(function (e) {
    return { ok: false, status: 0, error: (e && e.message) || String(e), url: url };
  }).finally(function () {
    clearTimeout(t);
  });
}

function ssoFetch(path, init) {
  return getSettings().then(function (s) {
    var primary = buildUrl(s.ssoHost, s.ssoPort, path);
    return fetchOnce(primary, init).then(function (r1) {
      if (r1.ok) return r1;
      if (s.ssoFallbackHost && s.ssoFallbackHost !== s.ssoHost) {
        var fb = buildUrl(s.ssoFallbackHost, s.ssoPort, path);
        return fetchOnce(fb, init).then(function (r2) {
          if (r2.ok) return r2;
          return {
            ok: false,
            status: r2.status,
            error: "Tried " + primary + " and " + fb + ": " + (r2.error || r1.error || "no response"),
            url: fb
          };
        });
      }
      return {
        ok: false,
        status: r1.status,
        error: (r1.error || "HTTP " + r1.status) + " (tried " + primary + ")",
        url: primary
      };
    });
  });
}

function isFwNewer(latest, current) {
  function p(s) { return String(s || "0.0.0").split(".").map(Number); }
  var a = p(latest), b = p(current);
  if (a[0] !== b[0]) return a[0] > b[0];
  if (a[1] !== b[1]) return a[1] > b[1];
  return a[2] > b[2];
}

function checkForUpdate() {
  return fetch(VERSION_JSON_URL + "?t=" + Date.now(), { cache: "no-store" }).then(function (r) {
    if (!r.ok) {
      if (r.status === 404) {
        return { ok: false, error: "Update manifest not found on GitHub (404). The file 'version.json' is not at " + VERSION_JSON_URL + ". Check that it is at the repository root on the 'main' branch and that the repo is public." };
      }
      return { ok: false, error: "Update server returned HTTP " + r.status };
    }
    return r.json().then(function (manifest) {
      return ssoFetch("/api/health").then(function (health) {
        if (!health.ok) {
          return { ok: false, error: "Could not reach SmartSpool: " + (health.error || ("HTTP " + health.status)) };
        }
        var currentFw = (health.data && health.data.fw) || "0.0.0";
        return {
          ok: true,
          data: {
            currentFw: currentFw,
            latestFw: manifest.version,
            updateAvailable: isFwNewer(manifest.version, currentFw),
            required: !isFwNewer(currentFw, MIN_REQUIRED_FW) && MIN_REQUIRED_FW !== "0.0.0",
            firmwareUrl: manifest.firmware_url,
            releaseNotes: manifest.release_notes || "",
            minCompatible: manifest.min_compatible || "0.0.0"
          }
        };
      });
    });
  }).catch(function (e) {
    return { ok: false, error: (e && e.message) || String(e) };
  });
}

function performUpdate(firmwareUrl) {
  if (!firmwareUrl) return Promise.resolve({ ok: false, error: "No firmwareUrl provided" });
  return fetch(firmwareUrl, { redirect: "follow" }).then(function (binRes) {
    if (!binRes.ok) return { ok: false, error: "Firmware download failed: HTTP " + binRes.status };
    var ct = binRes.headers.get("content-type") || "";
    if (ct.indexOf("text/html") !== -1) {
      return { ok: false, error: "Firmware URL returned HTML. Check the GitHub release URL." };
    }
    return binRes.blob().then(function (blob) {
      if (blob.size < 100000) {
        return { ok: false, error: "Downloaded file too small (" + blob.size + " bytes). Not a valid firmware binary." };
      }
      return getSettings().then(function (s) {
        var fd = new FormData();
        fd.append("firmware", blob, "firmware.bin");

        function tryUpload(host) {
          var u = buildUrl(host, s.ssoPort, "/api/update");
          return fetch(u, { method: "POST", body: fd }).then(function (res) {
            if (res.ok) return { ok: true, url: u };
            return res.json().catch(function () { return {}; }).then(function (j) {
              return { ok: false, url: u, error: j.error || ("HTTP " + res.status) };
            });
          }).catch(function (e) {
            return { ok: false, url: u, error: (e && e.message) || String(e) };
          });
        }

        return tryUpload(s.ssoHost).then(function (up1) {
          if (up1.ok) return { ok: true };
          if (s.ssoFallbackHost && s.ssoFallbackHost !== s.ssoHost) {
            return tryUpload(s.ssoFallbackHost).then(function (up2) {
              if (up2.ok) return { ok: true };
              return { ok: false, error: "Upload failed: " + up2.error + " (tried " + up2.url + ")" };
            });
          }
          return { ok: false, error: "Upload failed: " + up1.error + " (tried " + up1.url + ")" };
        });
      });
    });
  }).catch(function (e) {
    return { ok: false, error: (e && e.message) || String(e) };
  });
}

function pollReconnect() {
  function attempt(i) {
    if (i >= 24) return Promise.resolve({ ok: false, error: "Device did not come back online." });
    return new Promise(function (r) { setTimeout(r, 2500); }).then(function () {
      return ssoFetch("/api/health").then(function (h) {
        if (h && h.ok && h.data && h.data.ok) return { ok: true, fw: h.data.fw || "?" };
        return attempt(i + 1);
      }).catch(function () { return attempt(i + 1); });
    });
  }
  return attempt(0);
}

chrome.runtime.onMessage.addListener(function (msg, _sender, sendResponse) {
  var t = (msg && msg.type) || "";
  var promise;
  switch (t) {
    case "ping":            promise = Promise.resolve({ ok: true, pong: true }); break;
    case "sso:health":      promise = ssoFetch("/api/health"); break;
    case "sso:status":      promise = ssoFetch("/api/status"); break;
    case "sso:profiles":    promise = ssoFetch("/api/profiles"); break;
    case "sso:assign":      promise = ssoFetch("/api/assign",       { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(msg.payload || (msg.name ? { name: msg.name } : {})) }); break;
    case "sso:queueWrite":  promise = ssoFetch("/api/write",        { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(msg.payload || (msg.name ? { name: msg.name } : {})) }); break;
    case "sso:cancelWrite": promise = ssoFetch("/api/write/cancel", { method: "POST" }); break;
    case "sso:toggleRewrite": promise = ssoFetch("/api/rewrite",        { method: "POST" }); break;
    case "sso:cancelRewrite": promise = ssoFetch("/api/rewrite/cancel", { method: "POST" }); break;
    case "sso:checkUpdate":   promise = checkForUpdate(); break;
    case "sso:performUpdate": promise = performUpdate(msg.firmwareUrl); break;
    case "sso:pollReconnect": promise = pollReconnect(); break;
    case "settings:get":      promise = getSettings().then(function (d) { return { ok: true, data: d }; }); break;
    case "settings:set":      promise = chrome.storage.sync.set(msg.values || {}).then(function () { return { ok: true }; }); break;
    default:                  promise = Promise.resolve({ ok: false, error: "Unknown message type: " + t }); break;
  }
  promise.then(sendResponse).catch(function (e) {
    sendResponse({ ok: false, error: (e && e.message) || String(e) });
  });
  return true; // keep channel open for async sendResponse
});

chrome.runtime.onInstalled.addListener(function () {
  console.log("[PrintWizard] onInstalled fired");
  chrome.storage.sync.get(DEFAULTS).then(function (cur) {
    if (!cur.ssoHost) chrome.storage.sync.set(DEFAULTS);
  });
});

console.log("[PrintWizard] service worker ready.");
