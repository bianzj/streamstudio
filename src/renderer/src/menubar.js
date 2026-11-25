// ==========================================================================
// 文件：menubar.js
// 功能：应用顶部菜单栏的交互行为（展开/收起、鼠标悬停切换、键盘导航与全局快捷键）
// 主要模块：
//   1. open / close：菜单面板的展开与收起（维护 aria-expanded 与 hidden）
//   2. 触发按钮事件：点击切换，已有菜单展开时悬停切换，ArrowDown/Enter/空格展开
//   3. 捕获阶段的菜单项点击：先交给 route 处理，再关闭菜单
//   4. 菜单内键盘导航：上下键循环选项，左右键切换相邻菜单，Tab 关闭
//   5. 点击外部/窗口失焦关闭；Esc 先关菜单；Ctrl/Cmd+N、Ctrl/Cmd+O 快捷键
// 数据流：main.js 调用 createMenubar({ root, route })，返回 { close } 供外部关闭菜单
// 关系：菜单项自身的 click 处理器在别处注册；快捷键通过 click() 触发 #newProjectBtn / #openXmlBtn
// 注意：模块内部只保存一个 active 菜单；对话框或抽屉打开、焦点在输入控件时不响应快捷键
// ==========================================================================

// Application menu bar. Menu items keep their own click handlers; `route` may claim an item first
// (e.g. project commands while the satellite workspace is open).
export function createMenubar({ root, route }) {
  const menus = [...root.querySelectorAll('.menu')]
  let active = null

  // 取某个菜单中可用（未禁用）的菜单项，用于键盘导航。
  const items = (menu) => [...menu.querySelectorAll('.menu-item:not(:disabled)')]

  // 作用：展开 menu（若已有其他菜单展开则先关闭）；focusFirst 为真时聚焦第一个可用项。
  function open(menu, focusFirst = false) {
    if (active && active !== menu) close()
    active = menu
    menu.classList.add('open')
    menu.querySelector('.menu-trigger').setAttribute('aria-expanded', 'true')
    menu.querySelector('.menu-panel').hidden = false
    if (focusFirst) items(menu)[0]?.focus()
  }

  // 作用：关闭当前展开的菜单；returnFocus 为真时把焦点还给触发按钮（Esc 使用）。
  function close(returnFocus = false) {
    if (!active) return
    const menu = active
    active = null
    menu.classList.remove('open')
    menu.querySelector('.menu-trigger').setAttribute('aria-expanded', 'false')
    menu.querySelector('.menu-panel').hidden = true
    if (returnFocus) menu.querySelector('.menu-trigger').focus()
  }

  // 步骤 1：为每个菜单的触发按钮绑定点击、悬停和键盘展开事件。
  for (const menu of menus) {
    const trigger = menu.querySelector('.menu-trigger')
    trigger.addEventListener('click', () => (active === menu ? close() : open(menu)))
    trigger.addEventListener('pointerenter', () => { if (active && active !== menu) open(menu) })
    trigger.addEventListener('keydown', (event) => {
      if (event.key === 'ArrowDown' || event.key === 'Enter' || event.key === ' ') { event.preventDefault(); open(menu, true) }
    })
  }

  // 步骤 2：捕获阶段处理菜单项点击。route 返回真表示已被接管（如卫星工作区打开时的
  //   项目命令），此时阻止事件继续传给菜单项自身的处理器；随后总是关闭菜单。
  root.addEventListener('click', (event) => {
    const item = event.target.closest('.menu-item')
    if (!item) return
    if (route?.(item)) event.stopPropagation()
    close()
  }, true)

  // 步骤 3：菜单展开时的键盘导航（上下键在菜单项间循环，左右键切换相邻菜单）。
  root.addEventListener('keydown', (event) => {
    if (!active) return
    const list = items(active)
    const index = list.indexOf(document.activeElement)
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault()
      const step = event.key === 'ArrowDown' ? 1 : -1
      list[(index + step + list.length) % list.length]?.focus()
    } else if (event.key === 'ArrowRight' || event.key === 'ArrowLeft') {
      event.preventDefault()
      const position = menus.indexOf(active)
      const next = menus[(position + (event.key === 'ArrowRight' ? 1 : -1) + menus.length) % menus.length]
      open(next, true)
    } else if (event.key === 'Tab') close()
  })

  // 步骤 4：点击菜单栏之外或窗口失焦时关闭菜单。
  document.addEventListener('pointerdown', (event) => { if (active && !root.contains(event.target)) close() })
  window.addEventListener('blur', () => close())

  // 步骤 5：全局快捷键（捕获阶段）。Esc 关闭菜单并吞掉事件；Ctrl/Cmd+N 新建工程、
  //   Ctrl/Cmd+O 打开 XML。有打开的对话框/抽屉或焦点在可编辑控件时不触发，
  //   带 Alt 或 Shift 的组合也不触发。
  // Registered before the workspace's capture handler so Escape closes an open menu first.
  window.addEventListener('keydown', (event) => {
    if (event.key === 'Escape' && active) { event.preventDefault(); event.stopImmediatePropagation(); close(true); return }
    if (!(event.ctrlKey || event.metaKey) || event.altKey) return
    if (document.querySelector('dialog[open], .drawer-backdrop:not([hidden])')) return
    const focused = document.activeElement
    if (focused && (focused.isContentEditable || focused.closest?.('[contenteditable="true"]') ||
        /^(input|textarea|select)$/i.test(focused.tagName))) return
    const key = event.key.toLowerCase()
    const target = key === 'n' && !event.shiftKey ? '#newProjectBtn' : key === 'o' && !event.shiftKey ? '#openXmlBtn' : null
    if (!target) return
    event.preventDefault()
    event.stopImmediatePropagation()
    root.querySelector(target)?.click()
  }, true)

  return { close }
}
