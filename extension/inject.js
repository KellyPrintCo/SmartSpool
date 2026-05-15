/* inject.js -- runs in the page's MAIN JavaScript world.
 *
 * MakerWorld's "Print" button doesn't expose a static <a href="bambustudio..."
 * anchor we can scrape. The button is built as a dynamic component, and the
 * actual deeplink URL is constructed at click-time and either passed to
 * window.open() or used as anchor.href + .click() on a dynamically created
 * element. Either way, the URL touches one of the entry points below.
 *
 * We monkey-patch those entry points so that whenever the page tries to
 * launch a bambustudio* / orcaslicer URL, we capture it and forward it via
 * window.postMessage to our content script. The content script then forwards
 * it to the wizard iframe, which launches it via the hidden-iframe trick.
 *
 * This script is injected once per page load by content.js and is idempotent.
 */

(function () {
  if (window.__ssoMwWizInjected) return;
  window.__ssoMwWizInjected = true;

  const SCHEME_RX = /^(?:bambustudio(?:open|link)?|orcaslicer):/i;

  function emit(url, source) {
    try {
      window.postMessage(
        { __ssoMwWiz: true, type: "capturedUrl", url, source }, "*");
      console.log("[mw-wiz inject] captured", source, url);
    } catch (e) { /* ignore */ }
  }

  /* 1. window.open(url, ...) - most "open in app" buttons. */
  const _open = window.open.bind(window);
  window.open = function (url, ...rest) {
    if (typeof url === "string" && SCHEME_RX.test(url)) {
      emit(url, "window.open");
    }
    return _open(url, ...rest);
  };

  /* 2. HTMLAnchorElement.prototype.click - programmatic clicks. */
  const _click = HTMLAnchorElement.prototype.click;
  HTMLAnchorElement.prototype.click = function () {
    if (this.href && SCHEME_RX.test(this.href)) {
      emit(this.href, "anchor.click");
    }
    return _click.apply(this, arguments);
  };

  /* 3. Location.assign / Location.replace / location.href setter.
   *    Direct assignment to window.location.href is the most common path for
   *    custom-protocol launches. We have to use Object.defineProperty on the
   *    Location prototype to intercept the setter. */
  try {
    const locProto = Object.getPrototypeOf(window.location);
    const _assign  = locProto.assign;
    const _replace = locProto.replace;
    locProto.assign = function (url) {
      if (typeof url === "string" && SCHEME_RX.test(url)) emit(url, "location.assign");
      return _assign.call(this, url);
    };
    locProto.replace = function (url) {
      if (typeof url === "string" && SCHEME_RX.test(url)) emit(url, "location.replace");
      return _replace.call(this, url);
    };
    /* Intercept location.href = "..." assignment via property descriptor.
     * This is the most common protocol-launch path on modern sites. */
    const hrefDesc = Object.getOwnPropertyDescriptor(locProto, "href");
    if (hrefDesc && hrefDesc.set && hrefDesc.configurable) {
      const _hrefSetter = hrefDesc.set;
      Object.defineProperty(locProto, "href", {
        get: hrefDesc.get,
        set: function (url) {
          if (typeof url === "string" && SCHEME_RX.test(url)) emit(url, "location.href=");
          return _hrefSetter.call(this, url);
        },
        configurable: true,
        enumerable: hrefDesc.enumerable
      });
    }
  } catch (e) {
    console.warn("[mw-wiz inject] location patch partial:", e);
  }

  /* 4. Catch real user clicks on dynamically-styled "buttons" that internally
   *    trigger one of the above. Capture-phase so we see it before the page's
   *    handler runs. */
  document.addEventListener("click", (ev) => {
    const path = ev.composedPath ? ev.composedPath() : [];
    for (const el of path) {
      if (el && el.tagName === "A" && el.href && SCHEME_RX.test(el.href)) {
        emit(el.href, "click-event");
        return;
      }
    }
  }, true);

  /* 5. Intercept Element.setAttribute() to catch dynamic anchor/iframe builds.
   *    Some frameworks build the link like:
   *       const a = document.createElement('a');
   *       a.setAttribute('href', 'bambustudioopen://...');
   *       a.click();
   *    We catch it at setAttribute time so we don't miss the URL even if .click()
   *    is dispatched in a way we can't observe. */
  const _setAttribute = Element.prototype.setAttribute;
  Element.prototype.setAttribute = function (name, value) {
    if ((name === "href" || name === "src") &&
        typeof value === "string" && SCHEME_RX.test(value)) {
      emit(value, "setAttribute:" + this.tagName.toLowerCase() + "." + name);
    }
    return _setAttribute.call(this, name, value);
  };

  /* 6. Watch the DOM for any new element with a matching href/src appearing.
   *    Catches everything else: dynamically inserted iframes, anchors, etc. */
  function scanNode(node) {
    if (!node || node.nodeType !== 1) return;
    if (node.tagName === "A" && node.href && SCHEME_RX.test(node.href)) {
      emit(node.href, "mutation:a.href");
    }
    if (node.tagName === "IFRAME" && node.src && SCHEME_RX.test(node.src)) {
      emit(node.src, "mutation:iframe.src");
    }
    if (node.querySelectorAll) {
      const anchors = node.querySelectorAll("a[href]");
      for (const a of anchors) {
        if (SCHEME_RX.test(a.href)) emit(a.href, "mutation:a[href]");
      }
      const iframes = node.querySelectorAll("iframe[src]");
      for (const f of iframes) {
        if (SCHEME_RX.test(f.src)) emit(f.src, "mutation:iframe[src]");
      }
    }
  }
  try {
    const mo = new MutationObserver((mutations) => {
      for (const m of mutations) {
        for (const n of m.addedNodes) scanNode(n);
        if (m.type === "attributes" && m.target) scanNode(m.target);
      }
    });
    mo.observe(document.documentElement, {
      childList: true, subtree: true,
      attributes: true, attributeFilter: ["href", "src"]
    });
  } catch (e) {
    console.warn("[mw-wiz inject] MutationObserver setup failed:", e);
  }

  /* 7. DIAGNOSTIC: when the user clicks anything that looks like a Print
   *    button (text contains 'Bambu Studio' or similar), schedule a deep
   *    scan of the entire DOM 250ms later. This catches MakerWorld's
   *    delayed-construction launch pattern where the URL is generated
   *    after an async operation following the click. */
  document.addEventListener("click", (ev) => {
    const target = ev.target;
    if (!target || !target.textContent) return;
    const txt = (target.textContent || "").toLowerCase();
    if (txt.indexOf("bambu studio") === -1 && txt.indexOf("open in") === -1) return;
    console.log("[mw-wiz inject] Bambu-Studio-style click detected; scheduling deep scan");
    /* Run several scans over 2 seconds to catch async launches */
    let count = 0;
    const scanAll = () => {
      const allAnchors = document.querySelectorAll("a[href]");
      for (const a of allAnchors) {
        if (SCHEME_RX.test(a.href)) emit(a.href, "post-click-scan:a[href]");
      }
      const allIframes = document.querySelectorAll("iframe[src]");
      for (const f of allIframes) {
        if (SCHEME_RX.test(f.src)) emit(f.src, "post-click-scan:iframe[src]");
      }
      count++;
      if (count < 10) setTimeout(scanAll, 200);
    };
    setTimeout(scanAll, 100);
  }, true);

  console.log("[mw-wiz inject] v3 loaded - watching for bambustudio*:// URLs (window.open, anchor.click, setAttribute, MutationObserver, click event, location.assign/replace/href, post-click scan)");
})();
