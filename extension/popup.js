const $ = s => document.querySelector(s);

async function refresh() {
  const settings = await new Promise(r =>
    chrome.runtime.sendMessage({ type: "settings:get" }, r));
  const host = settings?.data?.ssoHost || "(not set)";
  $("#addr").textContent = host;

  const health = await new Promise(r =>
    chrome.runtime.sendMessage({ type: "sso:health" }, r));
  if (health?.ok && health.data?.ok) {
    $("#conn").textContent = "online · v" + (health.data.fw || "?");
    $("#conn").className = "pill ok";
    const status = await new Promise(r =>
      chrome.runtime.sendMessage({ type: "sso:status" }, r));
    if (status?.ok && status.data?.slot1) {
      const s = status.data.slot1;
      $("#slot").innerHTML = `<span class="swatch" style="background:#${s.color || "808080"}"></span> ${s.material || "?"} (${s.code || "?"})`;
    }
  } else {
    $("#conn").textContent = "unreachable";
    $("#conn").className = "pill err";
    $("#slot").textContent = "—";
  }
}

$("#optionsBtn").onclick = () => chrome.runtime.openOptionsPage();
$("#openSso").onclick = async () => {
  const s = await new Promise(r => chrome.runtime.sendMessage({ type: "settings:get" }, r));
  const host = s?.data?.ssoHost || "ssolite.local";
  chrome.tabs.create({ url: `http://${host}/` });
};

refresh();
