/*
 * FuckKookObs + KookClean 注入脚本
 *
 * 由 DLL（浏览器进程中的 CDP 客户端）通过
 *   Page.addScriptToEvaluateOnNewDocument + Runtime.evaluate
 * 注入到 KOOK 的每一个文档（含 iframe）里，负责：
 *   1. 广告/推广元素剔除（规则来自 DLL：内置默认值 + 同目录 kook-adblock.txt）
 *   2. 左上角“进入调试模式”小按钮；单击时调用 DLL 注册的 binding，
 *      由 DLL 切换 KOOK 内置 DevTools（Ctrl+Alt+Shift+0，右侧停靠）
 *
 * 注意：本文件被 tools/gen-payload-header.ps1 嵌入 UserFiles/kookclean_payload.h，
 *       修改后需要重新生成头文件再编译。
 */
(function () {
  'use strict';

  if (window.__FKO_INSTALLED__) { return; }
  Object.defineProperty(window, '__FKO_INSTALLED__', { value: 1, enumerable: false });

  // __FKO_RULES__ 会被 DLL 替换成 [{ "s": "css 选择器", "m": 0|1 }]
  // m = 1 表示把命中元素所在的“发现页模块”（.discover-module）整块隐藏
  var RULES = __FKO_RULES__;

  var HIDDEN_ATTR = 'data-fko-hidden';
  var MODULE_SEL = '.discover-module';
  var MODULE_FALLBACK_SEL = '[class*="discover-module"]';
  var BTN_ID = 'fko-debug-btn';
  var scheduling = false;

  function hideElement(el) {
    if (!el || el.nodeType !== 1) { return; }
    if (el.getAttribute(HIDDEN_ATTR) === '1') { return; }
    el.setAttribute(HIDDEN_ATTR, '1');
    el.style.setProperty('display', 'none', 'important');
  }

  function hideOwnerModule(el) {
    var scope = null;
    try {
      scope = el.closest(MODULE_SEL) || el.closest(MODULE_FALLBACK_SEL);
    } catch (e) {
      scope = null;
    }
    if (!scope || scope.nodeType !== 1) { return; }
    hideElement(scope);
  }

  function scan() {
    for (var i = 0; i < RULES.length; i++) {
      var rule = RULES[i];
      if (!rule || !rule.s) { continue; }
      var nodes = null;
      try { nodes = document.querySelectorAll(rule.s); } catch (e) { continue; }
      if (!nodes) { continue; }
      for (var k = 0; k < nodes.length; k++) {
        hideElement(nodes[k]);
        if (rule.m) { hideOwnerModule(nodes[k]); }
      }
    }
  }

  // 与 DLL 的通道：DLL 会在注入时通过 Runtime.addBinding 注册 __fkoToggleDevtools，
  // 页面调用它 -> 浏览器给 DLL 发 Runtime.bindingCalled 事件 -> DLL 切换 DevTools。
  // console.log 只作兜底（同一 target 多会话时收不到实时 console 事件）。
  function requestDevTools(source) {
    var viaBinding = false;
    try {
      if (typeof window.__fkoToggleDevtools === 'function') {
        window.__fkoToggleDevtools(String(source || 'button'));
        viaBinding = true;
      }
    } catch (e) {
      viaBinding = false;
    }
    try {
      console.log('__FKO_TOGGLE_DEVTOOLS__', viaBinding ? 'binding' : 'console');
    } catch (e) { /* ignore */ }
  }

  function buttonCss() {    return [
      'position:fixed', 'left:1px', 'top:1px', 'z-index:2147483647',
      'height:14px', 'line-height:14px', 'padding:0 4px', 'margin:0',
      'font:10px/14px "Microsoft YaHei",system-ui,sans-serif',
      'color:#ff4d4d', 'background:rgba(10,10,10,0.35)',
      'border:1px solid rgba(255,59,59,0.35)', 'border-radius:2px',
      'opacity:0.45', 'cursor:pointer', 'white-space:nowrap', 'overflow:hidden',
      'user-select:none', '-webkit-user-select:none', '-webkit-app-region:no-drag',
      'transition:opacity .12s linear,background .12s linear,border-color .12s linear'
    ].join(';');
  }

  function installButton() {
    if (!document.body) { return; }
    if (document.getElementById(BTN_ID)) { return; }

    var btn = document.createElement('div');
    btn.id = BTN_ID;
    btn.textContent = '进入调试模式';
    btn.title = '单击切换 DevTools（右侧停靠）';
    btn.style.cssText = buttonCss();

    btn.addEventListener('mouseenter', function () {
      btn.style.opacity = '1';
      btn.style.background = 'rgba(13,13,13,0.95)';
      btn.style.borderColor = '#ff3b3b';
    });
    btn.addEventListener('mouseleave', function () {
      btn.style.opacity = '0.45';
      btn.style.background = 'rgba(10,10,10,0.35)';
      btn.style.borderColor = 'rgba(255,59,59,0.35)';
    });
    btn.addEventListener('click', function (ev) {
      if (ev) {
        ev.preventDefault();
        ev.stopPropagation();
      }
      requestDevTools('button');
    });

    document.body.appendChild(btn);
  }

  function tick() {
    scheduling = false;
    try { scan(); } catch (e) { /* ignore */ }
    try { installButton(); } catch (e) { /* ignore */ }
  }

  function schedule() {
    if (scheduling) { return; }
    scheduling = true;
    if (window.requestAnimationFrame) {
      window.requestAnimationFrame(tick);
    } else {
      setTimeout(tick, 16);
    }
  }

  tick();

  try {
    var observer = new MutationObserver(schedule);
    observer.observe(document.documentElement, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ['class', 'style', 'hidden']
    });
  } catch (e) { /* ignore */ }

  document.addEventListener('DOMContentLoaded', tick, true);
  window.addEventListener('load', tick, true);
  window.addEventListener('popstate', schedule);
  window.addEventListener('hashchange', schedule);
  setInterval(tick, 2000);

  // 方便在 DevTools 里手动排查
  window.__FKO__ = {
    scan: scan,
    rules: RULES,
    hide: hideElement,
    devtools: requestDevTools
  };
})();
