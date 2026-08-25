// SpotiSpeed - playback speed knob in Spotify's footer.
//
// The knob talks to spotispeed.dll, which is injected into Spotify and retimes
// the audio on its way to the sound card. Drag the knob to change speed,
// click it to snap back to 1x.

(function SpotiSpeed() {
    "use strict";

    var MIN = 0.2, MAX = 2.0, PORT = 4381;
    var STORE = "spotispeed.rate";
    var LOG = Math.log(MAX / MIN);

    // speed <-> normalised knob position, log-scaled so 1x sits naturally
    function toT(v) { return Math.log(v / MIN) / LOG; }
    function toV(t) { return MIN * Math.exp(LOG * Math.max(0, Math.min(1, t))); }
    function clampV(v) { return Math.max(MIN, Math.min(MAX, v)); }

    var speed = 1.0;
    try {
        var saved = parseFloat(localStorage.getItem(STORE));
        if (isFinite(saved) && saved > 0) speed = clampV(saved);
    } catch (e) {}

    // ---------------------------------------------------------------- engine --
    var engineUp = false, pending = null, inFlight = false;

    function push(v) {
        pending = v;
        if (inFlight) return;
        inFlight = true;
        var send = pending; pending = null;
        fetch("http://127.0.0.1:" + PORT + "/speed?v=" + send.toFixed(4), { cache: "no-store" })
            .then(function (r) { return r.json(); })
            .then(function () { setEngine(true); })
            .catch(function () { setEngine(false); })
            .then(function () {
                inFlight = false;
                if (pending !== null) push(pending);
            });
    }

    function setEngine(up) {
        if (engineUp === up) return;
        engineUp = up;
        if (root) root.classList.toggle("ss-offline", !up);
        if (root) root.title = up ? tip() : "SpotiSpeed engine not running";
    }

    // The engine is a fresh process every time Spotify restarts, so it always
    // comes up at 1x, and it usually comes up a few seconds AFTER the knob does.
    // Without this the knob would keep showing the remembered speed while the
    // audio played at 1x. Whenever the engine appears, or its speed drifts from
    // what the knob shows, send ours again.
    function reconcile(j) {
        var wasUp = engineUp;
        setEngine(true);
        var theirs = (j && typeof j.speed === "number") ? j.speed : 1;
        if (!wasUp || Math.abs(theirs - speed) > 0.005) push(speed);
    }

    function poll() {
        fetch("http://127.0.0.1:" + PORT + "/speed", { cache: "no-store" })
            .then(function (r) { return r.json(); })
            .then(reconcile)
            .catch(function () { setEngine(false); });
    }

    function tip() { return "Playback speed " + speed.toFixed(2) + "x - drag to change, click to reset"; }

    // ------------------------------------------------------------------- ui --
    var root = null, arc = null, needle = null, label = null;
    var ARC_LEN = 0;

    function css() {
        if (document.getElementById("ss-style")) return;
        var s = document.createElement("style");
        s.id = "ss-style";
        s.textContent = [
            ".ss-knob{display:flex;align-items:center;gap:6px;margin-left:8px;",
            "  cursor:ns-resize;user-select:none;-webkit-user-select:none;touch-action:none;}",
            ".ss-knob svg{display:block;overflow:visible}",
            ".ss-knob .ss-track{stroke:currentColor;opacity:.25}",
            ".ss-knob .ss-fill{stroke:#1ed760;transition:stroke .15s}",
            ".ss-knob .ss-needle{stroke:currentColor}",
            ".ss-knob .ss-val{font-size:11px;font-weight:700;letter-spacing:.02em;",
            "  color:var(--text-subdued,#b3b3b3);min-width:34px;font-variant-numeric:tabular-nums}",
            ".ss-knob:hover .ss-val{color:var(--text-base,#fff)}",
            ".ss-knob.ss-active .ss-val{color:#1ed760}",
            ".ss-knob.ss-unity .ss-fill{stroke:currentColor;opacity:.55}",
            ".ss-knob.ss-offline{opacity:.4}",
            ".ss-knob.ss-offline .ss-fill{stroke:#e22134}"
        ].join("");
        document.head.appendChild(s);
    }

    function build() {
        var el = document.createElement("div");
        el.className = "ss-knob";
        el.setAttribute("role", "slider");
        el.setAttribute("aria-label", "Playback speed");
        el.setAttribute("aria-valuemin", String(MIN));
        el.setAttribute("aria-valuemax", String(MAX));
        el.setAttribute("tabindex", "0");

        var NS = "http://www.w3.org/2000/svg";
        var svg = document.createElementNS(NS, "svg");
        svg.setAttribute("width", "32"); svg.setAttribute("height", "32");
        svg.setAttribute("viewBox", "0 0 32 32");

        // 270-degree arc from 135deg round to 45deg
        var r = 12, cx = 16, cy = 16;
        var d = describeArc(cx, cy, r, -135, 135);

        var track = document.createElementNS(NS, "path");
        track.setAttribute("class", "ss-track");
        track.setAttribute("d", d);
        track.setAttribute("fill", "none");
        track.setAttribute("stroke-width", "3");
        track.setAttribute("stroke-linecap", "round");

        arc = document.createElementNS(NS, "path");
        arc.setAttribute("class", "ss-fill");
        arc.setAttribute("d", d);
        arc.setAttribute("fill", "none");
        arc.setAttribute("stroke-width", "3");
        arc.setAttribute("stroke-linecap", "round");

        needle = document.createElementNS(NS, "line");
        needle.setAttribute("class", "ss-needle");
        needle.setAttribute("stroke-width", "2.5");
        needle.setAttribute("stroke-linecap", "round");

        svg.appendChild(track); svg.appendChild(arc); svg.appendChild(needle);

        label = document.createElement("span");
        label.className = "ss-val";

        el.appendChild(svg); el.appendChild(label);
        wire(el);
        return el;
    }

    function polar(cx, cy, r, deg) {
        var a = (deg - 90) * Math.PI / 180;
        return { x: cx + r * Math.cos(a), y: cy + r * Math.sin(a) };
    }
    function describeArc(cx, cy, r, a0, a1) {
        // start at the low-left end and sweep clockwise through the top, so the
        // filled portion grows left -> right as speed increases
        var s = polar(cx, cy, r, a0), e = polar(cx, cy, r, a1);
        var big = (a1 - a0) <= 180 ? 0 : 1;
        return ["M", s.x, s.y, "A", r, r, 0, big, 1, e.x, e.y].join(" ");
    }

    function render() {
        if (!root) return;
        var t = toT(speed);
        if (ARC_LEN === 0 && arc.getTotalLength) ARC_LEN = arc.getTotalLength();
        if (ARC_LEN > 0) {
            arc.setAttribute("stroke-dasharray", ARC_LEN);
            arc.setAttribute("stroke-dashoffset", ARC_LEN * (1 - t));
        }
        var deg = -135 + 270 * t;
        var p1 = polar(16, 16, 4.5, deg), p2 = polar(16, 16, 10, deg);
        needle.setAttribute("x1", p1.x); needle.setAttribute("y1", p1.y);
        needle.setAttribute("x2", p2.x); needle.setAttribute("y2", p2.y);

        label.textContent = speed.toFixed(2).replace(/0$/, "") + "x";
        root.classList.toggle("ss-unity", Math.abs(speed - 1) < 0.005);
        root.setAttribute("aria-valuenow", speed.toFixed(2));
        root.title = engineUp ? tip() : "SpotiSpeed engine not running";
    }

    function set(v, persist) {
        speed = clampV(v);
        render();
        push(speed);
        if (persist !== false) {
            try { localStorage.setItem(STORE, String(speed)); } catch (e) {}
        }
    }

    function wire(el) {
        var dragging = false, moved = false, startY = 0, startT = 0, id = null;

        el.addEventListener("pointerdown", function (e) {
            if (e.button !== 0) return;
            dragging = true; moved = false;
            startY = e.clientY; startT = toT(speed); id = e.pointerId;
            el.setPointerCapture(id);
            el.classList.add("ss-active");
            e.preventDefault();
        });

        el.addEventListener("pointermove", function (e) {
            if (!dragging) return;
            var dy = startY - e.clientY;
            if (Math.abs(dy) > 2) moved = true;
            // ~1280px of travel spans the whole range; hold shift for fine control
            var span = e.shiftKey ? 3600 : 1280;
            set(toV(startT + dy / span));
        });

        function end() {
            if (!dragging) return;
            dragging = false;
            el.classList.remove("ss-active");
            if (id !== null) { try { el.releasePointerCapture(id); } catch (e) {} id = null; }
            if (!moved) set(1.0);          // plain click = reset to 1x
        }
        el.addEventListener("pointerup", end);
        el.addEventListener("pointercancel", end);

        el.addEventListener("wheel", function (e) {
            e.preventDefault();
            var step = e.shiftKey ? 0.0025 : 0.01;
            set(toV(toT(speed) + (e.deltaY < 0 ? step : -step)));
        }, { passive: false });

        el.addEventListener("keydown", function (e) {
            var step = e.shiftKey ? 0.005 : 0.02;
            if (e.key === "ArrowUp" || e.key === "ArrowRight") { set(toV(toT(speed) + step)); e.preventDefault(); }
            else if (e.key === "ArrowDown" || e.key === "ArrowLeft") { set(toV(toT(speed) - step)); e.preventDefault(); }
            else if (e.key === "Enter" || e.key === " ") { set(1.0); e.preventDefault(); }
        });

        el.addEventListener("dblclick", function (e) { e.preventDefault(); });
    }

    // ------------------------------------------------------------- mounting --
    function host() {
        return document.querySelector('[data-testid="general-controls"]')
            || document.querySelector('[data-testid="player-controls"]');
    }

    function mount() {
        var h = host();
        if (!h) return false;
        if (root && h.contains(root)) return true;
        css();
        if (!root) root = build();
        h.appendChild(root);
        render();
        return true;
    }

    function boot() {
        if (!mount()) { setTimeout(boot, 500); return; }
        push(speed);                       // apply the remembered rate
        // Spotify re-renders the footer; put the knob back when it does.
        var obs = new MutationObserver(function () { mount(); });
        obs.observe(document.body, { childList: true, subtree: true });
        poll();
        setInterval(poll, 2000);
        // Waking from sleep, or Spotify being backgrounded, can stall timers;
        // check as soon as the window is looked at again.
        document.addEventListener("visibilitychange", function () {
            if (!document.hidden) poll();
        });
        window.addEventListener("focus", poll);
    }

    boot();
})();
