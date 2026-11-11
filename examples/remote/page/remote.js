// examples/remote/page/remote.js: the remote example's page script. It speaks only to the server it came from
// (relative URLs: the same origin, so the library's Origin and Sec-Fetch-Site checks pass), with fetch and nothing a
// secure context is needed for. A token, when the host set one, comes in the page's address after a '#'
// (http://192.168.1.20:8080/#k=...), which a browser never sends to a server; the script puts it on every request it
// makes, in the Authorization header (docs/web.md, "The token").
"use strict";

const token = new URLSearchParams(location.hash.slice(1)).get("k");
const headers = token ? { Authorization: "Bearer " + token } : {};

const $ = (id) => document.getElementById(id);
const status = $("status");

// The pitch slider's 0 to 1 is 55 Hz to 1760 Hz, five octaves; the program takes 20 to 20000.
const toPitch = (x) => 55 * Math.pow(32, x);
const fromPitch = (hz) => Math.log(hz / 55) / Math.log(32);

// One change in flight at a time: a slider moves faster than a request goes, so the latest value waits for the one
// before it to be answered, and the values in between are dropped.
const pending = new Map();
let sending = false;

async function send() {
  if (sending) return;
  sending = true;
  while (pending.size > 0) {
    const [key, path] = pending.entries().next().value;
    pending.delete(key);
    try {
      const response = await fetch(path, { method: "POST", headers });
      if (!response.ok) status.textContent = path + ": " + response.status;
    } catch (error) {
      status.textContent = "not reachable: " + error.message;
    }
  }
  sending = false;
}

function change(key, path) {
  pending.set(key, path);   // replaces a value of the same control not yet sent
  send();
}

$("gain").addEventListener("input", (event) => {
  change("gain", "/gain?value=" + Number(event.target.value).toFixed(2));
});
$("pitch").addEventListener("input", (event) => {
  change("pitch", "/pitch?value=" + toPitch(Number(event.target.value)).toFixed(2));
});
for (const key of document.querySelectorAll(".keys button")) {
  key.addEventListener("click", () => change("note", "/note/" + key.dataset.note));
}

// The state, polled ten times a second over the one kept-alive connection: the meter, and the controls as the
// program has them (another page, or curl, may have moved them).
let touched = 0;
for (const control of [$("gain"), $("pitch")]) {
  control.addEventListener("pointerdown", () => { touched = Date.now(); });
  control.addEventListener("input", () => { touched = Date.now(); });
}

async function poll() {
  try {
    const response = await fetch("/state", { headers, cache: "no-store" });
    if (response.ok) {
      const state = await response.json();
      $("gain-value").textContent = state.gain.toFixed(2);
      $("pitch-value").textContent = state.pitch.toFixed(1);
      $("level-value").textContent = state.level.toFixed(3);
      $("level").style.width = Math.min(100, state.level * 100) + "%";
      if (Date.now() - touched > 1000) {   // not while the user's hand is on a slider
        $("gain").value = state.gain;
        $("pitch").value = fromPitch(state.pitch);
      }
      status.textContent = "connected: " + state.changes + " changes";
    } else {
      status.textContent = "/state: " + response.status + (response.status === 401 ? " (the token?)" : "");
    }
  } catch (error) {
    status.textContent = "not reachable: " + error.message;
  }
  setTimeout(poll, 100);
}
poll();
