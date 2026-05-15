/* wizard.js - the wizard's brain.
 *
 * Lives inside an iframe. Talks to the parent (content.js) via
 * postMessage to get page metadata and to trigger Studio opens.
 * Talks to SSO-Lite by sending sso:* messages to the extension's
 * background service worker.
 */

const STEPS = [
  "welcome", "connect", "fw-update", "filament-check",
  "filament-pick", "tag", "plate", "prep", "send", "done"
];

const state = {
  meta: { title: "Loading…", materials: [], plate: null, image: null },
  sso: { connected: false, host: null },
  status: null,            // last /api/status payload
  profiles: [],            // last /api/profiles payload
  pickedProfile: null,     // FilamentProfile chosen for write
  needsSwap: false,
  step: "welcome",
  fwInfo: null,            // last checkUpdate result
};

const $ = s => document.querySelector(s);
const $$ = s => Array.from(document.querySelectorAll(s));

/* ---------- step routing ------------------------------------------ */
function show(stepName) {
  state.step = stepName;
  $$(".step").forEach(s => s.classList.toggle("active", s.dataset.step === stepName));
  // Hide 'connect' and 'fw-update' from visible count once established/resolved.
  const visible = STEPS.filter(s =>
    !(s === "connect"   && state.sso.connected) &&
    !(s === "fw-update" && (!state.fwInfo?.updateAvailable))
  );
  const idx = visible.indexOf(stepName);
  const pct = idx < 0 ? 0 : Math.round(((idx + 1) / visible.length) * 100);
  $("#bar-fill").style.width = pct + "%";
  $("#step-label").textContent = idx < 0 ? "" : `Step ${idx + 1} of ${visible.length}`;
  document.getElementById("progress").style.opacity = (stepName === "welcome" ? .5 : 1);
  // run per-step entry hooks
  if (ENTER[stepName]) ENTER[stepName]();
}

const ENTER = {
  "welcome":         () => { /* nothing */ },
  "connect":         async () => { await tryConnect(); },
  "fw-update":       async () => { await enterFwUpdate(); },
  "filament-check":  async () => { await renderFilamentCheck(); },
  "filament-pick":   async () => { await renderProfileGrid(); },
  "tag":             async () => { await beginTagWrite(); },
  "plate":           ()       => { renderPlateStep(); },
  "prep":            ()       => { /* checkboxes handled by listener */ },
  "send":            ()       => { /* nothing */ },
  "done":            ()       => { /* nothing */ },
};

/* ---------- bootstrap --------------------------------------------- */
window.addEventListener("DOMContentLoaded", () => {
  hookButtons();
  // Ask the parent for metadata. content.js also pushes it on init.
  parent.postMessage({ type: "wizard:requestMeta" }, "*");
  // Step 1 immediately. We'll move to 'connect' on first action.
  show("welcome");
});

window.addEventListener("message", ev => {
  const m = ev.data || {};
  if (m.type === "wizard:init" && m.meta) {
    state.meta = { ...state.meta, ...m.meta };
    renderWelcome();
  }
  if (m.type === "wizard:studioUrlFound" && m.url) {
    /* inject.js captured a bambustudio*:// URL on the page. Clear the
     * capture-timeout, launch via our hidden iframe (no UI flash), and
     * show the confirmation panel. */
    console.log("[mw-wiz wizard] late-bound studio URL:", m.url);
    if (state.captureTimer) { clearTimeout(state.captureTimer); state.captureTimer = null; }
    state.meta.studioUrl = m.url;
    const status = $("#studioStatus");
    if (status) {
      status.textContent = "Opening Bambu Studio…";
      status.className   = "status ok";
      status.hidden      = false;
    }
    const link = $("#manualStudioLink");
    link.href = m.url;
    link.textContent = m.url.length > 80 ? m.url.slice(0, 77) + "…" : m.url;
    launchProtocol(m.url);
    $("#studioConfirm").hidden = false;
    $("#studioHelp").hidden = true;
    $("#studioMissing").hidden = true;
  }
  if (m.type === "wizard:studioLinkMissing") {
    document.getElementById("studioMissing").hidden = false;
    document.getElementById("studioConfirm").hidden = true;
  }
});

