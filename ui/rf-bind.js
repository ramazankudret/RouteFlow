// RouteFlow console — data binding.
//
// This file fills the design's hooks. It does not lay anything out, style
// anything, or decide what a screen looks like: it clones the <template>
// blocks the design ships and writes text and inline geometry into elements
// carrying data-rf="<field>". If a screen needs a new visual, that belongs in
// the design, not here.
//
// Three rules from the architecture are enforced here rather than left to the
// markup, because they are behavioural and a renderer is where they can
// actually be got wrong:
//
//   1. An unmeasured value renders as "—", never as 0 (§10, D8). A node with no
//      telemetry has no utilisation; drawing 0% would be a number nobody
//      measured, and the operator would act on it.
//   2. residency_known === false does not mean "nothing resident". The resident
//      list is replaced by an explicit unknown, never left empty (D3).
//   3. An omitted term is visually distinct from a zero term. t_load = 0 means
//      the model was warm; omitted means we could not see.

(function () {
  'use strict';

  var state = { nodes: [], jobs: [] };

  var API = {
    nodes: '/api/nodes',
    jobs: '/api/jobs?limit=200',
    events: '/events'
  };

  // A token is only needed when the router was started with one. The console is
  // served unauthenticated (it holds no cluster data), so it reads the token
  // from the URL once and keeps it for the session's fetches.
  var TOKEN = (function () {
    try {
      var q = new URLSearchParams(location.search).get('token');
      if (q) sessionStorage.setItem('rf_token', q);
      return sessionStorage.getItem('rf_token') || '';
    } catch (e) {
      return '';
    }
  })();

  function headers() {
    return TOKEN ? { Authorization: 'Bearer ' + TOKEN } : {};
  }

  // --- formatting -----------------------------------------------------------
  // Formats mirror the design's fixtures exactly. Changing one here changes the
  // look, which is the design's call, not this file's.

  var DASH = '—';

  function isMissing(v) {
    return v === null || v === undefined || (typeof v === 'number' && !isFinite(v));
  }

  function gib(bytes) {
    if (isMissing(bytes)) return DASH;
    return (bytes / 1073741824).toFixed(2) + ' GiB';
  }

  function pct(fraction, digits) {
    if (isMissing(fraction)) return DASH;
    return (fraction * 100).toFixed(digits === undefined ? 1 : digits) + '%';
  }

  // The design's own scale: milliseconds below a second, seconds above it with
  // two decimals. Deliberately not toLocaleString — on a European locale that
  // renders 1104 as "1.104 ms", which reads as one millisecond.
  function ms(value) {
    if (isMissing(value)) return DASH;
    if (value >= 1000) return (value / 1000).toFixed(2) + ' s';
    return Math.round(value) + ' ms';
  }

  function clockTime(iso) {
    if (!iso) return DASH;
    var d = new Date(iso);
    return isNaN(d.getTime()) ? DASH : d.toLocaleTimeString();
  }

  function watts(value) {
    return isMissing(value) ? DASH : value.toFixed(1);
  }

  function ago(epochMs) {
    if (isMissing(epochMs) || epochMs === 0) return DASH;
    var d = (Date.now() - epochMs) / 1000;
    if (d < 60) return Math.max(0, Math.round(d)) + 's ago';
    if (d < 3600) return Math.round(d / 60) + 'm ago';
    return Math.round(d / 3600) + 'h ago';
  }

  function shortId(id) {
    return typeof id === 'string' && id.length > 6 ? id.slice(-6) : (id || DASH);
  }

  // --- hook writing ---------------------------------------------------------

  // Writes a value into every [data-rf="name"] inside root.
  //
  // The element may carry an element child that is itself a hook — the node
  // inspector renders power as `15.7<u data-rf="power_cap_watts"> / 100 W</u>`.
  // Setting textContent would delete that child, so the leading text node is
  // replaced in place when one exists.
  function setHook(root, name, value, opts) {
    var missing = isMissing(value);
    var text = missing ? DASH : String(value);
    var list = root.querySelectorAll('[data-rf="' + name + '"]');
    for (var i = 0; i < list.length; i++) {
      var el = list[i];
      if (opts && opts.width) {
        el.style.width = missing ? '0%' : value;
        continue;
      }
      if (opts && opts.attr) {
        el.setAttribute(opts.attr, text);
        continue;
      }
      if (el.firstChild && el.firstChild.nodeType === 3) {
        el.firstChild.nodeValue = text;
      } else if (el.children.length === 0) {
        el.textContent = text;
      } else {
        el.insertBefore(document.createTextNode(text), el.firstChild);
      }
      // Rule 1: an unmeasured value is marked, so the design can dim it.
      el.classList.toggle('is-missing', missing);
    }
    return list.length;
  }

  function setClass(root, selector, cls, on) {
    var list = root.querySelectorAll(selector);
    for (var i = 0; i < list.length; i++) list[i].classList.toggle(cls, !!on);
  }

  function tpl(id) {
    var t = document.getElementById(id);
    return t && t.content ? t : null;
  }

  // importNode, not cloneNode.
  //
  // A <template>'s content lives in a separate inert document. cloneNode
  // returns a node still owned by that document, and while appending it does
  // adopt it, its CSS animations never get a start time — the design's `rf-in`
  // entrance sits at its 0% keyframe, which is opacity 0, and the card is
  // invisible forever. importNode brings the node into this document properly
  // and the animation runs like any other element's.
  function clone(id) {
    var t = tpl(id);
    return t ? document.importNode(t.content.firstElementChild, true) : null;
  }

  function clear(el) {
    while (el && el.firstChild) el.removeChild(el.firstChild);
  }

  function showEmpty(container, templateId) {
    var node = clone(templateId);
    if (node) {
      clear(container);
      container.appendChild(node);
    }
  }

  // --- node telemetry -------------------------------------------------------

  // Fills the telemetry hooks common to the card and the inspector. Whether a
  // value exists is decided by telemetry_ok, not by whether it happens to be
  // zero: an idle GPU genuinely reads 0%, and that is a different statement
  // from "this node has no telemetry backend" (rule 1).
  function fillTelemetry(root, node) {
    var live = node.telemetry_ok === true;
    setHook(root, 'gpu_util', live ? pct(node.gpu_util, 0) : null);
    setHook(root, 'temperature_c', live && !isMissing(node.temperature_c)
      ? Math.round(node.temperature_c) : null);
    setHook(root, 'power_watts', live ? watts(node.power_watts) : null);
    setHook(root, 'power_cap_watts', !isMissing(node.power_cap_watts) &&
      node.power_cap_watts > 0 ? ' / ' + Math.round(node.power_cap_watts) + ' W' : null);
    setHook(root, 'telemetry_backend', node.telemetry_backend || 'none');
    setClass(root, '[data-rf="telemetry_ok"]', 'is-on', live);
  }

  function fillVram(root, node) {
    var total = node.vram_total_bytes || 0;
    var free = node.vram_free_bytes || 0;
    var used = total > 0 ? (total - free) / total : null;

    var residentBytes = 0;
    (node.models_resident || []).forEach(function (m) {
      residentBytes += m.vram_bytes || 0;
    });
    var resident = total > 0 ? residentBytes / total : null;

    setHook(root, 'vram_total_bytes', gib(total));
    setHook(root, 'vram_free_bytes', gib(free));
    setHook(root, 'vram_used_pct', pct(used));
    setHook(root, 'vram_used_pct', isMissing(used) ? null : pct(used), { width: true });
    setHook(root, 'vram_resident_pct',
      isMissing(resident) ? null : pct(resident), { width: true });
  }

  function fillNodeCommon(root, node) {
    setHook(root, 'id', node.id);
    setHook(root, 'ip', node.endpoint || node.dispatch_endpoint || DASH);
    setHook(root, 'gpu_name', node.gpu_name || DASH);
    setHook(root, 'engine', node.engine || 'none');
    setHook(root, 'engine_slots', ' / ' + (node.engine_slots || 1) + ' slots');
    setHook(root, 'inflight', node.inflight === undefined ? null : node.inflight);
    fillTelemetry(root, node);
    fillVram(root, node);

    setClass(root, '[data-rf="engine_healthy"]', 'is-on', node.engine_healthy === true);
    setClass(root, '[data-rf="residency_known"]', 'is-on', node.residency_known !== false);
    setClass(root, '.node', 'is-down', node.engine_healthy !== true);
  }

  // --- cluster --------------------------------------------------------------

  // Where to put a node card.
  //
  // The design hand-placed its fixture cluster, and each screen frames the world
  // differently — the jobs page gives half its width to the feed, so positions
  // that suit the cluster page fall outside the view there. Rather than invent a
  // layout per screen, the slots the designer chose on *this* page are captured
  // before the fixtures are removed and reused in order. A ring is the fallback
  // for a cluster larger than the design drew for, and only then.
  var SLOTS = null;

  function captureSlots(world) {
    if (SLOTS) return SLOTS;
    SLOTS = [];
    var fixtures = world.querySelectorAll('.node[data-node]');
    for (var i = 0; i < fixtures.length; i++) {
      SLOTS.push({
        left: fixtures[i].style.left,
        top: fixtures[i].style.top
      });
    }
    return SLOTS;
  }

  function ringPosition(index, count) {
    var cx = 46, cy = 47, rx = 30, ry = 31;
    if (count === 1) return { left: cx + rx * 0.6 + '%', top: cy + '%' };
    var angle = (-Math.PI / 2) + (index / count) * Math.PI * 2;
    return {
      left: (cx + rx * Math.cos(angle)).toFixed(2) + '%',
      top: (cy + ry * Math.sin(angle)).toFixed(2) + '%'
    };
  }

  function slotFor(index, count) {
    if (SLOTS && index < SLOTS.length && SLOTS[index].left) return SLOTS[index];
    return ringPosition(index, count);
  }

  // Cards are updated in place, never rebuilt.
  //
  // Rebuilding them on every poll restarts the design's entrance animation —
  // `rf-in`, which begins at opacity 0 with fill-mode both — so the cluster
  // flickers back to invisible on every update, and with a fast enough stream
  // never appears at all. That is what the first live render did. A node is
  // created when it appears in the registry and removed when it leaves;
  // everything else is a value written into an element that is already there.
  function renderCluster(nodes) {
    var world = document.getElementById('rf-world');
    if (!world) return;

    // Captured before the fixture cards are removed below — after that the
    // designer's chosen positions are gone.
    captureSlots(world);

    var seen = {};
    nodes.forEach(function (n) { seen[n.id] = true; });

    // Only cluster nodes are ours to manage. The hub and the client cards
    // carry the same .node class but no data-node, and they are part of the
    // composition, not the data — an earlier version of this loop deleted them
    // and left the canvas showing nothing but the link curve.
    var existing = world.querySelectorAll('.node[data-node]');
    for (var i = 0; i < existing.length; i++) {
      if (!seen[existing[i].getAttribute('data-node')]) existing[i].remove();
    }

    if (!nodes.length) {
      var hub = world.querySelector('[data-rf-item="hub"]');
      showEmpty(hub ? hub.parentNode : world, 'rf-tpl-empty-nodes');
      return;
    }

    nodes.forEach(function (node, index) {
      var card = world.querySelector('.node[data-node="' + node.id + '"]');
      if (!card) {
        card = clone('rf-tpl-node');
        if (!card) return;
        card.setAttribute('data-node', node.id);
        card.style.setProperty('--i', String(index));
        world.appendChild(card);
      }
      // Recomputed each pass because the fallback ring depends on how many
      // nodes there are, and that changes when one joins or leaves.
      var pos = slotFor(index, nodes.length);
      card.style.left = pos.left;
      card.style.top = pos.top;

      fillNodeCommon(card, node);

      // Rule 2. An engine that cannot report residency is not a node with an
      // empty resident list; the two look identical if you let them.
      //
      // Toggled rather than removed: a node can go from reporting residency to
      // not reporting it and back, and an element deleted on the first pass is
      // not there to fill on the second.
      var res = card.querySelector('.m-res');
      var none = card.querySelector('.m-none');
      var unknown = node.residency_known === false;
      var resident = node.models_resident || [];
      if (res) {
        res.hidden = unknown;
        res.textContent = resident.length
          ? '● ' + resident.map(function (m) {
              return m.name + ' · ' + gib(m.vram_bytes);
            }).join(' · ')
          : 'nothing resident';
      }
      if (none) {
        none.hidden = !unknown;
        none.textContent = 'residency unknown';
      }
      setHook(card, 'models_on_disk',
        (node.models_on_disk || []).join(' · ') + ' · on disk');
    });
  }

  // The page's click handler shows the panel whose data-insp matches the node
  // and hides the rest, so there has to be one panel per node. This rendered
  // only the first one, which meant clicking any other node hid the single
  // panel and left the inspector blank until the next poll rebuilt it -- a
  // console that blinks out when you use it.
  //
  // Panels are created once and updated in place afterwards, for the same
  // reason the cards above are: a rebuild restarts the entrance animation and
  // throws away which node the operator had selected.
  function renderInspectors(nodes) {
    var host = document.getElementById('rf-insp');
    if (!host || !document.getElementById('rf-tpl-node-inspector')) return;

    var seen = {};
    nodes.forEach(function (n) { seen[n.id] = true; });

    var existing = host.querySelectorAll('[data-insp]');
    var byId = {};
    for (var i = 0; i < existing.length; i++) {
      var id = existing[i].getAttribute('data-insp');
      // A fixture panel from the static markup names a node that is not in the
      // registry; it goes, along with any node that has actually left.
      if (!seen[id]) { existing[i].remove(); continue; }
      byId[id] = existing[i];
    }

    nodes.forEach(function (n) {
      var panel = byId[n.id];
      if (!panel) {
        panel = clone('rf-tpl-node-inspector');
        if (!panel) return;
        panel.setAttribute('data-insp', n.id);
        panel.hidden = true;   // the selection below decides what is shown
        host.appendChild(panel);
      }
      fillInspector(panel, n);
    });

    // Something must be visible. If the operator has not chosen, or the node
    // they chose has gone, fall back to the first -- but never override a
    // choice that is still valid, which is what made the click flicker.
    var panels = host.querySelectorAll('[data-insp]');
    var anyVisible = false;
    for (var k = 0; k < panels.length; k++) if (!panels[k].hidden) anyVisible = true;
    if (!anyVisible && panels.length) {
      panels[0].hidden = false;
      var first = panels[0].getAttribute('data-insp');
      var cards = document.querySelectorAll('.node[data-node]');
      for (var m = 0; m < cards.length; m++) {
        cards[m].classList.toggle('is-selected',
                                  cards[m].getAttribute('data-node') === first);
      }
    }
  }

  function fillInspector(panel, node) {
    if (!panel || !node) return;
    fillNodeCommon(panel, node);

    var disk = panel.querySelector('.mcol.is-disk');
    var resCol = panel.querySelector('.mcol:not(.is-disk)');
    var sizes = node.model_disk_bytes || {};
    var residentNames = {};
    (node.models_resident || []).forEach(function (m) { residentNames[m.name] = true; });

    if (disk) {
      var diskRows = disk.querySelectorAll('[data-rf-item="model"]');
      for (var i = 0; i < diskRows.length; i++) diskRows[i].remove();
      var onDisk = (node.models_on_disk || []).filter(function (n) {
        return !residentNames[n];
      });
      var lbl = disk.querySelector('.lbl');
      if (lbl) lbl.textContent = 'on disk · ' + onDisk.length;
      onDisk.forEach(function (name) {
        var row = clone('rf-tpl-model-disk-row');
        if (!row) return;
        setHook(row, 'name', name);
        setHook(row, 'disk_bytes', sizes[name] ? gib(sizes[name]) : null);
        disk.appendChild(row);
      });
    }

    if (resCol) {
      var resRows = resCol.querySelectorAll('[data-rf-item="model"]');
      for (var j = 0; j < resRows.length; j++) resRows[j].remove();
      var label = resCol.querySelector('.lbl');
      if (node.residency_known === false) {
        if (label) label.textContent = 'residency unknown';
      } else {
        var resident = node.models_resident || [];
        if (label) label.textContent = 'resident in vram · ' + resident.length;
        resident.forEach(function (m) {
          var row = clone('rf-tpl-model-resident-row');
          if (!row) return;
          setHook(row, 'name', m.name);
          setHook(row, 'vram_bytes', gib(m.vram_bytes));
          setHook(row, 'last_used_ms', ago(m.last_used_ms));
          resCol.appendChild(row);
        });
      }
    }

  }

  // --- jobs -----------------------------------------------------------------

  function renderJobs(jobs) {
    var feed = document.getElementById('rf-feed');
    if (!feed) return;
    clear(feed);
    if (!jobs.length) {
      showEmpty(feed, 'rf-tpl-empty-jobs');
      return;
    }
    jobs.forEach(function (job) {
      var row = clone('rf-tpl-job-row');
      if (!row) return;
      row.setAttribute('data-job', job.job_id);
      setHook(row, 'job_id', shortId(job.job_id));
      setHook(row, 'model', job.model);
      setHook(row, 'role_hint', job.role_hint || DASH);
      setHook(row, 'node_id', job.node_id || DASH);
      setHook(row, 'started_at', clockTime(job.ts_received));
      setHook(row, 'ttft_ms', ms(job.ttft_ms));
      setHook(row, 'total_ms', ms(job.total_ms));
      setHook(row, 'predicted_total_ms', ms(job.predicted_total_ms));
      setHook(row, 'outcome', job.outcome);
      setHook(row, 'decided_by', job.decided_by || DASH);
      setHook(row, 'was_resident', job.was_resident ? 'warm' : 'cold');
      row.classList.toggle('is-warm', job.was_resident === true);
      row.classList.toggle('is-cold', job.was_resident === false);
      row.classList.toggle('is-failed', job.outcome !== 'ok');
      feed.appendChild(row);
    });
  }

  // --- decision -------------------------------------------------------------

  // A Decision candidate carries scheduling state, not hardware description;
  // the console has the node list anyway, so the two are joined here rather
  // than widening the trace record for a label.
  function node_by_id(id) {
    for (var i = 0; i < state.nodes.length; i++) {
      if (state.nodes[i].id === id) return state.nodes[i];
    }
    return null;
  }
  function node_gpu_name(id) { var n = node_by_id(id); return n && n.gpu_name; }
  function node_engine(id) { var n = node_by_id(id); return n && n.engine; }
  function node_slots(id) { var n = node_by_id(id); return n && n.engine_slots; }

  // A term appears twice in the candidate row: once as a bar segment carrying
  // geometry, once as a legend entry carrying text. Both hooks share the same
  // data-rf name, so writing by name alone stuffs numbers into the bars — which
  // is exactly what the first version did, and it looked like corruption.
  function fillTerm(row, term, value, omitted, scaleMs) {
    var seg = row.querySelector('.seg-' + term);
    if (seg) {
      seg.style.width = omitted ? '0%'
        : ((value || 0) / scaleMs * 100).toFixed(2) + '%';
      seg.classList.toggle('is-zero', !omitted && !value);
      seg.classList.toggle('is-omitted', omitted);
    }
    var label = row.querySelector('.term[data-rf="' + term + '"]');
    if (label) {
      var b = label.querySelector('b');
      // Rule 3: "omitted" is a different statement from "0 ms". A term with no
      // signal must not read as a term that cost nothing.
      if (b) b.textContent = omitted ? 'omitted' : ms(value);
      label.classList.toggle('is-zero', !omitted && !value);
      label.classList.toggle('is-omitted', omitted);
    }
  }

  // The margin line is a sentence with two numbers in it, hooked as one field.
  // Writing the value over its first text node turns "margin 4.23 s against the
  // largest sigma of 2.40 s" into "173 ms4.23 s against..." — so the two <b>
  // elements are filled and the verdict clause is rewritten instead.
  function fillMarginLine(host, job, candidates) {
    var el = host.querySelector('.dh-v');
    if (!el) return;
    var bs = el.querySelectorAll('b');
    var sigma = 0;
    candidates.forEach(function (c) { sigma = Math.max(sigma, c.sigma_ms || 0); });
    var noise = job.decided_by === 'within_noise';
    if (bs[0]) bs[0].textContent = ms(job.margin_ms);
    if (bs[1]) bs[1].textContent = ms(sigma);
    var verdict = candidates.length < 2
      ? ' — nothing to compare it against'
      : (noise ? ' — the ranking is inside the noise'
               : ' — the ranking survives the noise');
    // Replace only the trailing clause, after the second bold value.
    var last = el.lastChild;
    if (last && last.nodeType === 3) last.nodeValue = verdict;
    el.classList.toggle('is-noise', noise);
  }

  // The design leaves a sentence explaining the decision. Fixture prose that
  // names nodes which are not on screen is worse than none, so it is written
  // from the decision itself — the same terms the bars are drawn from.
  function fillThesis(host, job, candidates) {
    var el = host.querySelector('.thesis');
    if (!el) return;
    var winner = null, runner = null;
    candidates.forEach(function (c) {
      if (c.node_id === job.node_id) winner = c;
      else if (!runner || c.predicted_total_ms < runner.predicted_total_ms) runner = c;
    });
    if (!winner) { el.textContent = ''; return; }

    var w = '<b>' + winner.node_id + '</b>';
    var text;
    if (!runner) {
      text = w + ' was the only node that could serve this request; the others ' +
             'were rejected before scoring.';
    } else {
      var r = '<b>' + runner.node_id + '</b>';
      if (job.decided_by === 'within_noise') {
        text = w + ' was chosen over ' + r + ' by ' + ms(job.margin_ms) +
               ', which is smaller than the uncertainty on either estimate. ' +
               'The scheduler took the lower number, but it cannot tell these ' +
               'two apart.';
      } else if (job.decided_by === 't_load') {
        text = w + ' already holds the model; ' + r + ' would spend <b>' +
               ms(runner.t_load) + '</b> loading it first. Residency decides ' +
               'this, not raw speed.';
      } else if (job.decided_by === 't_evict') {
        text = w + ' can take this without displacing anything warm, while ' +
               r + ' would have to evict a model that was just used.';
      } else if (job.decided_by === 't_queue') {
        text = r + ' is already busy — ' + ms(runner.t_queue) +
               ' of queue ahead of this request — so ' + w + ' finishes sooner ' +
               'even though the work itself is the same.';
      } else {
        text = w + ' generates faster for this model: ' + ms(winner.t_decode) +
               ' of decode against ' + ms(runner.t_decode) + ' on ' + r +
               '. Nothing needed loading on either.';
      }
    }
    el.innerHTML = text;
  }

  function renderDecisionPanel(host, job) {
    if (!host) return;
    if (!job) {
      showEmpty(host, 'rf-tpl-empty-decision');
      host.removeAttribute('data-job');
      return;
    }

    // A decision is finished. Its candidates, their terms and their bands were
    // fixed the moment the router chose, and nothing that arrives later can
    // change them -- so re-rendering the same job is not just wasted work, it
    // tears down the bars and builds them again on every poll, restarting the
    // entrance animation. On screen that reads as a chart that will not sit
    // still. Only a *different* job in this panel is worth redrawing.
    //
    // Same lesson as renderCluster's cards, which carries the same paragraph.
    if (host.getAttribute('data-job') === job.job_id) return;
    host.setAttribute('data-job', job.job_id);

    var admitted = (job.candidates || []).filter(function (c) { return c.admitted; });

    setHook(host, 'job_id', shortId(job.job_id));
    // The one field in this meta line that was never hooked, sitting between
    // three that were. D15: the estimate is what the router scheduled on and
    // the actual is what the engine counted, so when both exist the actual is
    // shown and the title says the estimate it was scheduled against.
    var actual = job.prompt_tokens_actual;
    var est = job.prompt_tokens_est;
    var shown = isMissing(actual) ? est : actual;
    setHook(host, 'prompt_tokens', isMissing(shown) ? null : shown + ' tok');
    var promptEl = host.querySelector('[data-rf="prompt_tokens"]');
    if (promptEl) {
      promptEl.title = isMissing(actual)
        ? 'estimated; the engine reported no prompt token count'
        : 'counted by the engine; the router scheduled against ' + est;
    }
    setHook(host, 'candidate_count', (job.candidates || []).length);
    setHook(host, 'admitted_count', admitted.length);
    setHook(host, 'model', job.model);
    setHook(host, 'role_hint', job.role_hint || DASH);
    setHook(host, 'prompt_tokens_actual',
      job.prompt_tokens_actual || job.prompt_tokens_est);
    setHook(host, 'decided_by', job.decided_by || DASH);
    fillMarginLine(host, job, admitted);
    fillThesis(host, job, admitted);
    // The scheduler saying it could not tell two candidates apart is a state
    // the design draws differently, and it must not be smoothed over.
    host.classList.toggle('is-noise', job.decided_by === 'within_noise');

    var list = host.querySelector('[data-rf-list="candidates"]') ||
               (host.querySelector('[data-rf-item="candidate"]') || {}).parentNode ||
               host;
    var old = list.querySelectorAll('[data-rf-item="candidate"]');
    for (var i = 0; i < old.length; i++) old[i].remove();

    // One shared axis across every candidate, including the uncertainty bands:
    // without it the longest bar is always full width and the picture says
    // nothing about how the options actually compare.
    var scaleMs = 1;
    admitted.forEach(function (c) {
      scaleMs = Math.max(scaleMs, (c.predicted_total_ms || 0) + (c.sigma_ms || 0) / 2);
    });

    var TERMS = ['t_queue', 't_load', 't_prefill', 't_decode', 't_evict'];

    (job.candidates || []).forEach(function (c) {
      var row = clone(c.admitted ? 'rf-tpl-candidate-admitted'
                                 : 'rf-tpl-candidate-rejected');
      if (!row) return;
      row.setAttribute('data-node', c.node_id);
      setHook(row, 'node_id', c.node_id);
      setHook(row, 'vram_free', gib(c.vram_free));
      setHook(row, 'gpu_util', isMissing(c.gpu_util) ? null : pct(c.gpu_util, 0));
      setHook(row, 'was_resident', c.was_resident ? 'warm' : 'cold');
      setHook(row, 'would_evict', (c.would_evict || []).join(' · ') || DASH);

      var cgpu = row.querySelector('.cgpu');
      if (cgpu) {
        cgpu.textContent = (c.gpu_name || node_gpu_name(c.node_id) || 'unknown gpu') +
          ' · ' + (node_engine(c.node_id) || 'engine') +
          ' · ' + (node_slots(c.node_id) || 1) + ' slots';
      }
      var inflightEl = row.querySelector('[data-rf="inflight"]');
      if (inflightEl) {
        inflightEl.textContent = (c.inflight || 0) + ' / ' + (node_slots(c.node_id) || 1);
      }

      if (!c.admitted) {
        setHook(row, 'reason', c.reason);
        row.setAttribute('data-reason', c.reason);
      } else {
        var omitted = c.omitted || [];
        TERMS.forEach(function (term) {
          fillTerm(row, term, c[term], omitted.indexOf(term) !== -1, scaleMs);
        });

        var total = c.predicted_total_ms || 0;
        var tt = row.querySelector('.ctotal .tt');
        if (tt) tt.textContent = ms(total);
        var ts = row.querySelector('.ctotal .ts');
        if (ts) ts.textContent = '± ' + ms(c.sigma_ms) + ' · 1σ';
        setHook(row, 'conf', c.conf || 'seeded');
        setHook(row, 'samples', c.samples);

        var pos = (total / scaleMs * 100);
        var mark = row.querySelector('.whisk .mark');
        if (mark) mark.style.left = pos.toFixed(2) + '%';
        var guide = row.querySelector('.guide');
        if (guide) guide.style.left = pos.toFixed(2) + '%';
        // The uncertainty band is the point of the whisker: where two bands
        // overlap, the ranking is not distinguishable from noise (D8).
        var band = row.querySelector('.whisk .band');
        if (band) {
          var half = (c.sigma_ms || 0) / 2 / scaleMs * 100;
          band.style.left = Math.max(0, pos - half).toFixed(2) + '%';
          band.style.width = (half * 2).toFixed(2) + '%';
        }
      }
      row.classList.toggle('is-winner', c.node_id === job.node_id);
      list.appendChild(row);
    });
  }

  // The screen has three panels behind three tabs. The design used them to show
  // three different situations; live, they show the three most recent decisions,
  // and each tab is labelled with what actually decided that one.
  function renderDecision(jobs) {
    var panels = document.querySelectorAll('[data-rf-item="decision"]');
    var tabs = document.querySelectorAll('#rf-tabs .tab');
    for (var i = 0; i < panels.length; i++) {
      var job = jobs[i] || null;
      renderDecisionPanel(panels[i], job);
      var tab = tabs[i];
      if (!tab) continue;
      var id = tab.querySelector('.t-id');
      var nm = tab.querySelector('.t-nm');
      if (id) id.textContent = job ? shortId(job.job_id) : '—';
      if (nm) nm.textContent = job ? (job.decided_by || 'no decision') : 'no job yet';
      tab.disabled = !job;
    }
  }

  // --- accuracy -------------------------------------------------------------

  function median(values) {
    if (!values.length) return null;
    var s = values.slice().sort(function (a, b) { return a - b; });
    var mid = Math.floor(s.length / 2);
    return s.length % 2 ? s[mid] : (s[mid - 1] + s[mid]) / 2;
  }

  // Two plots, two errors, kept apart on purpose (§8): timing error moves with
  // load and page cache, length error with a few runaway generations. Folding
  // them into one number would hide both, so each panel gets its own axis and
  // its own points.
  // A round ceiling above the data. Scaling an axis to the largest point exactly
  // puts that point on the border where it is half clipped, and labels the axis
  // with a number nobody chose.
  function niceMax(value) {
    if (!(value > 0)) return 1;
    var mag = Math.pow(10, Math.floor(Math.log(value) / Math.LN10));
    var steps = [1, 1.5, 2, 2.5, 3, 4, 5, 7.5, 10];
    for (var i = 0; i < steps.length; i++) {
      if (steps[i] * mag >= value) return steps[i] * mag;
    }
    return 10 * mag;
  }

  // The axis ticks are drawn as fixture values - 0 to 16 s, 0 to 1200 tokens.
  // Live data has its own range, so leaving them as drawn would put a 150-token
  // reply at "40% of 1200" and the plot would be wrong in the one way a plot
  // must never be. Each tick already carries its own position, so its label is
  // just that fraction of the new ceiling.
  function rescaleAxis(ticks, max, fmt) {
    for (var i = 0; i < ticks.length; i++) {
      var css = ticks[i].style.bottom || ticks[i].style.left;
      var frac = parseFloat(css) / 100;
      if (isFinite(frac)) ticks[i].textContent = fmt(max * frac);
    }
  }

  function fillScatter(section, points, label) {
    if (!section) return;
    // Dots are positioned against .plot-area. `.plot` is a two-column grid with
    // no positioning of its own, so appending there placed every point against
    // an ancestor further up and off the axes entirely - bound, and invisible.
    var area = section.querySelector('.plot-area');
    if (!area) return;

    var old = area.querySelectorAll('[data-rf-item="trace"]');
    for (var i = 0; i < old.length; i++) old[i].remove();
    var stale = area.querySelector('.plabel');
    if (stale) stale.remove();

    var n = section.querySelector('.pan-h .n');
    if (n) n.textContent = 'n ' + points.length;
    if (!points.length) return;

    // A shared max on both axes keeps the 1:1 reference line meaningful; a
    // point above it ran longer than predicted, which is the whole reading.
    var max = 1;
    points.forEach(function (p) { max = Math.max(max, p.x, p.y); });
    max = niceMax(max);
    rescaleAxis(section.querySelectorAll('.plot-y span'), max, label.axis);
    rescaleAxis(section.querySelectorAll('.plot-x span'), max, label.axis);

    var worst = null;
    points.forEach(function (p) {
      var dot = clone('rf-tpl-scatter-point');
      if (!dot) return;
      dot.style.left = (p.x / max * 100).toFixed(2) + '%';
      dot.style.bottom = (p.y / max * 100).toFixed(2) + '%';
      dot.classList.toggle('is-warm', !!p.warm);
      dot.classList.toggle('is-cold', !p.warm);
      // The template carries the timing panel's two field names. On the length
      // panel they are different fields, so the hooks are renamed rather than
      // left pointing at quantities this dot does not hold.
      var slots = dot.querySelectorAll('[data-rf]');
      if (slots.length >= 2) {
        slots[0].setAttribute('data-rf', label.x);
        slots[1].setAttribute('data-rf', label.y);
      }
      setHook(dot, label.x, label.fmt(p.x));
      setHook(dot, label.y, label.fmt(p.y));
      dot.title = shortId(p.id) + ' \u00b7 predicted ' + label.fmt(p.x) +
                  ' \u00b7 measured ' + label.fmt(p.y);
      area.appendChild(dot);
      if (!worst || relErr(p) > relErr(worst)) worst = p;
    });

    // The design annotates the worst outlier by name. It was drawn against a
    // fixture job; pointing it at the real one is what makes it true.
    if (worst && relErr(worst) > 0.1) {
      var note = document.createElement('span');
      note.className = 'plabel';
      note.style.left = (worst.x / max * 100).toFixed(2) + '%';
      note.style.bottom = (worst.y / max * 100).toFixed(2) + '%';
      note.textContent = shortId(worst.id) + ' \u00b7 ' + label.note(worst);
      area.appendChild(note);
    }
  }

  function relErr(p) { return p.y > 0 ? Math.abs(p.x - p.y) / p.y : 0; }

  function signedPct(p) {
    var d = (p.y - p.x) / p.x;
    return (d >= 0 ? '+' : '') + (d * 100).toFixed(0) + '%';
  }

  function renderAccuracy(jobs) {
    var ok = jobs.filter(function (j) { return j.outcome === 'ok'; });

    var timing = ok.filter(function (j) {
      return j.predicted_total_ms && j.total_ms;
    }).map(function (j) {
      return { id: j.job_id, x: j.predicted_total_ms, y: j.total_ms,
               warm: !!j.was_resident };
    });
    var length = ok.filter(function (j) {
      return j.predicted_output_tokens && j.output_tokens;
    }).map(function (j) {
      return { id: j.job_id, x: j.predicted_output_tokens, y: j.output_tokens,
               warm: !!j.was_resident };
    });

    // No early return on an empty window. Returning here used to leave every
    // other panel on this screen showing the numbers it was drawn with, so a
    // scheduler that had run nothing at all reported a full set of
    // measurements — a median error, a worst case, five named nodes. Each fill
    // below states its own emptiness instead.
    emptyCard(document.querySelector('[data-rf-item="accuracy_total"] .plot-area'),
              !timing.length);
    emptyCard(document.querySelector('[data-rf-item="accuracy_tokens"] .plot-area'),
              !length.length);

    var tokens = function (v) { return Math.round(v) + ' tok'; };
    fillScatter(document.querySelector('[data-rf-item="accuracy_total"]'), timing,
      { x: 'predicted_total_ms', y: 'total_ms', fmt: ms, axis: ms,
        note: function (p) {
          return (p.warm ? 'warm' : 'cold') + ' \u00b7 ' + signedPct(p);
        } });
    fillScatter(document.querySelector('[data-rf-item="accuracy_tokens"]'), length,
      { x: 'predicted_output_tokens', y: 'output_tokens', fmt: tokens,
        axis: function (v) { return Math.round(v) + ''; },
        note: function (p) {
          return Math.round(p.x) + ' \u2192 ' + Math.round(p.y) + ' tok';
        } });

    fillTimingStats(timing);
    fillLengthStats(length);
    fillErrorSpread(timing);
    fillErrorByNode(ok);
    fillDrift(ok);
  }

  // --- signed error spread ---------------------------------------------------

  var BUCKETS = 7;
  var HIST_PX = 190;   // .hist is 190px tall and aligns its bars to the bottom

  // Bucket edges come from the data, but zero is always one of them. The panel
  // reads "left of zero the job finished early", so a bucket straddling zero
  // would put early and late jobs in the same bar and the whole panel would
  // stop meaning what it says. Every edge is a multiple of the width, so zero
  // is an edge for free.
  function bucketWidth(min, max) {
    var steps = [0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 5, 10];
    for (var i = 0; i < steps.length; i++) {
      if (Math.floor(min / steps[i]) * steps[i] + BUCKETS * steps[i] >= max) {
        return steps[i];
      }
    }
    return steps[steps.length - 1];
  }

  function signedPctLabel(v) {
    // Rounded first, so a boundary that floating-point arithmetic left at
    // -1e-15 prints as 0% and not as -0%.
    var n = Math.round(v * 100);
    if (n === 0) return '0%';
    return (n > 0 ? '+' : '') + n + '%';
  }

  function fillErrorSpread(points) {
    var section = document.querySelector('[data-rf-item="accuracy_error_dist"]');
    if (!section) return;
    var hist = section.querySelector('.hist');
    var axis = section.querySelector('.hx');
    if (!hist || !axis) return;
    clear(hist);
    clear(axis);

    setHook(section, 'dist_count', points.length);
    setHook(section, 'dist_n', 'n ' + points.length +
      (points.length ? '' : ' \u00b7 nothing to bucket'));
    if (!points.length) {
      setHook(section, 'dist_warm', 'warm \u00b7 0');
      setHook(section, 'dist_cold', 'cold \u00b7 0');
      setHook(section, 'dist_summary', 'no finished jobs in this window');
      return;
    }

    var signed = points.map(signedErr);
    // Width comes from the middle of the distribution, not its extremes. One
    // job that ran three times its estimate would otherwise set a bucket width
    // wide enough to put every other job in two bars, and the panel exists to
    // show shape. Anything outside the range is clamped into the end buckets,
    // and those buckets say so rather than claiming a range they do not hold.
    var min = percentileOf(signed, 0.05);
    var max = percentileOf(signed, 0.95);
    var w = bucketWidth(min, max);
    var lo = Math.floor(min / w) * w;
    var under = 0, over = 0;

    var cells = [];
    for (var i = 0; i < BUCKETS; i++) cells.push({ warm: 0, cold: 0 });
    points.forEach(function (p) {
      var index = Math.floor((signedErr(p) - lo) / w);
      if (index < 0) { index = 0; under++; }
      if (index >= BUCKETS) { index = BUCKETS - 1; over++; }
      cells[index][p.warm ? 'warm' : 'cold']++;
    });

    var tallest = 1;
    cells.forEach(function (c) { tallest = Math.max(tallest, c.warm + c.cold); });

    cells.forEach(function (cell, index) {
      var col = clone('rf-tpl-bucket');
      var label = clone('rf-tpl-bucket-label');
      if (!col || !label) return;
      var total = cell.warm + cell.cold;
      setHook(col, 'count', total ? total : '');
      // Selected by class, not by hook name: these two bars are geometry, and
      // the same hook names carry text elsewhere on the page.
      var cold = col.querySelector('.h-cold');
      var warm = col.querySelector('.h-warm');
      cold.style.height = (cell.cold / tallest * HIST_PX).toFixed(1) + 'px';
      warm.style.height = (cell.warm / tallest * HIST_PX).toFixed(1) + 'px';
      cold.classList.toggle('h-zero', cell.cold === 0);
      warm.classList.toggle('h-zero', cell.warm === 0);
      hist.appendChild(col);

      // setHook searches *inside* a root; the label element is the hook
      // itself, so writing through it left all seven reading the fixture.
      var from = lo + index * w, to = from + w;
      label.textContent =
        index === 0 && under ? '\u2264 ' + signedPctLabel(to)
        : index === BUCKETS - 1 && over ? '\u2265 ' + signedPctLabel(from)
        : signedPctLabel(from) + ' \u2026 ' + signedPctLabel(to);
      axis.appendChild(label);
    });

    var warmN = points.filter(function (p) { return p.warm; }).length;
    var early = points.filter(function (p) { return p.y < p.x; }).length;
    setHook(section, 'dist_warm', 'warm \u00b7 ' + warmN);
    setHook(section, 'dist_cold', 'cold \u00b7 ' + (points.length - warmN));
    setHook(section, 'dist_summary',
      'median ' + signedPctLabel(percentileOf(signed, 0.5)) +
      ' \u00b7 p90 ' + signedPctLabel(percentileOf(signed, 0.9)) +
      ' \u00b7 ' + early + ' of ' + points.length + ' finished early');
  }

  // --- error by node ---------------------------------------------------------

  function fillErrorByNode(jobs) {
    var section = document.querySelector('[data-rf-item="accuracy_by_node"]');
    if (!section) return;
    var list = section.querySelector('.dumb');
    if (!list) return;
    clear(list);

    var byNode = {};
    state.nodes.forEach(function (n) {
      byNode[n.id] = { node: n, warm: [], cold: [] };
    });
    jobs.forEach(function (j) {
      if (!j.node_id || !j.predicted_total_ms || !j.total_ms) return;
      var row = byNode[j.node_id];
      // A trace can name a node the registry no longer lists; it is still a
      // measurement and dropping it would quietly shrink the sample.
      if (!row) row = byNode[j.node_id] = { node: { id: j.node_id }, warm: [], cold: [] };
      row[j.was_resident ? 'warm' : 'cold'].push(
        Math.abs(j.predicted_total_ms - j.total_ms) / j.total_ms);
    });

    var ids = Object.keys(byNode).sort();
    // A floor on the axis so a well-calibrated cluster is not magnified into
    // looking erratic; above that the axis follows the data.
    var top = 0.1;
    ids.forEach(function (id) {
      var r = byNode[id];
      if (r.warm.length) top = Math.max(top, median(r.warm));
      if (r.cold.length) top = Math.max(top, median(r.cold));
    });
    top = niceMax(top);

    var axisLabels = section.querySelectorAll('[data-rf="node_axis"]');
    for (var a = 0; a < axisLabels.length; a++) {
      var frac = axisLabels.length > 1 ? a / (axisLabels.length - 1) : 0;
      axisLabels[a].textContent = a === 0 ? '0' : pct(top * frac, 0);
    }

    ids.forEach(function (id) {
      var r = byNode[id];
      var row = clone('rf-tpl-node-error');
      if (!row) return;
      setHook(row, 'node_id', id);

      var warmErr = r.warm.length ? median(r.warm) : null;
      var coldErr = r.cold.length ? median(r.cold) : null;
      var warmDot = row.querySelector('.dpt.is-warm');
      var coldDot = row.querySelector('.dpt.is-cold');
      var link = row.querySelector('.dlink');

      if (warmErr === null) {
        warmDot.classList.add('is-none');
        warmDot.classList.remove('is-warm');
        warmDot.hidden = true;
      } else {
        warmDot.style.left = Math.min(100, warmErr / top * 100).toFixed(2) + '%';
      }
      if (coldErr === null) {
        coldDot.classList.add('is-none');
        coldDot.classList.remove('is-cold');
        coldDot.hidden = true;
      } else {
        coldDot.style.left = Math.min(100, coldErr / top * 100).toFixed(2) + '%';
      }
      if (warmErr !== null && coldErr !== null) {
        var a1 = Math.min(warmErr, coldErr) / top * 100;
        var a2 = Math.max(warmErr, coldErr) / top * 100;
        link.style.left = a1.toFixed(2) + '%';
        link.style.width = Math.max(0, a2 - a1).toFixed(2) + '%';
      } else {
        link.hidden = true;
      }

      setPart(row, 'warm_part', warmErr !== null);
      setPart(row, 'cold_part', coldErr !== null);
      setPart(row, 'no_data', warmErr === null && coldErr === null);
      if (warmErr !== null) {
        setHook(row, 'warm_err', pct(warmErr));
        setHook(row, 'warm_n', r.warm.length);
      }
      if (coldErr !== null) {
        setHook(row, 'cold_err', pct(coldErr));
        setHook(row, 'cold_n', r.cold.length);
      }

      var note = row.querySelector('[data-rf="node_note"]');
      var text = null;
      if (warmErr === null && coldErr === null) {
        text = 'no traces in this window';
      } else if (warmErr === null) {
        text = r.node && r.node.residency_known === false
          ? 'no warm traces \u2014 this engine never reports residency, so every '
            + 'job here is scored cold'
          : 'no warm traces in this window';
      }
      if (note) {
        note.textContent = text || '';
        note.hidden = !text;
      }
      list.appendChild(row);
    });
  }

  function setPart(root, name, on) {
    var el = root.querySelector('[data-rf-item="' + name + '"]');
    if (el) el.hidden = !on;
  }

  // --- calibration drift -----------------------------------------------------

  // A rolling median needs enough jobs per step to be a median at all. With a
  // short window the honest move is fewer steps, not a smoother-looking line
  // drawn through ones and twos.
  function driftSteps(count) {
    return Math.max(3, Math.min(12, Math.floor(count / 4)));
  }

  function fillDrift(jobs) {
    var section = document.querySelector('[data-rf-item="accuracy_drift"]');
    if (!section) return;
    var warmPath = section.querySelector('[data-rf="warm_series"]');
    var coldPath = section.querySelector('[data-rf="cold_series"]');
    var bandPath = section.querySelector('[data-rf="cold_band"]');
    if (!warmPath || !coldPath || !bandPath) return;

    var usable = jobs.filter(function (j) {
      return j.ts_done && j.predicted_total_ms && j.total_ms > 0;
    }).map(function (j) {
      return {
        t: new Date(j.ts_done).getTime(),
        err: Math.abs(j.predicted_total_ms - j.total_ms) / j.total_ms,
        warm: !!j.was_resident
      };
    }).filter(function (j) { return !isNaN(j.t); })
      .sort(function (a, b) { return a.t - b.t; });

    var blank = function (message) {
      warmPath.setAttribute('d', '');
      coldPath.setAttribute('d', '');
      bandPath.setAttribute('d', '');
      setPart(section, 'drift_event', false);
      setHook(section, 'drift_note', message);
      setHook(section, 'drift_summary', DASH);
      var ticks = section.querySelectorAll('[data-rf="drift_time"]');
      for (var i = 0; i < ticks.length; i++) ticks[i].textContent = DASH;
    };

    setHook(section, 'drift_steps', usable.length
      ? 'rolling median · ' + driftSteps(usable.length) + ' steps over ' +
        Math.max(1, Math.round((usable[usable.length - 1].t - usable[0].t) / 60000)) + ' min'
      : 'rolling median');

    if (usable.length < 8) {
      blank('Not enough finished jobs yet to draw a rolling median. ' +
            'The window needs at least eight; it has ' + usable.length + '.');
      return;
    }

    var first = usable[0].t, last = usable[usable.length - 1].t;
    var span = Math.max(1, last - first);
    var steps = driftSteps(usable.length);
    var cells = [];
    for (var i = 0; i < steps; i++) cells.push({ warm: [], cold: [], t: first + span * (i + 0.5) / steps });
    usable.forEach(function (j) {
      var index = Math.min(steps - 1, Math.floor((j.t - first) / span * steps));
      cells[index][j.warm ? 'warm' : 'cold'].push(j.err);
    });

    var top = 0.05;
    cells.forEach(function (c) {
      if (c.warm.length) top = Math.max(top, median(c.warm));
      if (c.cold.length) top = Math.max(top, median(c.cold) + spread(c.cold));
    });
    top = niceMax(top);

    var axis = section.querySelectorAll('[data-rf="drift_axis"]');
    for (var a = 0; a < axis.length; a++) {
      var frac = parseFloat(axis[a].style.bottom) / 100;
      if (isFinite(frac)) axis[a].textContent = pct(top * frac, 0);
    }

    // viewBox is 0 0 100 100 with y running downward, so a larger error sits
    // closer to the top of the box and therefore at a smaller y.
    var x = function (i) { return steps > 1 ? i / (steps - 1) * 100 : 50; };
    var y = function (v) { return Math.max(0, Math.min(100, 100 - v / top * 100)); };

    warmPath.setAttribute('d', series(cells, 'warm', x, y));
    coldPath.setAttribute('d', series(cells, 'cold', x, y));
    bandPath.setAttribute('d', band(cells, x, y));

    var ticks = section.querySelectorAll('[data-rf="drift_time"]');
    for (var k = 0; k < ticks.length; k++) {
      var f = ticks.length > 1 ? k / (ticks.length - 1) : 0;
      ticks[k].textContent = clockTime(new Date(first + span * f).toISOString());
    }

    // The design marks an event on the timeline. Rather than name a cause we
    // cannot see, it marks the step that actually paid for the most cold
    // starts, and only when one step stands out.
    var busiest = -1, busiestCount = 1;
    cells.forEach(function (c, index) {
      if (c.cold.length > busiestCount) { busiest = index; busiestCount = c.cold.length; }
    });
    var event = section.querySelector('[data-rf-item="drift_event"]');
    if (event) {
      if (busiest >= 0) {
        event.hidden = false;
        event.style.left = x(busiest).toFixed(1) + '%';
        setHook(event, 'drift_event_label', busiestCount + ' cold starts \u00b7 ' +
          clockTime(new Date(cells[busiest].t).toISOString()));
      } else {
        event.hidden = true;
      }
    }

    var firstCold = firstWith(cells, 'cold'), lastCold = lastWith(cells, 'cold');
    var warmAll = [], coldAll = [];
    cells.forEach(function (c) {
      warmAll = warmAll.concat(c.warm);
      coldAll = coldAll.concat(c.cold);
    });
    setHook(section, 'drift_note', describeDrift(warmAll, coldAll, firstCold, lastCold));
    setHook(section, 'drift_summary', coldAll.length && warmAll.length
      ? 'cold error runs ' + (median(coldAll) / Math.max(1e-9, median(warmAll))).toFixed(1) +
        '\u00d7 warm across the window'
      : 'not enough of both kinds to compare');
  }

  function spread(values) {
    if (values.length < 2) return 0;
    var m = values.reduce(function (a, b) { return a + b; }, 0) / values.length;
    var v = values.reduce(function (a, b) { return a + (b - m) * (b - m); }, 0) /
            (values.length - 1);
    return Math.sqrt(v);
  }

  function series(cells, key, x, y) {
    var parts = [];
    cells.forEach(function (c, i) {
      if (!c[key].length) return;   // a step with no jobs is a gap, not a zero
      parts.push((parts.length ? 'L' : 'M') + ' ' + x(i).toFixed(2) + ' ' +
                 y(median(c[key])).toFixed(2));
    });
    return parts.length > 1 ? parts.join(' ') : '';
  }

  function band(cells, x, y) {
    var upper = [], lower = [];
    cells.forEach(function (c, i) {
      if (c.cold.length < 2) return;
      var m = median(c.cold), sd = spread(c.cold);
      upper.push(x(i).toFixed(2) + ' ' + y(m + sd).toFixed(2));
      lower.unshift(x(i).toFixed(2) + ' ' + y(Math.max(0, m - sd)).toFixed(2));
    });
    if (upper.length < 2) return '';
    return 'M ' + upper.join(' L ') + ' L ' + lower.join(' L ') + ' Z';
  }

  function firstWith(cells, key) {
    for (var i = 0; i < cells.length; i++)
      if (cells[i][key].length) return median(cells[i][key]);
    return null;
  }

  function lastWith(cells, key) {
    for (var i = cells.length - 1; i >= 0; i--)
      if (cells[i][key].length) return median(cells[i][key]);
    return null;
  }

  function describeDrift(warmAll, coldAll, firstCold, lastCold) {
    if (!warmAll.length && !coldAll.length) return 'No finished jobs in this window.';
    var parts = [];
    if (warmAll.length) {
      parts.push('Warm error sits near ' + pct(median(warmAll)) + ' across ' +
                 warmAll.length + ' job(s).');
    } else {
      parts.push('No warm jobs in this window.');
    }
    if (!coldAll.length) {
      parts.push('No cold starts, so there is no load cost to track.');
    } else if (firstCold === null || lastCold === null || firstCold === lastCold) {
      // Too few cold starts to land in two different steps: the pooled median
      // is still worth stating, but there is no trend and no line to draw.
      parts.push('Only ' + coldAll.length + ' cold start(s), near ' +
                 pct(median(coldAll)) + ' \u2014 too few to trace a trend, so no ' +
                 'cold line is drawn.');
    } else if (firstCold > 0) {
      var change = (lastCold - firstCold) / firstCold;
      parts.push('Cold error ' +
        (change > 0.15 ? 'climbs from ' : change < -0.15 ? 'falls from ' : 'holds near ') +
        pct(firstCold) + ' to ' + pct(lastCold) + ' across the window.');
    } else {
      parts.push('Cold error sits near ' + pct(median(coldAll)) + '.');
    }
    return parts.join(' ');
  }

  // The design's "no traces yet" card, shown inside the plot area so the axes
  // survive. showEmpty() clears its container, which would delete .plot-y,
  // .plot-area and .plot-x and leave the panel unable to draw again.
  function emptyCard(area, empty) {
    if (!area) return;
    var existing = area.querySelector('[data-rf-item="empty_traces"]');
    if (!empty) {
      if (existing) existing.remove();
      return;
    }
    if (existing) return;
    var card = clone('rf-tpl-empty-traces');
    if (!card) return;
    card.style.position = 'absolute';
    card.style.inset = '0';
    card.style.display = 'grid';
    card.style.alignContent = 'center';
    area.appendChild(card);
  }

  function relPct(values, q) {
    var v = percentileOf(values, q);
    return v === null ? DASH : pct(v);
  }

  function percentileOf(values, q) {
    if (!values.length) return null;
    var s = values.slice().sort(function (a, b) { return a - b; });
    var pos = (s.length - 1) * q;
    var lo = Math.floor(pos), hi = Math.ceil(pos);
    return lo === hi ? s[lo] : s[lo] + (s[hi] - s[lo]) * (pos - lo);
  }

  // Signed, so a bias has a direction: positive means the job ran longer than
  // promised. |err| alone cannot tell a scatter from a shift, and the design's
  // note line asks which one it is.
  function signedErr(p) { return (p.y - p.x) / p.x; }
  function absErr(p) { return p.y > 0 ? Math.abs(p.x - p.y) / p.y : 0; }

  function worstOf(points) {
    var worst = null;
    points.forEach(function (p) {
      if (!worst || absErr(p) > absErr(worst)) worst = p;
    });
    return worst;
  }

  // The stats block under each scatter. It was drawn with fixture numbers and
  // no hooks at all, so it sat under live points asserting measurements of a
  // run that never happened — the quietest way for a console to mislead. Every
  // line here is computed from the same points the scatter plots.
  function fillTimingStats(points) {
    var section = document.querySelector('[data-rf-item="accuracy_total"]');
    if (!section) return;
    var warm = points.filter(function (p) { return p.warm; });
    var cold = points.filter(function (p) { return !p.warm; });

    setHook(section, 'timing_all_value',
      points.length ? 'median |err| ' + relPct(points.map(absErr), 0.5) : DASH);
    if (points.length) {
      var bias = percentileOf(points.map(signedErr), 0.5);
      setHook(section, 'timing_all_note',
        'p90 ' + relPct(points.map(absErr), 0.9) + ' \u00b7 bias ' +
        (bias >= 0 ? '+' : '') + (bias * 100).toFixed(1) + '%, ' +
        (Math.abs(bias) < 0.02 ? 'no systematic direction'
          : bias > 0 ? 'the scheduler runs optimistic'
                     : 'the scheduler runs pessimistic'));
    } else {
      setHook(section, 'timing_all_note', null);
    }

    [['warm', warm], ['cold', cold]].forEach(function (pair) {
      var name = pair[0], group = pair[1];
      setHook(section, 'timing_' + name + '_label', name + ' \u00b7 n ' + group.length);
      setHook(section, 'timing_' + name + '_value',
        group.length ? 'median ' + relPct(group.map(absErr), 0.5) : DASH);
      if (!group.length) {
        setHook(section, 'timing_' + name + '_note', 'no ' + name + ' jobs in this window');
        return;
      }
      var over = group.filter(function (p) { return p.y > p.x; }).length;
      setHook(section, 'timing_' + name + '_note',
        'p90 ' + relPct(group.map(absErr), 0.9) + ' \u00b7 ' + over + ' of ' +
        group.length + ' ran longer than predicted');
    });

    var worst = worstOf(points);
    setHook(section, 'timing_worst_value',
      worst ? (worst.y >= worst.x ? '+' : '') +
              (signedErr(worst) * 100).toFixed(1) + '%' : DASH);
    setHook(section, 'timing_worst_note',
      worst ? shortId(worst.id) + ' \u00b7 predicted ' + ms(worst.x) +
              ' \u00b7 actual ' + ms(worst.y) : DASH);
  }

  function fillLengthStats(points) {
    var section = document.querySelector('[data-rf-item="accuracy_tokens"]');
    if (!section) return;
    var tok = function (v) { return Math.round(v) + ' tok'; };

    setHook(section, 'length_all_value',
      points.length ? 'median |err| ' + relPct(points.map(absErr), 0.5) : DASH);
    setHook(section, 'length_all_note', points.length
      ? 'p90 ' + relPct(points.map(absErr), 0.9) + ' \u00b7 ' +
        (percentileOf(points.map(signedErr), 0.5) > 0.02
          ? 'replies run past the estimate'
          : percentileOf(points.map(signedErr), 0.5) < -0.02
            ? 'replies stop short of the estimate'
            : 'a spread, not a shift')
      : null);

    // "Over-run" and "early stop" are the design's words for the two ways a
    // length prediction fails, and they are not symmetric: one wastes VRAM
    // time, the other wastes the reservation.
    var over = points.filter(function (p) { return p.y > p.x; }).length;
    var early = points.filter(function (p) { return p.y < p.x; }).length;
    setHook(section, 'length_over_value', over + ' of ' + points.length);
    setHook(section, 'length_over_note', 'generated more than predicted');
    setHook(section, 'length_early_value', early + ' of ' + points.length);
    setHook(section, 'length_early_note', 'finished below the estimate');

    var worst = worstOf(points);
    setHook(section, 'length_worst_value',
      worst ? (worst.y >= worst.x ? '+' : '') +
              (signedErr(worst) * 100).toFixed(0) + '%' : DASH);
    setHook(section, 'length_worst_note',
      worst ? shortId(worst.id) + ' \u00b7 predicted ' + tok(worst.x) +
              ' \u00b7 actual ' + tok(worst.y) : DASH);
  }

  // --- wiring ---------------------------------------------------------------

  // The header counters sit on every screen. They were drawn but unhooked, so
  // they showed fixture numbers beside live data — the quietest way for a
  // console to mislead. A node whose engine cannot report residency is not
  // counted as holding nothing warm; it is left out of the total, because
  // "warm 3" would be a claim we cannot make (rule 2).
  // The two cards in the world that are not nodes. They shipped with fixture
  // labels -- a scheduler on port 11500 and three connected apps -- and neither
  // is something the router had been asked. One it knows exactly; the other it
  // cannot know at all, so the card says what it does know instead of asserting
  // a client count nothing measures.
  function renderWorldCards() {
    setHook(document, 'router_endpoint', location.host + ' \u00b7 scheduler');
    var n = state.jobs.length;
    setHook(document, 'client_summary',
      n ? n + ' job' + (n === 1 ? '' : 's') + ' \u00b7 openai-compatible'
        : 'no traffic yet \u00b7 openai-compatible');
  }

  function renderHeader() {
    var up = 0, warm = 0, inflight = 0, unknown = 0;
    state.nodes.forEach(function (n) {
      if (n.engine_healthy) up++;
      inflight += n.inflight || 0;
      if (n.residency_known === false) unknown++;
      else warm += (n.models_resident || []).length;
    });
    setHook(document, 'nodes_total', state.nodes.length);
    setHook(document, 'engines_up', up);
    setHook(document, 'warm_total', unknown && !warm ? null : warm);
    setHook(document, 'inflight_total', inflight);
  }

  function repaint() {
    renderHeader();
    renderWorldCards();
    if (document.getElementById('rf-world')) {
      renderCluster(state.nodes);
      renderInspectors(state.nodes);
    }
    if (document.getElementById('rf-feed')) renderJobs(state.jobs);
    if (document.getElementById('rf-tpl-candidate-admitted')) {
      renderDecision(state.jobs);
    }
    if (document.getElementById('rf-tpl-scatter-point')) renderAccuracy(state.jobs);
  }

  function get(url) {
    return fetch(url, { headers: headers() }).then(function (r) {
      if (r.status === 401) throw new Error('unauthorized');
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    });
  }

  function refresh() {
    return Promise.all([
      get(API.nodes).then(function (n) { state.nodes = n || []; }),
      get(API.jobs).then(function (j) { state.jobs = j || []; })
    ]).then(repaint).catch(function (e) {
      // A console that silently shows stale data is worse than one that says
      // it is disconnected.
      document.documentElement.classList.add('rf-offline');
      document.documentElement.setAttribute('data-rf-error', e.message);
    });
  }

  function subscribe() {
    if (typeof EventSource === 'undefined') return;
    // EventSource cannot carry an Authorization header, so the live stream is
    // only available when the router runs without a token — the loopback
    // default. With a token set the console falls back to polling.
    if (TOKEN) {
      setInterval(refresh, 2000);
      return;
    }
    var es = new EventSource(API.events);
    es.addEventListener('nodes', function (ev) {
      try {
        state.nodes = JSON.parse(ev.data) || [];
        document.documentElement.classList.remove('rf-offline');
        repaint();
      } catch (e) { /* a malformed frame is skipped, never fatal */ }
    });
    es.addEventListener('job', function () { refresh(); });
    es.onerror = function () {
      document.documentElement.classList.add('rf-offline');
    };
  }

  function start() {
    refresh().then(subscribe);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', start);
  } else {
    start();
  }
})();
