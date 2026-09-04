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

  function renderInspector(node) {
    var host = document.getElementById('rf-insp');
    if (!host || !node) return;
    var panel = clone('rf-tpl-node-inspector');
    if (!panel) return;

    panel.setAttribute('data-insp', node.id);
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

    clear(host);
    host.appendChild(panel);
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
      return;
    }

    var admitted = (job.candidates || []).filter(function (c) { return c.admitted; });

    setHook(host, 'job_id', shortId(job.job_id));
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
  function fillScatter(section, points, label) {
    if (!section) return;
    var plot = section.querySelector('.plot');
    if (!plot) return;

    var old = plot.querySelectorAll('[data-rf-item="trace"]');
    for (var i = 0; i < old.length; i++) old[i].remove();

    var n = section.querySelector('.pan-h .n');
    if (n) n.textContent = 'n ' + points.length;
    if (!points.length) return;

    // A shared max on both axes keeps the 1:1 reference line meaningful; a
    // point above it ran longer than predicted, which is the whole reading.
    var max = 1;
    points.forEach(function (p) { max = Math.max(max, p.x, p.y); });

    points.forEach(function (p) {
      var dot = clone('rf-tpl-scatter-point');
      if (!dot) return;
      dot.style.left = (p.x / max * 100).toFixed(2) + '%';
      dot.style.bottom = (p.y / max * 100).toFixed(2) + '%';
      dot.classList.toggle('is-warm', !!p.warm);
      dot.classList.toggle('is-cold', !p.warm);
      setHook(dot, label.x, label.fmt(p.x));
      setHook(dot, label.y, label.fmt(p.y));
      plot.appendChild(dot);
    });
  }

  function renderAccuracy(jobs) {
    var ok = jobs.filter(function (j) { return j.outcome === 'ok'; });

    var timing = ok.filter(function (j) {
      return j.predicted_total_ms && j.total_ms;
    }).map(function (j) {
      return { x: j.predicted_total_ms, y: j.total_ms, warm: !!j.was_resident };
    });
    var length = ok.filter(function (j) {
      return j.predicted_output_tokens && j.output_tokens;
    }).map(function (j) {
      return { x: j.predicted_output_tokens, y: j.output_tokens, warm: !!j.was_resident };
    });

    if (!timing.length && !length.length) {
      var section = document.querySelector('[data-rf-item="accuracy_total"] .plot');
      if (section) showEmpty(section, 'rf-tpl-empty-traces');
      return;
    }

    fillScatter(document.querySelector('[data-rf-item="accuracy_total"]'), timing,
      { x: 'predicted_total_ms', y: 'total_ms', fmt: ms });
    fillScatter(document.querySelector('[data-rf-item="accuracy_tokens"]'), length,
      { x: 'predicted_output_tokens', y: 'output_tokens',
        fmt: function (v) { return Math.round(v) + ' tok'; } });

    var err = function (p) { return Math.abs(p.x - p.y) / p.y; };
    var warm = timing.filter(function (p) { return p.warm; });
    var cold = timing.filter(function (p) { return !p.warm; });
    setHook(document, 'warm_count', warm.length);
    setHook(document, 'cold_count', cold.length);
    setHook(document, 'warm_median', warm.length ? pct(median(warm.map(err))) : null);
    setHook(document, 'cold_median', cold.length ? pct(median(cold.map(err))) : null);
    setHook(document, 'median_err', timing.length ? pct(median(timing.map(err))) : null);
  }

  // --- wiring ---------------------------------------------------------------

  // The header counters sit on every screen. They were drawn but unhooked, so
  // they showed fixture numbers beside live data — the quietest way for a
  // console to mislead. A node whose engine cannot report residency is not
  // counted as holding nothing warm; it is left out of the total, because
  // "warm 3" would be a claim we cannot make (rule 2).
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
    if (document.getElementById('rf-world') &&
        document.getElementById('rf-tpl-node-inspector')) {
      renderCluster(state.nodes);
      renderInspector(state.nodes[0]);
    } else if (document.getElementById('rf-world')) {
      renderCluster(state.nodes);
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