function hookButtons() {
  $("#close").onclick = () => parent.postMessage({ type: "wizard:close" }, "*");
  $$("[data-go]").forEach(b => b.addEventListener("click", () => {
    const next = b.dataset.go;
    // If the user clicks "Let's go" from welcome and we haven't checked
    // SSO-Lite yet, route through 'connect' first.
    if (next === "filament-check" && !state.sso.connected) {
      show("connect"); return;
    }
    show(next);
  }));
  $('[data-action="openSettings"]').onclick = () =>
    chrome.runtime.sendMessage({ type: "settings:get" }, ({ data }) => {
      const cur = data?.ssoHost || "ssolite.local";
      const next = prompt("SSO-Lite address (hostname or IP):", cur);
      if (next && next.trim() && next.trim() !== cur) {
        chrome.runtime.sendMessage(
          { type: "settings:set", values: { ssoHost: next.trim() } },
          () => tryConnect());
      }
    });
  $('[data-action="retryConnect"]').onclick = () => tryConnect();

  /* The Continue button on the connect step used to use inline onclick=,
   * which violates MV3 CSP. Bind it programmatically here instead. */
  $("#connectContinue").addEventListener("click", () => advanceFromConnect());

  $("#cancelWriteBtn").onclick = () =>
    chrome.runtime.sendMessage({ type: "sso:cancelWrite" });

  $("#plateOk").onchange = e =>
    $("#plateContinueBtn").disabled = !e.target.checked;

  $$(".prep-chk").forEach(c => c.addEventListener("change", () => {
    const all = $$(".prep-chk").every(x => x.checked);
    $("#prepContinueBtn").disabled = !all;
  }));

  $("#openStudioBtn").onclick = () => attemptOpenStudio();

  $("#studioFailedBtn").onclick = () => {
    const help = $("#studioHelp");
    help.hidden = false;
    help.open = true;
    $("#studioConfirm").hidden = true;
  };

  $("#finishBtn").onclick = () =>
    parent.postMessage({ type: "wizard:close" }, "*");
}

/* Launch Bambu Studio via the bambustudio:// deeplink scraped from the
 * MakerWorld page. Uses a hidden iframe rather than navigating the
 * current window — this is the standard pattern for protocol launches
 * and avoids the "site can't be reached" flash that direct navigation
 * sometimes shows when the OS hands off to the protocol handler.
 *
 * If we don't have a URL (rare — MakerWorld renders one on every model
 * page), fall back to asking the parent to click whatever it can find. */
function attemptOpenStudio() {
  const url = state.meta.studioUrl;
  console.log("[mw-wiz] attemptOpenStudio url=", url);
  $("#studioConfirm").hidden = false;
  $("#studioHelp").hidden  = true;
  $("#studioMissing").hidden = true;

  if (url) {
    /* Best case: we already scraped a static deeplink from the page.
     * Launch via hidden iframe directly. */
    const link = $("#manualStudioLink");
    link.href = url;
    link.textContent = url.length > 80 ? url.slice(0, 77) + "…" : url;
    launchProtocol(url);
    return;
  }

  /* No static URL on the page. Ask the content script to programmatically
   * click MakerWorld's own "Open in Bambu Studio" button. inject.js will
   * capture the resulting bambustudioopen:// URL and forward it back to
   * us, at which point we relaunch via hidden iframe.
   *
   * The user never sees MakerWorld's modal or has to do anything — the
   * whole thing happens behind our wizard UI. */
  $("#studioConfirm").hidden = true;
  const status = $("#studioStatus");
  if (status) {
    status.textContent = "Opening Bambu Studio…";
    status.className = "status pending";
    status.hidden = false;
  }

  parent.postMessage({ type: "wizard:openInStudio" }, "*");

  /* Timeout in case the click is intercepted or the URL is never captured. */
  if (state.captureTimer) clearTimeout(state.captureTimer);
  state.captureTimer = setTimeout(() => {
    /* If we still haven't seen a capture, show help. */
    if (status) {
      status.textContent = "Couldn't catch the launch automatically.";
      status.className = "status err";
    }
    $("#studioConfirm").hidden = false;
    $("#studioHelp").hidden = false;
    $("#studioHelp").open = true;
    $("#manualStudioLink").textContent = "(no direct link auto-detected)";
    $("#manualStudioLink").removeAttribute("href");
  }, 8000);
}

