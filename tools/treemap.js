const CSS_PREFIX = 'webtreemap-';
const NODE_CSS_CLASS = CSS_PREFIX + 'node';

const DEFAULT_CSS = `
.webtreemap-node {
  cursor: pointer;
  position: absolute;
  border: solid 1px #666;
  box-sizing: border-box;
  overflow: hidden;
  background: white;
  transition: left .15s, top .15s, width .15s, height .15s;
}

.webtreemap-node:hover {
  background: #ddd;
}

.webtreemap-caption {
  font-size: 10px;
  text-align: center;
}
`;

function addCSS(parent) {
  const style = document.createElement('style');
  style.innerText = DEFAULT_CSS;
  parent.appendChild(style);
}

function isDOMNode(e) {
  return e.classList.contains(NODE_CSS_CLASS);
}

function getNodeIndex(target) {
  let index = 0;
  let node = target;
  while ((node = node.previousElementSibling)) {
    if (isDOMNode(node)) index++;
  }
  return index;
}

function getAddress(el) {
  let address = [];
  let n = el;
  while (n && isDOMNode(n)) {
    address.unshift(getNodeIndex(n));
    n = n.parentElement;
  }
  address.shift(); // The first element will be the root, index 0.
  return address;
}

function px(x) {
  return Math.round(x) + 'px';
}

function defaultOptions(options) {
  const opts = {
    padding: options.padding || [14, 3, 3, 3],
    lowerBound: options.lowerBound === undefined ? 0.1 : options.lowerBound,
    applyMutations: options.applyMutations || (() => null),
    caption: options.caption || ((node) => node.id || ''),
    showNode:
      options.showNode ||
      ((node, width, height) => {
        return width > 20 && height >= opts.padding[0];
      }),
    showChildren:
      options.showChildren ||
      ((node, width, height) => {
        return width > 40 && height > 40;
      }),
  };
  return opts;
}

class TreeMap {
  constructor(node, options) {
    this.node = node;
    this.options = defaultOptions(options);
  }

  ensureDOM(node) {
    if (node.dom) return node.dom;
    const dom = document.createElement('div');
    dom.className = NODE_CSS_CLASS;
    if (this.options.caption) {
      const caption = document.createElement('div');
      caption.className = CSS_PREFIX + 'caption';
      caption.innerText = this.options.caption(node);
      dom.appendChild(caption);
    }
    node.dom = dom;
    this.options.applyMutations(node);
    return dom;
  }

  selectSpan(children, space, start) {
    let smin = children[start].size; // Smallest seen child so far.
    let smax = smin; // Largest child.
    let sum = 0; // Sum of children in this span.
    let lastScore = 0; // Best score yet found.
    let end = start;
    for (; end < children.length; end++) {
      const size = children[end].size;
      if (size < smin) smin = size;
      if (size > smax) smax = size;

      const nextSum = sum + size;
      const score = Math.max(
        (smax * space * space) / (nextSum * nextSum),
        (nextSum * nextSum) / (smin * space * space)
      );

      if (lastScore && score > lastScore) {
        break;
      }
      lastScore = score;
      sum = nextSum;
    }
    return { end, sum };
  }


  layoutChildren(node, level, width, height) {
    const total = node.size;
    const children = node.children;
    if (!children) return;

    let x1 = -1, y1 = -1, x2 = width - 1, y2 = height - 1;
    const padding = this.options.padding;
    y1 += padding[0];
    if (padding[1]) {
      x2 -= padding[1] + 1;
    }
    y2 -= padding[2];
    x1 += padding[3];

    let i = 0;
    if (this.options.showChildren(node, x2 - x1, y2 - y1)) {
      const scale = Math.sqrt(total / ((x2 - x1) * (y2 - y1)));
      let x = x1, y = y1;
      for (let start = 0; start < children.length;) {
        x = x1;
        const space = scale * (x2 - x1);
        const { end, sum } = this.selectSpan(children, space, start);
        if (sum / total < this.options.lowerBound) break;
        const heightPx = Math.round(sum / space) + 1;
        for (i = start; i < end; i++) {
          const child = children[i];
          const size = child.size;
          const widthPx = Math.round(size / sum * space) + 1;
          if (!this.options.showNode(child, widthPx, heightPx)) {
            break;
          }
          const needsAppend = !child.dom;
          const dom = this.ensureDOM(child);
          dom.style.left = px(x);
          dom.style.width = px(widthPx);
          dom.style.top = px(y);
          dom.style.height = px(heightPx);
          if (needsAppend) {
            node.dom.appendChild(dom);
          }
          this.layoutChildren(child, level + 1, widthPx, heightPx);
          x += widthPx - 1;
        }
        y += heightPx - 1;
        start = end;
      }
    }
    for (; i < children.length; i++) {
      const child = children[i];
      if (child.dom) {
        child.dom.parentNode.removeChild(child.dom);
        child.dom = undefined;
      }
    }
  }


  render(container) {
    addCSS(container);
    const dom = this.ensureDOM(this.node);
    const width = container.offsetWidth;
    const height = container.offsetHeight;
    dom.onclick = e => {
      let node = e.target;
      while (!isDOMNode(node)) {
        node = node.parentElement;
        if (!node) return;
      }
      const address = getAddress(node);
      this.zoom(address);
    };
    dom.style.width = px(width);
    dom.style.height = px(height);
    container.appendChild(dom);
    this.layoutChildren(this.node, 0, width, height);
  }

  zoom(address) {
    let node = this.node;
    const [padTop, padRight, padBottom, padLeft] = this.options.padding;

    let width = node.dom.offsetWidth;
    let height = node.dom.offsetHeight;
    for (const index of address) {
      width -= padLeft + padRight;
      height -= padTop + padBottom;

      if (!node.children) throw new Error('bad address');
      for (const c of node.children) {
        if (c.dom) c.dom.style.zIndex = '0';
      }
      node = node.children[index];
      const style = node.dom.style;
      style.zIndex = '1';
      style.left = px(padLeft - 1);
      style.width = px(width);
      style.top = px(padTop - 1);
      style.height = px(height);
    }
    this.layoutChildren(node, 0, width, height);
  }
}

function webtreemapRender(container, node, options) {
  new TreeMap(node, options).render(container);
}

function humanSizeCaption(node) {
  let units = ['', 'k', 'm', 'g'];
  let unit = 0;
  let size = node.size;
  while (size > 1024 && unit < units.length - 1) {
    size = size / 1024;
    unit++;
  }
  return `${node.id || ''} (${size.toFixed(1)}${units[unit]})`;
}
