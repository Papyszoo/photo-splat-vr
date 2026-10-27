#pragma once

namespace splat {

// C9. One self-contained mobile page: PIN entry, camera-roll picker, per-file
// upload progress and the job queue. No external scripts, fonts or images, so
// it works on a LAN with no internet access.
inline constexpr const char* kIngestPage = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Photo Splat VR - add photos</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; padding: 1rem; font: 16px/1.4 -apple-system, system-ui, sans-serif;
         background: #101216; color: #e8eaed; }
  main { max-width: 40rem; margin: 0 auto; }
  h1 { font-size: 1.35rem; margin: 0 0 .25rem; }
  h2 { font-size: 1rem; margin: 1.5rem 0 .5rem; color: #b8bec9; text-transform: uppercase;
       letter-spacing: .04em; }
  p.hint { color: #9aa2ae; margin: .25rem 0 1rem; }
  .card { background: #191d24; border: 1px solid #2a303a; border-radius: .75rem; padding: 1rem; }
  input[type=text] { width: 100%; padding: .9rem 1rem; font-size: 1.6rem; letter-spacing: .35em;
                     text-align: center; border-radius: .6rem; border: 1px solid #39414d;
                     background: #0d0f13; color: #fff; }
  button { width: 100%; margin-top: .75rem; padding: .9rem 1rem; font-size: 1rem; font-weight: 600;
           border: 0; border-radius: .6rem; background: #3b82f6; color: #fff; }
  button:disabled { opacity: .55; }
  label.pick { display: block; text-align: center; padding: 1.4rem 1rem; border: 2px dashed #39414d;
               border-radius: .75rem; color: #c7ced9; }
  label.pick input { display: none; }
  ul { list-style: none; margin: 0; padding: 0; }
  li { padding: .65rem 0; border-bottom: 1px solid #232933; }
  li:last-child { border-bottom: 0; }
  .row { display: flex; align-items: center; gap: .6rem; }
  .name { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .state { font-size: .75rem; padding: .2rem .5rem; border-radius: 99px; background: #2a303a;
           text-transform: uppercase; letter-spacing: .04em; }
  .state.queued { background: #334155; }
  .state.running { background: #7c5e10; color: #fff3c4; }
  .state.done { background: #14532d; color: #bbf7d0; }
  .state.failed, .state.cancelled { background: #641e1e; color: #fecaca; }
  .bar { height: 6px; background: #232933; border-radius: 99px; margin-top: .5rem; overflow: hidden; }
  .bar > i { display: block; height: 100%; width: 0; background: #3b82f6; transition: width .15s; }
  .msg { color: #fca5a5; font-size: .85rem; margin-top: .3rem; word-break: break-word; }
  .status { min-height: 1.4rem; margin-top: .75rem; color: #fbbf24; }
  .hidden { display: none !important; }
  .counts { color: #9aa2ae; font-size: .85rem; margin-bottom: .5rem; }
</style>
</head>
<body>
<main>
  <h1>Photo Splat VR</h1>
  <p class="hint">Photos go straight to the console on your local network.</p>

  <section id="login" class="card">
    <h2>Enter the PIN shown on the TV</h2>
    <form id="pin-form">
      <input id="pin" type="text" inputmode="numeric" autocomplete="one-time-code"
             pattern="[0-9]*" maxlength="6" placeholder="000000" aria-label="PIN">
      <button id="pin-button" type="submit">Unlock</button>
    </form>
    <div id="login-status" class="status" role="status"></div>
  </section>

  <section id="uploader" class="card hidden">
    <h2>Add photos</h2>
    <label class="pick">Tap to choose photos from the camera roll
      <input id="files" type="file" accept="image/jpeg,image/png" multiple>
    </label>
    <ul id="uploads"></ul>
  </section>

  <section id="queue-section" class="hidden">
    <h2>Queue</h2>
    <div id="counts" class="counts">No jobs yet.</div>
    <ul id="queue"></ul>
  </section>
</main>
<script>
(function () {
  "use strict";
  var pinForm = document.getElementById("pin-form");
  var pinInput = document.getElementById("pin");
  var loginStatus = document.getElementById("login-status");
  var uploader = document.getElementById("uploader");
  var queueSection = document.getElementById("queue-section");
  var uploadList = document.getElementById("uploads");
  var queueList = document.getElementById("queue");
  var counts = document.getElementById("counts");
  var filesInput = document.getElementById("files");

  function showLogin() {
    uploader.classList.add("hidden");
    queueSection.classList.add("hidden");
    document.getElementById("login").classList.remove("hidden");
    pinInput.focus();
  }

  function showLoggedIn() {
    document.getElementById("login").classList.add("hidden");
    uploader.classList.remove("hidden");
    queueSection.classList.remove("hidden");
  }

  function setLoginStatus(text) { loginStatus.textContent = text || ""; }

  function uploadRow(name) {
    var item = document.createElement("li");
    var row = document.createElement("div");
    row.className = "row";
    var label = document.createElement("span");
    label.className = "name";
    label.textContent = name;
    var state = document.createElement("span");
    state.className = "state";
    state.textContent = "0%";
    row.appendChild(label);
    row.appendChild(state);
    var bar = document.createElement("div");
    bar.className = "bar";
    var fill = document.createElement("i");
    bar.appendChild(fill);
    item.appendChild(row);
    item.appendChild(bar);
    uploadList.insertBefore(item, uploadList.firstChild);
    return { item: item, state: state, fill: fill };
  }

  function setRow(row, percent, text, failed) {
    row.fill.style.width = Math.max(0, Math.min(100, percent)) + "%";
    row.state.textContent = text;
    if (failed) row.state.className = "state failed";
    else if (text === "done") row.state.className = "state done";
    else row.state.className = "state running";
  }

  function uploadOne(file) {
    return new Promise(function (resolve) {
      var row = uploadRow(file.name);
      var xhr = new XMLHttpRequest();
      xhr.open("POST", "/api/upload");
      xhr.upload.onprogress = function (event) {
        if (event.lengthComputable) {
          setRow(row, Math.round(event.loaded / event.total * 100), "uploading");
        }
      };
      xhr.onload = function () {
        if (xhr.status === 200) {
          setRow(row, 100, "done");
        } else if (xhr.status === 401) {
          setRow(row, 100, "login needed", true);
          showLogin();
        } else {
          var message = "rejected";
          try { message = JSON.parse(xhr.responseText).error || message; } catch (e) {}
          setRow(row, 100, "rejected", true);
          var detail = document.createElement("div");
          detail.className = "msg";
          detail.textContent = message;
          row.item.appendChild(detail);
        }
        resolve();
      };
      xhr.onerror = function () {
        setRow(row, 0, "network error", true);
        resolve();
      };
      var body = new FormData();
      body.append("photo", file, file.name);
      xhr.send(body);
    });
  }

  function renderQueue(data) {
    var jobs = data.jobs || [];
    var summary = data.counts || {};
    counts.textContent = (summary.queued || 0) + " queued, " + (summary.running || 0) +
      " running, " + (summary.done || 0) + " done, " + (summary.failed || 0) + " failed";
    queueList.textContent = "";
    jobs.slice().reverse().forEach(function (job) {
      var item = document.createElement("li");
      var row = document.createElement("div");
      row.className = "row";
      var name = document.createElement("span");
      name.className = "name";
      name.textContent = job.photo || job.id || "job";
      var state = document.createElement("span");
      state.className = "state " + (job.state || "");
      state.textContent = job.state || "unknown";
      row.appendChild(name);
      row.appendChild(state);
      item.appendChild(row);
      if (job.message) {
        var message = document.createElement("div");
        message.className = "msg";
        message.textContent = job.message;
        item.appendChild(message);
      }
      queueList.appendChild(item);
    });
  }

  function refreshQueue() {
    return fetch("/api/queue", { cache: "no-store" }).then(function (response) {
      if (response.status === 401) { showLogin(); return; }
      if (!response.ok) return;
      return response.json().then(function (data) {
        showLoggedIn();
        renderQueue(data);
      });
    }).catch(function () { /* retry on the next poll */ });
  }

  pinForm.addEventListener("submit", function (event) {
    event.preventDefault();
    var pin = pinInput.value.trim();
    if (pin.length !== 6) { setLoginStatus("The PIN has six digits."); return; }
    setLoginStatus("");
    document.getElementById("pin-button").disabled = true;
    fetch("/api/login", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: "pin=" + encodeURIComponent(pin)
    }).then(function (response) {
      document.getElementById("pin-button").disabled = false;
      if (response.ok) {
        pinInput.value = "";
        setLoginStatus("");
        showLoggedIn();
        refreshQueue();
      } else if (response.status === 429) {
        setLoginStatus("Too many attempts. Wait a minute, then try again.");
      } else {
        setLoginStatus("Wrong PIN. Check the TV.");
      }
    }).catch(function () {
      document.getElementById("pin-button").disabled = false;
      setLoginStatus("Cannot reach the console.");
    });
  });

  filesInput.addEventListener("change", function () {
    var files = Array.prototype.slice.call(filesInput.files);
    filesInput.value = "";
    var chain = Promise.resolve();
    files.forEach(function (file) {
      chain = chain.then(function () { return uploadOne(file); })
                   .then(function () { return refreshQueue(); });
    });
  });

  refreshQueue();
  setInterval(refreshQueue, 2000);
})();
</script>
</body>
</html>
)HTML";

}  // namespace splat
