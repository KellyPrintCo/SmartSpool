/* content.js - runs on every makerworld.com page.
 *
 * Responsibilities:
 *   1. Detect when we're on a model detail page.
 *   2. Inject a "Print Wizard" button somewhere visible.
 *   3. When clicked, open the wizard as a fixed-position iframe
 *      so the user stays on the model page.
 *   4. Scrape best-effort metadata from the page (title, materials,
 *      plate type) and pass it to the wizard via postMessage.
 *
 * MakerWorld's DOM is a moving target; selectors are wrapped in
 * defensive tryNodes() helpers and fall back gracefully.
 */

const NS = "ssoMwWiz";

(function init() {
  if (window[NS]) return;            // hot-reload guard
  window[NS] = true;

  injectMainWorldScript();
  listenForCapturedUrls();

  let lastUrl = location.href;
  injectButtonWhenReady();

  /* MakerWorld is a single-page app — the URL changes without a full
   * navigation. Re-inject when the URL changes. */
  const obs = new MutationObserver(() => {
    if (location.href !== lastUrl) {
      lastUrl = location.href;
      setTimeout(injectButtonWhenReady, 400);
    }
  });
  obs.observe(document.body, { childList: true, subtree: true });
})();

/* Inject inject.js into the page's MAIN world by appending a <script> tag.
 * This lets us monkey-patch window.open, anchor clicks, etc. — content
 * scripts run in an isolated world and can't touch the page's real
 * window object directly. */
function injectMainWorldScript() {
  try {
    const s = document.createElement("script");
    s.src = chrome.runtime.getURL("inject.js");
    s.async = false;
    s.onload = () => s.remove();
    (document.head || document.documentElement).appendChild(s);
    console.log("[mw-wiz] inject.js requested");
  } catch (e) {
    console.warn("[mw-wiz] could not inject main-world script:", e);
  }
}

/* inject.js posts a window message whenever the page tries to launch a
 * bambustudio*:// URL. Forward those to the wizard iframe so it can
 * relaunch via the hidden-iframe trick (no UI flash) and advance. */
function listenForCapturedUrls() {
  window.addEventListener("message", (ev) => {
    if (ev.source !== window) return;
    const m = ev.data;
    if (m && m.__ssoMwWiz && m.type === "capturedUrl" && m.url) {
      console.log("[mw-wiz] inject captured URL via", m.source, ":", m.url);
      wizardFrame?.contentWindow?.postMessage(
        { type: "wizard:studioUrlFound", url: m.url }, "*");
    }
  });
}

function isModelPage() {
  // /en/models/12345-name, /models/12345, /de/models/...
  return /\/models\/\d+/.test(location.pathname);
}

function injectButtonWhenReady() {
  if (!isModelPage()) {
    document.getElementById("sso-mw-wizbtn")?.remove();
    return;
  }
  let tries = 0;
  const tick = () => {
    if (document.getElementById("sso-mw-wizbtn")) return;
    const anchor = findAnchor();
    if (anchor) {
      anchor.parentNode.insertBefore(makeButton(), anchor);
    } else if (tries++ < 40) {
      setTimeout(tick, 250);
    } else {
      // Fallback: pin to viewport corner so the button is always reachable.
      document.body.appendChild(makeFloatingButton());
    }
  };
  tick();
}

function findAnchor() {
  /* Look for MakerWorld's existing "Open in Studio" / "Print" CTA.
   * These selectors are heuristic; if MakerWorld redesigns we fall
   * back to text-based scanning. */
  const candidates = [
    /* Direct deeplink anchors — any of the known protocol schemes. */
    'a[href^="bambustudioopen://"]',
    'a[href^="bambustudio://"]',
    'a[href^="bambustudiolink://"]',
    'a[href^="orcaslicer://"]',
    /* Class/data-attribute heuristics — MakerWorld changes these often. */
    'button[data-trace-name*="open_in_studio" i]',
    'button[data-trace-name*="print" i]',
    'button[class*="OpenInStudio" i]',
    'button[class*="open-in-studio" i]',
    'button[class*="PrintBtn" i]',
  ];
  for (const sel of candidates) {
    try {
      const el = document.querySelector(sel);
      if (el && el.offsetParent !== null) return el;
    } catch { /* selector might be invalid in some browsers */ }
  }
  /* Text-based scan as last resort. Match many languages and partial
   * substrings — MakerWorld is internationalised. */
  const haystacks = [
    "open in bambu studio", "open in studio", "open in orcaslicer",
    "send to printer", "print this model", "print",
    "in studio öffnen", "ouvrir dans bambu studio", "abrir en bambu studio",
    "studioで開く", "在 studio 中打开", "在bambu studio中打开",
  ];
  const buttons = document.querySelectorAll("button, a");
  for (const b of buttons) {
    if (b.offsetParent === null) continue;       // hidden
    const t = (b.innerText || b.textContent || "").trim().toLowerCase();
    if (!t || t.length > 60) continue;
    for (const needle of haystacks) {
      if (t === needle || t.startsWith(needle)) return b;
    }
  }
  return null;
}

