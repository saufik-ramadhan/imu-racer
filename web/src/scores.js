const KEY = 'imu-racer.scores.v1';
const MAX = 8;

export function load() {
  try {
    const raw = JSON.parse(localStorage.getItem(KEY) || '[]');
    return Array.isArray(raw) ? raw.slice(0, MAX) : [];
  } catch {
    return [];
  }
}

/**
 * Insert a run and return { table, rank, isBest }. rank is -1 when the score
 * did not make the table.
 */
export function submit(entry) {
  const table = load();
  const row = {
    score: Math.round(entry.score),
    dodged: entry.dodged | 0,
    speed: +entry.speed.toFixed(1),
    date: Date.now(),
  };

  table.push(row);
  table.sort((a, b) => b.score - a.score || b.dodged - a.dodged);
  const trimmed = table.slice(0, MAX);
  const rank = trimmed.indexOf(row);

  try {
    localStorage.setItem(KEY, JSON.stringify(trimmed));
  } catch {
    // Private browsing can refuse writes; the run still shows on screen.
  }

  return { table: trimmed, rank, isBest: rank === 0 };
}

export function clear() {
  try {
    localStorage.removeItem(KEY);
  } catch { /* ignore */ }
}

export function render(tbody, table, highlightRank = -1) {
  tbody.innerHTML = '';

  if (!table.length) {
    const tr = document.createElement('tr');
    tr.innerHTML = '<td colspan="4" class="empty">No runs yet</td>';
    tbody.append(tr);
    return;
  }

  table.forEach((row, i) => {
    const tr = document.createElement('tr');
    if (i === highlightRank) tr.className = 'you';
    const when = new Date(row.date).toLocaleDateString(undefined, {
      month: 'short',
      day: 'numeric',
    });
    tr.innerHTML =
      `<td>${i + 1}</td>` +
      `<td>${row.dodged} dodged</td>` +
      `<td>${when}</td>` +
      `<td>${row.score.toLocaleString()}</td>`;
    tbody.append(tr);
  });
}
