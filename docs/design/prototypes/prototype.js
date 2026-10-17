document.addEventListener("DOMContentLoaded", () => {
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

  const search = $("[data-app-search]");
  if (search) {
    search.addEventListener("input", () => {
      const query = search.value.trim().toLocaleLowerCase();
      let visible = 0;
      $$(".app-card", $("[data-app-grid]") || document).forEach(card => {
        const match = (card.dataset.search || card.innerText).toLocaleLowerCase().includes(query);
        card.hidden = !match;
        if (match) visible++;
      });
      const empty = $("[data-search-empty]");
      if (empty) empty.hidden = visible !== 0;
    });
  }

  $$("[data-demo-tabs]").forEach(group => {
    const buttons = $$("[data-demo-tab]", group);
    buttons.forEach(button => button.addEventListener("click", () => {
      buttons.forEach(item => item.classList.toggle("active", item === button));
      const name = button.dataset.demoTab;
      const root = group.closest("[data-tab-root]") || document;
      $$("[data-tab-panel]", root).forEach(panel => panel.hidden = panel.dataset.tabPanel !== name);
    }));
  });

  $$("[data-demo-toggle]").forEach(button => {
    button.addEventListener("click", () => {
      const active = button.getAttribute("aria-checked") !== "true";
      button.setAttribute("aria-checked", String(active));
      button.title = active ? "已开启（演示）" : "已关闭（演示）";
    });
  });

  $$("[data-window-action]").forEach(button => {
    button.addEventListener("click", () => {
      const windowEl = button.closest(".window");
      if (!windowEl) return;
      const action = button.dataset.windowAction;
      if (action === "minimize") windowEl.classList.toggle("demo-minimized");
      if (action === "maximize") windowEl.classList.toggle("demo-maximized");
      if (action === "close") windowEl.classList.toggle("demo-closed");
    });
  });

  $$("[data-file-row]").forEach(row => {
    row.addEventListener("click", event => {
      if (event.target.closest("button,input,a")) return;
      row.classList.toggle("selected");
    });
  });

  $$("[data-desktop-icon]").forEach(icon => {
    icon.addEventListener("click", event => {
      event.preventDefault();
      if (event.detail > 1) return;
      $$("[data-desktop-icon]").forEach(item => item.classList.toggle("selected", item === icon));
    });
    icon.addEventListener("dblclick", () => {
      const target = icon.dataset.launchTarget;
      if (target) window.location.href = target;
    });
  });

  $$("[data-prototype-theme]").forEach(button => {
    button.addEventListener("click", () => {
      document.body.classList.toggle("theme-light");
      button.textContent = document.body.classList.contains("theme-light") ? "切换深色主题" : "切换浅色主题";
    });
  });

  const timeRange = $("[data-time-range]");
  if (timeRange) {
    timeRange.addEventListener("change", () => {
      const label = $("[data-range-label]");
      if (label) label.textContent = timeRange.options[timeRange.selectedIndex].text;
    });
  }
});