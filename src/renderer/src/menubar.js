// Application menu bar. Menu items keep their own click handlers; `route` may claim an item first
// (e.g. project commands while the satellite workspace is open).
export function createMenubar({ root, route }) {
  const menus = [...root.querySelectorAll('.menu')]
  let active = null

  const items = (menu) => [...menu.querySelectorAll('.menu-item:not(:disabled)')]

  function open(menu, focusFirst = false) {
    if (active && active !== menu) close()
    active = menu
    menu.classList.add('open')
    menu.querySelector('.menu-trigger').setAttribute('aria-expanded', 'true')
    menu.querySelector('.menu-panel').hidden = false
    if (focusFirst) items(menu)[0]?.focus()
  }

  function close(returnFocus = false) {
    if (!active) return
    const menu = active
    active = null
    menu.classList.remove('open')
    menu.querySelector('.menu-trigger').setAttribute('aria-expanded', 'false')
    menu.querySelector('.menu-panel').hidden = true
    if (returnFocus) menu.querySelector('.menu-trigger').focus()
  }

  for (const menu of menus) {
    const trigger = menu.querySelector('.menu-trigger')
    trigger.addEventListener('click', () => (active === menu ? close() : open(menu)))
    trigger.addEventListener('pointerenter', () => { if (active && active !== menu) open(menu) })
    trigger.addEventListener('keydown', (event) => {
      if (event.key === 'ArrowDown' || event.key === 'Enter' || event.key === ' ') { event.preventDefault(); open(menu, true) }
    })
  }

  root.addEventListener('click', (event) => {
    const item = event.target.closest('.menu-item')
    if (!item) return
    if (route?.(item)) event.stopPropagation()
    close()
  }, true)

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

  document.addEventListener('pointerdown', (event) => { if (active && !root.contains(event.target)) close() })
  window.addEventListener('blur', () => close())

  // Registered before the workspace's capture handler so Escape closes an open menu first.
  window.addEventListener('keydown', (event) => {
    if (event.key === 'Escape' && active) { event.preventDefault(); event.stopImmediatePropagation(); close(true); return }
    if (!(event.ctrlKey || event.metaKey) || event.altKey) return
    // FIXED[低] Bug: 全局捕获 Ctrl+N/Ctrl+O 并 stopImmediatePropagation，模态对话框打开或 project.json 源码编辑器聚焦时也会触发新建/打开；已修复：原生 dialog[open]、应用内 .drawer-backdrop 弹层打开或焦点在可编辑元素时直接跳过
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
