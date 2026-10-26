(() => {
  const pickupStream = document.getElementById('pickup-stream');
  const locationStream = document.getElementById('location-stream');
  const pickupCount = document.getElementById('pickup-count');
  const locationCount = document.getElementById('location-count');
  const pickups = new Map();
  const locations = new Map();
  const keyFor = (value) => `${value?.plugin || ''}|${value?.localFormID || value?.formID || value?.key || value?.name || ''}`;

  function text(tag, className, value) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    node.textContent = String(value || '');
    return node;
  }

  function makeEntry(record, location = false) {
    const row = document.createElement('article');
    row.className = `entry${location ? ' location-entry' : ''}`;
    const art = document.createElement('div');
    art.className = 'art';
    if (record.icon || record.image) {
      const image = document.createElement('img');
      image.alt = '';
      image.src = record.icon || record.image;
      image.addEventListener('error', () => { image.remove(); art.textContent = '◇'; }, { once: true });
      art.appendChild(image);
    } else art.textContent = '◇';
    const copy = document.createElement('div');
    copy.className = 'copy';
    copy.append(text('strong', '', record.name || 'Unknown discovery'));
    const meta = [record.category || (location ? 'Location' : 'Item'), record.plugin || 'Unknown plugin'].filter(Boolean).join(' · ');
    copy.append(text('small', '', meta));
    row.append(art, copy);
    if (!location) row.append(text('span', 'quantity', `${Number(record.lastPickup || 0).toLocaleString()} × picked up`));
    return row;
  }

  function render() {
    const recent = [...pickups.values()].sort((a, b) => Number(b.updatedAt || 0) - Number(a.updatedAt || 0)).slice(0, 12);
    pickupCount.textContent = `${recent.length} recent pickups`;
    pickupStream.replaceChildren(...(recent.length ? recent.map((record) => makeEntry(record)) : [text('div', 'empty', 'New pickups appear here.') ]));
    const places = [...locations.values()].sort((a, b) => Number(b.updatedAt || 0) - Number(a.updatedAt || 0)).slice(0, 8);
    locationCount.textContent = `${places.length} locations`;
    locationStream.replaceChildren(...(places.length ? places.map((record) => makeEntry(record, true)) : [text('div', 'empty', 'Newly discovered locations appear here.') ]));
  }

  function receive(type, payload) {
    if (payload?.clearStream === true) {
      pickups.clear();
      locations.clear();
    } else if (type === 'pickup' && payload?.item) {
      const record = { ...payload.item, updatedAt: Date.now() };
      pickups.set(keyFor(record), record);
    } else if (type === 'icon' && payload) {
      const formID = String(payload.formID || '').toUpperCase();
      for (const [key, prior] of pickups) {
        if (String(prior.formID || '').toUpperCase() === formID || String(prior.localFormID || '').toUpperCase() === formID) {
          pickups.set(key, { ...prior, ...payload, updatedAt: Date.now() });
        }
      }
    } else if (type === 'location' && payload) {
      const record = { ...payload, updatedAt: Date.now() };
      locations.set(keyFor(record), record);
    }
    render();
  }

  window.addEventListener('message', (event) => {
    if (event.source !== window.parent || event.data?.channel !== 'skycounter-ui') return;
    receive(event.data.type, event.data.payload);
  });
  render();
  window.parent.postMessage({ channel: 'skycounter-ui', type: 'ready' }, '*');
})();