/* The hidden-iframe protocol-launch pattern: create an off-screen iframe,
 * point it at the custom-protocol URL, the OS catches the navigation and
 * hands off to the registered handler. The iframe never renders any
 * visible content, so there's no error flash even if the handler isn't
 * registered (the failed nav happens in a 0x0 hidden frame). */
function launchProtocol(url) {
  const f = document.createElement("iframe");
  f.style.cssText = "position:absolute;width:0;height:0;border:0;visibility:hidden";
  f.src = url;
  document.body.appendChild(f);
  setTimeout(() => f.remove(), 2500);
}

/* ---------- step 1 / welcome -------------------------------------- */
function renderWelcome() {
  $("#modelTitle").textContent = state.meta.title || "(unknown model)";
  if (state.meta.image) {
    $("#modelImg").src = state.meta.image;
    $("#modelImg").hidden = false;
  }
  const chips = $("#modelMeta");
  chips.innerHTML = "";
  for (const m of state.meta.materials || []) chip(chips, m, "mat");
  if (state.meta.plate) chip(chips, state.meta.plate, "plate");
  if (!chips.children.length) {
    chip(chips, "Couldn't detect material — we'll ask you in a moment", "muted");
  }
}

function chip(into, text, kind = "") {
  const c = document.createElement("span");
  c.className = "chip " + kind;
  c.textContent = text;
  into.appendChild(c);
}

/* ---------- step 2 / connect -------------------------------------- */
async function tryConnect() {
  const el = $("#connectStatus");
  el.className = "status pending";
  el.textContent = "Looking for SSO-Lite…";
  $("#connectContinue").disabled = true;
  $("#connectHelp").style.display = "none";   // hide help while pending

  const r = await sendBg({ type: "sso:health" });
  if (r?.ok && r.data?.ok) {
    state.sso.connected = true;
    el.className = "status ok";
    el.textContent = `Connected (${r.data.name} v${r.data.fw})`;
    $("#connectHelp").style.display = "none";

    // Eagerly check for firmware updates while user reads the status.
    el.textContent += "  ·  Checking for updates…";
    const upd = await sendBg({ type: "sso:checkUpdate" });
    if (upd?.ok) {
      state.fwInfo = upd.data;
      if (upd.data.updateAvailable) {
        el.textContent = `Connected (v${upd.data.currentFw}) — firmware update available (v${upd.data.latestFw})`;
      } else {
        el.textContent = `Connected (${r.data.name} v${r.data.fw}) — up to date`;
      }
    } else {
      // Couldn't reach update server — not a blocking error, just log it
      state.fwInfo = null;
      el.textContent = `Connected (${r.data.name} v${r.data.fw})`;
      console.warn("[Print Wizard] update check failed:", upd?.error);
    }

    $("#connectContinue").disabled = false;
    pollStatus();
    await loadProfiles();
  } else {
    state.sso.connected = false;
    el.className = "status err";
    const reason = r?.error || `HTTP ${r?.status || "?"}`;
    el.textContent = `Couldn't reach SmartSpool: ${reason}`;
    /* Show the troubleshooting box and fill in the URL that was tried so the
     * user can paste it into a new tab to verify connectivity manually. */
    $("#connectHelp").style.display = "";
    if (r?.url) {
      $("#connectTriedUrl").textContent = r.url.replace(/\/api\/health$/, "/");
    }
  }
}

/* Called by the "Continue" button on the connect step.
 * Routes to fw-update if an update is available, otherwise straight to
 * filament-check. */
function advanceFromConnect() {
  if (state.fwInfo?.updateAvailable) {
    show("fw-update");
  } else {
    show("filament-check");
  }
}

