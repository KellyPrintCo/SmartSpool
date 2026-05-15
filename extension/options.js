// options.js -- Print Wizard Settings page
// Pure ASCII source. Uses DOMContentLoaded to ensure DOM is ready.

function $(s) { return document.querySelector(s); }

function sendBg(msg) {
  return new Promise(function (resolve) {
    try {
      chrome.runtime.sendMessage(msg, function (response) {
        var err = chrome.runtime.lastError;
        if (err) {
          resolve({ ok: false, error: "Background script unreachable: " + err.message });
          return;
        }
        resolve(response || { ok: false, error: "No response from background script." });
      });
    } catch (e) {
      resolve({ ok: false, error: "sendMessage failed: " + ((e && e.message) || String(e)) });
    }
  });
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, function (c) {
    return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
  });
}

function showMsg(text, kind) {
  var el = $("#msg");
  if (!el) return;
  el.textContent = text;
  el.style.color = kind === "err" ? "var(--err)" : "var(--ok)";
}

function readValues() {
  return {
    ssoHost: $("#ssoHost").value.trim() || "ssolite.local",
    ssoFallbackHost: $("#ssoFallbackHost").value.trim(),
    ssoPort: parseInt($("#ssoPort").value, 10) || 80
  };
}

function load() {
  return sendBg({ type: "settings:get" }).then(function (r) {
    if (r && r.ok && r.data) {
      $("#ssoHost").value         = r.data.ssoHost || "ssolite.local";
      $("#ssoFallbackHost").value = r.data.ssoFallbackHost || "";
      $("#ssoPort").value         = r.data.ssoPort || 80;
    } else if (r && r.error) {
      showMsg("Cannot load settings: " + r.error, "err");
    }
  });
}

function onSave() {
  var values = readValues();
  sendBg({ type: "settings:set", values: values }).then(function (r) {
    if (r && r.ok) {
      showMsg("Saved.");
    } else {
      showMsg("Failed to save: " + ((r && r.error) || "unknown error"), "err");
    }
  });
}

function onTest() {
  var values = readValues();
  sendBg({ type: "settings:set", values: values }).then(function (saveRes) {
    if (!saveRes || !saveRes.ok) {
      showMsg("Could not save settings before test: " + ((saveRes && saveRes.error) || "unknown"), "err");
      return;
    }
    showMsg("Testing...", "");
    var resultBox = $("#testResult");
    if (resultBox) resultBox.innerHTML = "";

    sendBg({ type: "sso:health" }).then(function (r) {
      if (r && r.ok && r.data && r.data.ok) {
        showMsg("Connected: " + r.data.name + " v" + r.data.fw);
        if (resultBox) {
          resultBox.innerHTML =
            '<div style="background:#0d1117;border-left:3px solid var(--ok);padding:10px 14px;border-radius:6px;font-size:.9em">' +
              '<strong>Success.</strong> Connected to <code>' + escapeHtml(r.url || "") + '</code><br>' +
              'You can close this page. Print Wizard is ready to use.' +
            '</div>';
        }
      } else {
        showMsg("Failed -- see details below", "err");
        var reason = (r && r.error) || ("HTTP " + ((r && r.status) || "?"));
        if (resultBox) {
          resultBox.innerHTML =
            '<div style="background:#0d1117;border-left:3px solid var(--err);padding:10px 14px;border-radius:6px;font-size:.9em">' +
              '<strong>Could not reach SmartSpool.</strong><br>' +
              '<span style="color:var(--mut)">' + escapeHtml(reason) + '</span><br><br>' +
              '<strong>Troubleshooting:</strong>' +
              '<ol style="margin:6px 0 0 18px;padding:0">' +
                '<li>Is SmartSpool powered on?</li>' +
                '<li>Are your computer and SmartSpool on the same Wi-Fi?</li>' +
                '<li>Try opening <code>http://' + escapeHtml(values.ssoHost) + '/</code> in a new tab.</li>' +
                '<li>If not, find SmartSpool\'s IP on its dashboard, enter it in Fallback IP, then Test again.</li>' +
              '</ol>' +
            '</div>';
        }
      }
    });
  });
}

document.addEventListener("DOMContentLoaded", function () {
  load();
  var saveBtn = $("#saveBtn");
  var testBtn = $("#testBtn");
  if (saveBtn) saveBtn.addEventListener("click", onSave);
  if (testBtn) testBtn.addEventListener("click", onTest);
});
