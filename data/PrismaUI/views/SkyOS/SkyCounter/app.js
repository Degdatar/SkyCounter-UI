(() => {
  const byId = (id) => document.getElementById(id);
  const overlay = byId('skycounter-overlay');
  let hideTimer = 0;
  let currentItemKey = '';
  let currentItemFormID = '';
  let currentLocationKey = '';
  let currentNotice = null;
  const noticeQueue = [];
  let settings = { enabled: true, messageDesign: 'relic-card', theme: 'dwemer', position: 'top-center', size: 'standard', durationMs: 1500, showItemCount: true, showPluginName: true, showCategory: true };

  function setField(field, value) {
    document.querySelectorAll(`[data-field="${field}"]`).forEach((node) => { node.textContent = value; });
  }

  function setDetails(value) {
    document.querySelectorAll('[data-field="details"]').forEach((node) => {
      node.textContent = value;
      node.hidden = !value;
    });
  }

  function setCount(value) {
    document.querySelectorAll('[data-field="count"]').forEach((node) => {
      node.textContent = value;
      node.hidden = !value;
    });
  }

  function setArtwork(path) {
    document.querySelectorAll('[data-field="icon"]').forEach((icon) => {
      const fallback = icon.parentElement.querySelector('[data-field="fallback"]');
      if (path) {
        icon.src = path;
        icon.hidden = false;
        if (fallback) fallback.hidden = true;
      } else {
        icon.removeAttribute('src');
        icon.hidden = true;
        if (fallback) fallback.hidden = false;
      }
    });
  }

  function hideNativeView() {
    if (window.skyrim && typeof window.skyrim.skyos_skycounter_hide === 'function') window.skyrim.skyos_skycounter_hide('');
    else if (typeof window.skyos_skycounter_hide === 'function') window.skyos_skycounter_hide('');
  }

  function paintPickup(payload) {
    const item = payload && typeof payload === 'object' ? payload.item : null;
    if (!item) return;
    currentLocationKey = '';
    currentItemKey = `${item.plugin || ''}|${item.localFormID || item.formID || ''}`;
    currentItemFormID = String(item.formID || '').toUpperCase();
    const discovered = item.discoveredNow === true;
    setField('eyebrow', discovered ? 'ITEM DISCOVERED' : 'ITEM ACQUIRED');
    setField('title', discovered ? `Discovered · ${item.name || 'Unknown item'}` : (item.name || 'Unknown item'));
    const details = [settings.showCategory ? (item.category || 'Item') : '', settings.showPluginName ? (item.plugin || 'Unknown plugin') : ''].filter(Boolean);
    setDetails(details.join(' · '));
    setCount(settings.showItemCount && !discovered ? `${Number(item.lastPickup || 0).toLocaleString()} ×` : '');
    setArtwork(item.icon || '');
  }

  function paintLocation(location) {
    if (!location) return;
    currentItemKey = '';
    currentItemFormID = '';
    currentLocationKey = location.key || `${location.plugin || ''}|${location.localFormID || location.formID || ''}`;
    setField('eyebrow', 'LOCATION DISCOVERED');
    setField('title', location.name || 'Unknown location');
    const details = [settings.showCategory ? 'New place' : '', settings.showPluginName ? (location.plugin || 'Unknown plugin') : ''].filter(Boolean);
    setDetails(details.join(' · '));
    setCount('');
    setArtwork(location.image || '');
  }

  function renderNotice(payload) {
    if (payload && payload.location) paintLocation(payload.location);
    else paintPickup(payload);
  }

  function showNextNotice() {
    if (!settings.enabled || noticeQueue.length === 0) {
      currentNotice = null;
      overlay.classList.remove('visible');
      window.setTimeout(hideNativeView, 220);
      return;
    }
    currentNotice = noticeQueue.shift();
    renderNotice(currentNotice);
    overlay.classList.add('visible');
    if (hideTimer) window.clearTimeout(hideTimer);
    hideTimer = window.setTimeout(() => {
      hideTimer = 0;
      if (noticeQueue.length) showNextNotice();
      else {
        currentNotice = null;
        overlay.classList.remove('visible');
        window.setTimeout(hideNativeView, 220);
      }
    }, settings.durationMs);
  }

  function clearNoticeQueue() {
    noticeQueue.length = 0;
    currentNotice = null;
    currentItemKey = '';
    currentItemFormID = '';
    currentLocationKey = '';
    if (hideTimer) window.clearTimeout(hideTimer);
    hideTimer = 0;
    overlay.classList.remove('visible');
    hideNativeView();
  }

  window.SkyCounter = {
    setSettings(next) {
      const value = next && typeof next === 'object' ? next : {};
      settings = {
        enabled: value.enabled !== false,
        messageDesign: ['relic-card', 'loot-banner', 'quest-toast', 'minimal-pill'].includes(value.messageDesign) ? value.messageDesign : 'relic-card',
        theme: ['dwemer', 'frost', 'imperial', 'arcane'].includes(value.theme) ? value.theme : 'dwemer',
        position: ['top-center', 'center-center', 'top-left', 'top-right', 'center-left', 'center-right', 'bottom-left', 'bottom-center', 'bottom-right'].includes(value.position) ? value.position : 'top-center',
        size: ['compact', 'standard', 'large'].includes(value.size) ? value.size : 'standard',
        durationMs: [500, 1000, 1500, 2000, 2500, 3000].includes(Number(value.durationMs)) ? Number(value.durationMs) : 1500,
        showItemCount: value.showItemCount !== false,
        showPluginName: value.showPluginName !== false,
        showCategory: value.showCategory !== false
      };
      document.body.dataset.design = settings.messageDesign;
      document.body.dataset.theme = settings.theme;
      document.body.dataset.position = settings.position;
      document.body.dataset.size = settings.size;
      if (!settings.enabled) clearNoticeQueue();
      else if (currentNotice) renderNotice(currentNotice);
    },
    showPickup(payload) {
      this.showNotice(payload);
    },
    showNotice(payload) {
      if (!settings.enabled || !payload || (!payload.item && !payload.location)) return;
      if (noticeQueue.length >= 256) noticeQueue.shift();
      noticeQueue.push(payload);
      if (!currentNotice) showNextNotice();
    },
    clearNotice: clearNoticeQueue,
    showLocation(location) {
      this.showNotice({ location });
    },
    updateIcon(item) {
      if (!item || !item.icon) return;
      const key = `${item.plugin || ''}|${item.localFormID || item.formID || ''}`;
      const formID = String(item.formID || '').toUpperCase();
      if (key !== currentItemKey && (!formID || formID !== currentItemFormID)) return;
      setArtwork(item.icon);
    },
    updateLocation(location) {
      if (!location || !location.image) return;
      const key = location.key || `${location.plugin || ''}|${location.localFormID || location.formID || ''}`;
      if (key !== currentLocationKey) return;
      setArtwork(location.image);
    }
  };

})();