/* ---------- fw-update step ---------------------------------------- */
async function enterFwUpdate() {
  const info = state.fwInfo;
  if (!info || !info.updateAvailable) {
    // Nothing to update — skip straight to filament-check.
    show("filament-check");
    return;
  }

  $("#fwCurrent").textContent = "v" + (info.currentFw || "?");
  $("#fwLatest").textContent  = "v" + (info.latestFw  || "?");

  if (info.required) {
    $("#fwUpdateTitle").textContent = "Firmware update required";
    $("#fwUpdateLead").textContent  =
      "Your SmartSpool firmware is too old to work with this version of Print Wizard. " +
      "You need to update before continuing.";
    $("#fwSkipBtn").style.display = "none";
  } else {
    $("#fwUpdateTitle").textContent = "Firmware update available";
    $("#fwUpdateLead").textContent  =
      "A new version of SmartSpool firmware is ready. Updating takes about 20 seconds " +
      "and your printer won't be affected.";
    $("#fwSkipBtn").style.display = "";
  }

  if (info.releaseNotes) {
    const nb = $("#fwReleaseNotes");
    nb.style.display = "";
    nb.innerHTML = `<strong>What's new:</strong> ${escapeHtml(info.releaseNotes)}`;
  }

  // Reset progress UI
  $("#fwProgress").style.display = "none";
  $("#fwError").style.display    = "none";
  $("#fwActions").style.display  = "";
  $("#fwUpdateBtn").disabled     = false;
  $("#fwProgressFill").style.width = "0%";

  $("#fwUpdateBtn").onclick = () => runFwUpdate(info);
}

async function runFwUpdate(info) {
  const btn    = $("#fwUpdateBtn");
  const skipBtn= $("#fwSkipBtn");
  const prog   = $("#fwProgress");
  const fill   = $("#fwProgressFill");
  const phase  = $("#fwPhaseLabel");
  const detail = $("#fwProgressDetail");
  const errBox = $("#fwError");
  const acts   = $("#fwActions");

  btn.disabled      = true;
  skipBtn.style.display = "none";
  prog.style.display = "";
  errBox.style.display = "none";
  acts.style.display = "none";

  const setProgress = (pct, phaseText, detailText = "") => {
    fill.style.width    = pct + "%";
    phase.textContent   = phaseText;
    detail.textContent  = detailText;
  };

  try {
    setProgress(5, "Downloading firmware from server…", `v${info.latestFw} · ${info.firmwareUrl.split("/").pop()}`);

    // Animate download phase (fake progress, real download is in background)
    let dlPct = 5;
    const dlTimer = setInterval(() => {
      dlPct = Math.min(dlPct + 3, 28);
      fill.style.width = dlPct + "%";
    }, 300);

    const upResult = await sendBg({ type: "sso:performUpdate", firmwareUrl: info.firmwareUrl });
    clearInterval(dlTimer);

    if (!upResult?.ok) throw new Error(upResult?.error || "Update failed — no response");

    setProgress(88, "Flashing complete — SmartSpool is rebooting…",
                "This takes about 10-15 seconds. Don't close this window.");

    // Poll for reconnection
    for (let i = 0; i < 24; i++) {
      await new Promise(r => setTimeout(r, 2500));
      const p = Math.min(88 + i * 0.5, 97);
      fill.style.width = p + "%";
      detail.textContent = `Waiting for device to come back online… (${i * 2.5 | 0}s)`;

      const health = await sendBg({ type: "sso:health" });
      if (health?.ok && health.data?.ok) {
        const newFw = health.data.fw || "?";
        setProgress(100, `Updated to v${newFw} ✓`, "SmartSpool is back online.");
        state.fwInfo = null;   // clear stale update info
        // Auto-advance after a beat so the user sees the success state
        await new Promise(r => setTimeout(r, 1800));
        show("filament-check");
        return;
      }
    }
    throw new Error("SmartSpool didn't come back online in time. Try refreshing the wizard.");

  } catch (e) {
    setProgress(0, "Update failed", "");
    prog.style.display    = "none";
    errBox.style.display  = "";
    errBox.textContent    = e.message;
    acts.style.display    = "";
    btn.disabled          = false;
    if (!info.required) skipBtn.style.display = "";
  }
}

