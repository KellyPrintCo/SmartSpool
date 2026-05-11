const $ = s => document.querySelector(s);

async function load() {
  const r = await new Promise(res =>
    chrome.runtime.sendMessage({ type: "settings:get" }, res));
  if (r?.data) {
    $("#ssoHost").value = r.data.ssoHost || "ssolite.local";
    $("#ssoPort").value = r.data.ssoPort || 80;
  }
}

$("#saveBtn").onclick = async () => {
  const values = {
    ssoHost: $("#ssoHost").value.trim() || "ssolite.local",
    ssoPort: parseInt($("#ssoPort").value, 10) || 80,
  };
  await new Promise(res =>
    chrome.runtime.sendMessage({ type: "settings:set", values }, res));
  show("Saved.");
};

$("#testBtn").onclick = async () => {
  const values = {
    ssoHost: $("#ssoHost").value.trim() || "ssolite.local",
    ssoPort: parseInt($("#ssoPort").value, 10) || 80,
  };
  // save first so background uses these
  await new Promise(res =>
    chrome.runtime.sendMessage({ type: "settings:set", values }, res));
  show("Testing…", "");
  const r = await new Promise(res =>
    chrome.runtime.sendMessage({ type: "sso:health" }, res));
  if (r?.ok && r.data?.ok) show(`Connected (v${r.data.fw}).`);
  else show("Failed: " + (r?.error || "no response"), "err");
};

function show(text, kind = "ok") {
  const el = $("#msg");
  el.textContent = text;
  el.style.color = kind === "err" ? "var(--err)" : "var(--ok)";
}

load();