function makeButton() {
  const btn = document.createElement("button");
  btn.id = "sso-mw-wizbtn";
  btn.className = "sso-mw-wizbtn";
  btn.type = "button";
  btn.innerHTML =
    `<span class="sso-mw-wizbtn-ic">🖨️</span><span>Print Wizard</span>`;
  btn.title = "Step-by-step print guide via SSO-Lite";
  btn.addEventListener("click", openWizard);
  return btn;
}

function makeFloatingButton() {
  const wrap = document.createElement("div");
  wrap.id = "sso-mw-wizbtn-float";
  wrap.appendChild(makeButton());
  return wrap;
}

let wizardFrame = null;

function openWizard() {
  if (wizardFrame) { wizardFrame.style.display = "block"; return; }

  const meta = scrapePageMeta();

  wizardFrame = document.createElement("iframe");
  wizardFrame.id = "sso-mw-wizard-frame";
  wizardFrame.src = chrome.runtime.getURL("wizard.html");
  wizardFrame.allow = "clipboard-write";
  document.body.appendChild(wizardFrame);

  /* When the wizard signals it's ready, push the page metadata across.
   * Listen on window for messages back from the iframe. */
  window.addEventListener("message", onWizardMessage);

  // Send meta after a short delay; wizard.js will also re-request on init.
  setTimeout(() => {
    wizardFrame?.contentWindow?.postMessage(
      { type: "wizard:init", meta }, "*");
  }, 600);
}

function closeWizard() {
  wizardFrame?.remove();
  wizardFrame = null;
  window.removeEventListener("message", onWizardMessage);
}

function onWizardMessage(ev) {
  const m = ev.data || {};
  switch (m.type) {
    case "wizard:close":
      closeWizard();
      break;
    case "wizard:requestMeta":
      wizardFrame?.contentWindow?.postMessage(
        { type: "wizard:init", meta: scrapePageMeta() }, "*");
      break;
    case "wizard:openInStudio":
      triggerOpenInStudio();
      break;
    case "wizard:hideForUserClick":
      hideOverlayWithReminder();
      break;
    case "wizard:show":
      showOverlay();
      break;
  }
}

let reminderEl = null;
function hideOverlayWithReminder() {
  if (wizardFrame) wizardFrame.style.display = "none";
  if (reminderEl) return;
  reminderEl = document.createElement("div");
  reminderEl.id = "sso-mw-reminder";
  reminderEl.innerHTML = `
    <div class="sso-mw-reminder-inner">
      <strong>👇 Click MakerWorld's <em>Print</em> button now</strong>
      <p>Bambu Studio will open. We'll catch the launch automatically.</p>
      <button id="sso-mw-reminder-back" type="button">Bring wizard back</button>
    </div>`;
  document.body.appendChild(reminderEl);
  document.getElementById("sso-mw-reminder-back").addEventListener("click", showOverlay);
}
function showOverlay() {
  if (reminderEl) { reminderEl.remove(); reminderEl = null; }
  if (wizardFrame) wizardFrame.style.display = "";
}

/* Click MakerWorld's own "Open in Studio" button or a-tag, hiding our
 * overlay first so any MakerWorld toast/modal is visible to the user.
 * NOTE: Most of the time, the wizard launches the bambustudio:// URL
 * itself via the hidden-iframe trick — this function is only called
 * as a last resort when the URL couldn't be scraped. */
function triggerOpenInStudio() {
  /* If we missed the URL during initial scrape (it may have rendered
   * after page-idle), try once more right before clicking. */
  const urlNow = scrapeStudioUrl();
  if (urlNow) {
    console.log("[mw-wiz] late-binding studio URL:", urlNow);
    wizardFrame?.contentWindow?.postMessage(
      { type: "wizard:studioUrlFound", url: urlNow }, "*");
    return;
  }
  const target = findAnchor();
  if (!target) {
    console.warn("[mw-wiz] no Studio link or button found on page");
    /* Diagnostic: dump candidate elements to console so the user can share. */
    debugDumpButtons();
    wizardFrame?.contentWindow?.postMessage({
      type: "wizard:studioLinkMissing"
    }, "*");
    return;
  }
  console.log("[mw-wiz] clicking", target.tagName, target.outerHTML.slice(0, 120));
  const prev = wizardFrame?.style.display;
  if (wizardFrame) wizardFrame.style.display = "none";
  target.click();
  setTimeout(() => { if (wizardFrame) wizardFrame.style.display = prev || ""; }, 1500);
}