/* ---------- step 3 / filament check ------------------------------- */
async function renderFilamentCheck() {
  await pollStatus();
  const recs = (state.meta.materials || []).map(m => m.toUpperCase());
  const $rec = $("#recMaterials");
  $rec.innerHTML = "";
  if (!recs.length) {
    chip($rec, "Couldn't detect — pick anything you like", "muted");
  } else {
    recs.forEach(m => chip($rec, m, "mat"));
  }

  const slot = state.status?.slot1 || {};
  $("#currentSlot").innerHTML = renderSlotCard(slot, state.status?.last_scan);

  const cur = (slot.material || "").toUpperCase();
  const ok = recs.length === 0 ? true : recs.includes(cur);
  state.needsSwap = !ok;
  const v = $("#filamentVerdict");
  if (ok) {
    v.className = "verdict ok";
    v.innerHTML = "✓ Slot 1 has a compatible material — you're ready to print.";
    $("#filamentOkBtn").style.display = "";
    $("#swapBtn").textContent = "I want to swap anyway";
  } else {
    v.className = "verdict warn";
    v.innerHTML = recs.length
      ? `Slot 1 has <b>${cur || "(empty)"}</b>, but this model wants <b>${recs.join(" or ")}</b>. Let's swap it.`
      : "Slot 1 looks empty.";
    $("#filamentOkBtn").style.display = "none";
    $("#swapBtn").textContent = "Swap filament →";
  }
}

function renderSlotCard(slot, lastScan) {
  const color = slot.color && /^[0-9a-f]{6}$/i.test(slot.color) ? slot.color : "808080";
  const mat = slot.material || "(empty)";
  const code = slot.code || "—";
  const name = lastScan?.name || "";
  return `
    <div class="slot-row">
      <span class="swatch big" style="background:#${color}"></span>
      <div>
        <div class="slot-mat">${mat}</div>
        <div class="slot-meta">${code}${name ? " · " + escapeHtml(name) : ""}</div>
      </div>
    </div>`;
}

/* ---------- step 4 / pick profile --------------------------------- */
async function loadProfiles() {
  const r = await sendBg({ type: "sso:profiles" });
  if (r?.ok && Array.isArray(r.data)) state.profiles = r.data;
}
async function renderProfileGrid() {
  if (!state.profiles.length) await loadProfiles();
  const grid = $("#profileGrid");
  grid.innerHTML = "";
  const recs = (state.meta.materials || []).map(m => m.toUpperCase());

  // Sort: recommended materials first
  const ranked = [...state.profiles].sort((a, b) => {
    const ar = recs.includes((a.material || "").toUpperCase()) ? 0 : 1;
    const br = recs.includes((b.material || "").toUpperCase()) ? 0 : 1;
    return ar - br || a.name.localeCompare(b.name);
  });

  ranked.forEach(p => {
    const matched = recs.includes((p.material || "").toUpperCase());
    const card = document.createElement("button");
    card.type = "button";
    card.className = "profile-card" + (matched ? " match" : "");
    card.innerHTML = `
      <span class="swatch big" style="background:#${p.color}"></span>
      <div class="meta">
        <div class="title">${escapeHtml(p.name)}</div>
        <div class="sub">${escapeHtml(p.brand || "Generic")} · ${p.material} · ${p.code}</div>
      </div>
      ${matched ? '<span class="badge">recommended</span>' : ''}`;
    card.onclick = () => {
      state.pickedProfile = p;
      show("tag");
    };
    grid.appendChild(card);
  });

  if (!ranked.length) {
    grid.innerHTML = `<div class="empty">No profiles yet. Open SSO-Lite's web UI and add some.</div>`;
  }
}

