/* inject.js — runs in the page's MAIN JavaScript world.
 *
 * MakerWorld's "Print" button doesn't expose a static <a href="bambustudio…">
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

  /* 1. window.open(url, ...) — this is how most "open in app" buttons work. */
  const _open = window.open.bind(window);
  window.open = function (url, ...rest) {
    if (typeof url === "string" && SCHEME_RX.test(url)) {
      emit(url, "window.open");
      // Don't suppress: still let the browser try to launch. If it fails,
      // our content script will relaunch via hidden iframe as a backup.
    }
    return _open(url, ...rest);
  };

  /* 2. HTMLAnchorElement.prototype.click — programmatic clicks on <a> tags
   *    that the page creates dynamically. */
  const _click = HTMLAnchorElement.prototype.click;
  HTMLAnchorElement.prototype.click = function () {
    if (this.href && SCHEME_RX.test(this.href)) {
      emit(this.href, "anchor.click");
    }
    return _click.apply(this, arguments);
  };

  /* 3. Location.assign / Location.replace / location.href setter.
   *    Some apps navigate the top window directly to a custom-protocol URL.
   *    We can't override the Location prototype on all browsers reliably,
   *    so we monkey-patch the methods that are accessible. */
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
  } catch (e) { /* some browsers freeze the prototype; that's fine */ }

  /* 4. Catch real user clicks on dynamically-styled "buttons" that internally
   *    trigger one of the above. We also listen for click events at the
   *    capture phase and look at the URL of the topmost <a> in the path —
   *    handles cases where the click handler immediately calls .click() on
   *    a hidden anchor inside an event handler. */
  document.addEventListener("click", (ev) => {
    const path = ev.composedPath ? ev.composedPath() : [];
    for (const el of path) {
      if (el && el.tagName === "A" && el.href && SCHEME_RX.test(el.href)) {
        emit(el.href, "click-event");
        return;
      }
    }
  }, true);

  console.log("[mw-wiz inject] loaded — watching for bambustudio*:// URLs");
})();