/* Dumps all visible buttons/anchors to console so we can diagnose why
 * the Studio launcher couldn't find anything. The user can copy this
 * output and share it. */
function debugDumpButtons() {
  const els = document.querySelectorAll("button, a[href]");
  const interesting = [];
  for (const el of els) {
    if (el.offsetParent === null) continue;
    const text = (el.innerText || el.textContent || "").trim().slice(0, 80);
    const href = el.getAttribute("href") || "";
    const cls  = el.getAttribute("class") || "";
    if (text.length === 0 && !href) continue;
    if (text.toLowerCase().includes("studio") ||
        text.toLowerCase().includes("print")  ||
        href.includes("bambu") || href.includes("studio") ||
        cls.toLowerCase().includes("print")   ||
        cls.toLowerCase().includes("studio")) {
      interesting.push({ tag: el.tagName, text, href, cls });
    }
  }
  console.group("[mw-wiz] candidate buttons/anchors on this page");
  console.table(interesting.slice(0, 30));
  console.log("(if you see what looks like the right button above, please share this list.)");
  console.groupEnd();
}

/* ----------------------- page scraping ----------------------- */

function scrapePageMeta() {
  const meta = {
    title: scrapeTitle(),
    url: location.href,
    materials: scrapeMaterials(),
    plate: scrapePlate(),
    image: scrapeImage(),
    studioUrl: scrapeStudioUrl(),
  };
  console.log("[mw-wiz] page meta:", meta);
  return meta;
}

/* MakerWorld renders an <a href="bambustudioopen://..."> on every model
 * page that the regular "Open in Studio" button triggers. (Earlier
 * versions used "bambustudio://"; some forks use "bambustudiolink://".)
 * We grab whichever one is present and let the wizard launch it directly
 * via the hidden-iframe trick — bypasses any MakerWorld confirmation
 * modal or login toast. */
function scrapeStudioUrl() {
  /* Match any anchor whose href starts with a known scheme. Order matters:
   * we prefer bambustudioopen because that's MakerWorld's current scheme. */
  const schemes = [
    "bambustudioopen://",
    "bambustudio://",
    "bambustudiolink://",
    "orcaslicer://",     // for users who set OrcaSlicer as the handler
  ];
  for (const scheme of schemes) {
    const a = document.querySelector(`a[href^="${scheme}"]`);
    if (a?.href) return a.href;
  }
  /* Last-ditch: any href that contains "bambustudio" or starts with a
   * non-http(s) scheme that has "studio" in it. */
  const all = document.querySelectorAll("a[href]");
  for (const a of all) {
    const h = a.getAttribute("href") || "";
    if (/^[a-z][a-z0-9+\-.]*:/i.test(h) &&
        !/^https?:/i.test(h) &&
        /studio|orcaslicer|bambu/i.test(h)) {
      return h;
    }
  }
  return null;
}

function scrapeTitle() {
  const h1 = document.querySelector("h1");
  if (h1?.innerText) return h1.innerText.trim();
  const og = document.querySelector('meta[property="og:title"]');
  if (og?.content) return og.content.trim();
  return document.title.replace(/\s*\|\s*MakerWorld.*$/i, "").trim();
}

function scrapeImage() {
  const og = document.querySelector('meta[property="og:image"]');
  return og?.content || null;
}

/* MakerWorld typically lists "Filament" or "Materials" in a side panel
 * with chip-style tags. Look for those, then fall back to body text. */
function scrapeMaterials() {
  const materials = new Set();
  const known = ["PLA", "PETG", "ABS", "ASA", "TPU", "PA", "PC", "PVA", "PETG-CF", "PA-CF", "PLA-CF"];
  const bodyText = (document.body.innerText || "").toUpperCase();
  for (const m of known) {
    // word boundary match (fairly forgiving)
    const re = new RegExp(`\\b${m.replace("-", "\\-")}\\b`);
    if (re.test(bodyText)) materials.add(m);
  }
  return Array.from(materials);
}

/* Plate detection: look for known plate names in the page body. */
function scrapePlate() {
  const text = (document.body.innerText || "").toLowerCase();
  const plates = [
    { match: ["textured pei", "textured plate"], label: "Textured PEI Plate" },
    { match: ["smooth pei"],                     label: "Smooth PEI Plate" },
    { match: ["cool plate supertack"],           label: "Cool Plate SuperTack" },
    { match: ["cool plate"],                     label: "Cool Plate" },
    { match: ["engineering plate"],              label: "Engineering Plate" },
    { match: ["high temperature plate", "high temp plate"], label: "High Temperature Plate" },
  ];
  for (const p of plates) {
    if (p.match.some(s => text.includes(s))) return p.label;
  }
  return null;
}