/* ---------- step 5 / tag write ------------------------------------ */
let tagPollTimer = null;
async function beginTagWrite() {
  const p = state.pickedProfile;
  if (!p) { show("filament-pick"); return; }
  $("#assignedCard").hidden = true;
  $("#tagContinueBtn").disabled = true;
  $("#tagStatus").className = "status pending";
  $("#tagStatus").textContent = `Queueing write for ${p.name}…`;

  const r = await sendBg({ type: "sso:queueWrite", name: p.name });
  if (!r?.ok) {
    $("#tagStatus").className = "status err";
    /* If the device responded but with a non-OK status, r.data may contain its
     * plain-text error (e.g. "profile not found"). Show that when possible
     * since it's far more useful than just "HTTP 404". */
    const deviceMsg = (typeof r?.data === "string" && r.data.trim()) ? r.data.trim() : "";
    const detail    = deviceMsg || r?.error || (`HTTP ${r?.status || "?"}`);
    $("#tagStatus").textContent = `Couldn't queue write: ${detail}`;
    return;
  }
  $("#tagStatus").textContent = `Tap your tag on the SSO-Lite reader (you have 60s).`;

  // Poll status; when last_scan UID changes AND code matches the picked
  // profile, we know the write succeeded and the tag was re-read.
  const startedAt = Date.now();
  const startUid = state.status?.last_scan?.uid || "";
  if (tagPollTimer) clearInterval(tagPollTimer);
  tagPollTimer = setInterval(async () => {
    if (Date.now() - startedAt > 65000) {
      clearInterval(tagPollTimer); tagPollTimer = null;
      $("#tagStatus").className = "status err";
      $("#tagStatus").textContent = "Timed out waiting for a tag. Try again.";
      return;
    }
    await pollStatus();
    const ls = state.status?.last_scan;
    if (ls?.valid && ls.code === p.code &&
        (ls.uid !== startUid || ls.age_ms < 8000)) {
      clearInterval(tagPollTimer); tagPollTimer = null;
      $("#tagStatus").className = "status ok";
      $("#tagStatus").textContent = "Tag written and AMS Slot 1 updated.";
      $("#assignedCard").hidden = false;
      $("#assignedCard").innerHTML = renderSlotCard(state.status.slot1, ls);
      $("#tagContinueBtn").disabled = false;
    }
  }, 800);
}

/* ---------- step 6 / plate ---------------------------------------- */
function renderPlateStep() {
  $("#plateOk").checked = false;
  $("#plateContinueBtn").disabled = true;
  const card = $("#plateCard");
  const plate = state.meta.plate || guessPlateFromMaterial();
  card.innerHTML = `
    <div class="plate-icon" data-plate="${escapeHtml(plate)}"></div>
    <div>
      <div class="plate-name">${escapeHtml(plate || "Any compatible plate")}</div>
      <div class="plate-help">${plateHelp(plate)}</div>
    </div>`;
}

function guessPlateFromMaterial() {
  const mats = (state.meta.materials || []).map(m => m.toUpperCase());
  if (mats.includes("PLA"))  return "Textured PEI Plate";
  if (mats.includes("PETG")) return "Textured PEI Plate";
  if (mats.includes("ABS") || mats.includes("ASA")) return "Engineering Plate";
  if (mats.includes("TPU"))  return "Textured PEI Plate";
  return "Textured PEI Plate";
}
function plateHelp(p) {
  if (!p) return "Use the plate that came with your printer if you're not sure.";
  const map = {
    "Textured PEI Plate":      "All-rounder. Great for PLA, PETG, TPU, and most everyday prints.",
    "Smooth PEI Plate":        "Glossy bottom finish. Watch out — too sticky for cold PLA without glue.",
    "Cool Plate":              "Best for low-temperature PLA. Don't use above 65°C bed temp.",
    "Cool Plate SuperTack":    "Newer plate, very strong adhesion for PLA/PETG.",
    "Engineering Plate":       "For ABS, ASA, PA, PC. Needs higher bed temps.",
    "High Temperature Plate":  "For PEEK, PEI and other very-high-temp filaments.",
  };
  return map[p] || "Check the model page for specifics.";
}

/* ---------- helpers ----------------------------------------------- */
async function pollStatus() {
  const r = await sendBg({ type: "sso:status" });
  if (r?.ok) state.status = r.data;
  return state.status;
}
function sendBg(msg) {
  return new Promise(res =>
    chrome.runtime.sendMessage(msg, r => res(r || { ok: false, error: chrome.runtime.lastError?.message })));
}
function escapeHtml(s) {
  return String(s ?? "").replace(/[&<>"']/g, c =>
    ({ "&":"&amp;", "<":"&lt;", ">":"&gt;", '"':"&quot;", "'":"&#39;" }[c]));
}
