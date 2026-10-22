// Railroad diagrams as SVG, drawn at build time: the layout of Tab Atkins's railroad-diagrams (sequences, choices,
// loops; arcs of 10 between tracks), redrawn small so that terminals and nonterminals carry links and classes, and
// the text is measured for the page's monospace font. The page's stylesheet colours them for light and dark.
import { escapeHtml } from './text.js';

const AR = 10; // arc radius
const VS = 8; // vertical space between the tracks of a choice or a loop
const CHAR = 7.83; // width of a character at 13px in a monospace font (0.6em, as SF Mono, Menlo and Consolas have)
const BOX = 11; // half a box's height

const fmt = (n) => (Math.round(n * 100) / 100).toString();
const path = (d, cls = 'rr-line') => `<path class="${cls}" d="${d}"/>`;
const h = (x, y, w) => (w > 0 ? path(`M${fmt(x)} ${fmt(y)}h${fmt(w)}`) : '');

class Box {
	constructor(text, { href = null, terminal = true, title = null } = {}) {
		this.text = text;
		this.href = href;
		this.terminal = terminal;
		this.title = title;
		this.width = Math.ceil(text.length * CHAR) + 20;
		this.up = BOX;
		this.down = BOX;
		this.needsSpace = true;
	}

	render(x, y, width) {
		// left-aligned on a wider track, as a sequence is
		const gap = width - this.width;
		const left = x;
		const rx = this.terminal ? ' rx="11" ry="11"' : '';
		const cls = this.terminal ? 'rr-terminal' : 'rr-nonterminal';
		let box = `<rect x="${fmt(left)}" y="${fmt(y - BOX)}" width="${fmt(this.width)}" height="${BOX * 2}"${rx}/>` +
			`<text x="${fmt(left + this.width / 2)}" y="${fmt(y + 4)}">${escapeHtml(this.text)}</text>`;
		if (this.title) box = `<title>${escapeHtml(this.title)}</title>` + box;
		const group = `<g class="${cls}">${box}</g>`;
		return (this.href ? `<a href="${escapeHtml(this.href)}">${group}</a>` : group) + h(left + this.width, y, gap);
	}
}

class Marker {
	// a mid-rule action: a dot on the line
	constructor(title) {
		this.title = title;
		this.width = 12;
		this.up = 4;
		this.down = 4;
		this.needsSpace = true;
	}

	render(x, y, width) {
		return h(x, y, width) + `<circle class="rr-marker" cx="${fmt(x + 6)}" cy="${fmt(y)}" r="4"><title>${escapeHtml(this.title)}</title></circle>`;
	}
}

class Skip {
	constructor() {
		this.width = 0;
		this.up = 0;
		this.down = 0;
	}

	render(x, y, width) {
		return h(x, y, width);
	}
}

class Sequence {
	constructor(items) {
		this.items = items.length ? items : [new Skip()];
		this.width = this.items.reduce((w, it) => w + it.width + (it.needsSpace ? 20 : 0), 0);
		this.up = Math.max(...this.items.map((it) => it.up));
		this.down = Math.max(...this.items.map((it) => it.down));
	}

	render(x, y, width) {
		let out = '';
		const gap = width - this.width;
		for (const it of this.items) {
			if (it.needsSpace) {
				out += h(x, y, 10);
				x += 10;
			}
			out += it.render(x, y, it.width);
			x += it.width;
			if (it.needsSpace) {
				out += h(x, y, 10);
				x += 10;
			}
		}
		return out + h(x, y, gap);
	}
}

class Choice {
	constructor(normal, items) {
		this.normal = normal;
		this.items = items;
		this.width = Math.max(...items.map((it) => it.width)) + AR * 4;
		// the distance of each track from the main one
		this.offsets = items.map(() => 0);
		let d = 0;
		for (let i = normal - 1; i >= 0; i--) {
			d += Math.max(AR * 2, items[i + 1].up + VS + items[i].down);
			this.offsets[i] = -d;
		}
		d = 0;
		for (let i = normal + 1; i < items.length; i++) {
			d += Math.max(AR * 2, items[i - 1].down + VS + items[i].up);
			this.offsets[i] = d;
		}
		this.up = Math.max(items[normal].up, ...items.map((it, i) => -this.offsets[i] + it.up));
		this.down = Math.max(items[normal].down, ...items.map((it, i) => this.offsets[i] + it.down));
	}

	render(x, y, width) {
		const inner = width - AR * 4;
		let out = '';
		this.items.forEach((it, i) => {
			const d = this.offsets[i];
			if (i === this.normal) {
				out += h(x, y, AR * 2) + it.render(x + AR * 2, y, inner) + h(x + AR * 2 + inner, y, AR * 2);
			} else if (d < 0) {
				const v = -d - AR * 2;
				out += path(`M${fmt(x)} ${fmt(y)}a${AR} ${AR} 0 0 0 ${AR} ${-AR}v${fmt(-v)}a${AR} ${AR} 0 0 1 ${AR} ${-AR}`);
				out += it.render(x + AR * 2, y + d, inner);
				out += path(`M${fmt(x + AR * 2 + inner)} ${fmt(y + d)}a${AR} ${AR} 0 0 1 ${AR} ${AR}v${fmt(v)}a${AR} ${AR} 0 0 0 ${AR} ${AR}`);
			} else {
				const v = d - AR * 2;
				out += path(`M${fmt(x)} ${fmt(y)}a${AR} ${AR} 0 0 1 ${AR} ${AR}v${fmt(v)}a${AR} ${AR} 0 0 0 ${AR} ${AR}`);
				out += it.render(x + AR * 2, y + d, inner);
				out += path(`M${fmt(x + AR * 2 + inner)} ${fmt(y + d)}a${AR} ${AR} 0 0 0 ${AR} ${-AR}v${fmt(-v)}a${AR} ${AR} 0 0 1 ${AR} ${-AR}`);
			}
		});
		return out;
	}
}

class OneOrMore {
	// the item, then back along the bottom track (through the separator, if any) to repeat it
	constructor(item, repeat = new Skip()) {
		this.item = item;
		this.repeat = repeat;
		this.width = Math.max(item.width, repeat.width) + AR * 2;
		this.up = item.up;
		this.d = Math.max(AR * 2, item.down + VS + repeat.up);
		this.down = this.d + repeat.down;
	}

	render(x, y, width) {
		const v = this.d - AR * 2;
		let out = h(x, y, AR) + this.item.render(x + AR, y, width - AR * 2) + h(x + width - AR, y, AR);
		out += path(`M${fmt(x + AR)} ${fmt(y)}a${AR} ${AR} 0 0 0 ${-AR} ${AR}v${fmt(v)}a${AR} ${AR} 0 0 0 ${AR} ${AR}`);
		out += this.repeat.render(x + AR, y + this.d, width - AR * 2);
		out += path(`M${fmt(x + width - AR)} ${fmt(y + this.d)}a${AR} ${AR} 0 0 0 ${AR} ${-AR}v${fmt(-v)}a${AR} ${AR} 0 0 0 ${-AR} ${-AR}`);
		return out;
	}
}

export const terminal = (text, href, title) => new Box(text, { href, terminal: true, title });
export const nonterminal = (text, href, title) => new Box(text, { href, terminal: false, title });
export const marker = (title) => new Marker(title);
export const skip = () => new Skip();
export const sequence = (items) => (items.length === 1 ? items[0] : new Sequence(items));
export const choice = (items, normal = 0) => (items.length === 1 ? items[0] : new Choice(normal, items));
export const optional = (item) => new Choice(1, [new Skip(), item]);
export const oneOrMore = (item, repeat) => new OneOrMore(item, repeat);
export const zeroOrMore = (item, repeat) => optional(oneOrMore(item, repeat));

/** The whole diagram: a start bar, the item, an end bar. */
export function diagram(item, label) {
	const pad = 10;
	const width = item.width + 2 * 20 + pad * 2;
	const height = item.up + item.down + pad * 2 + 2;
	const y = pad + item.up + 1;
	const x0 = pad;
	let body = path(`M${x0} ${fmt(y - 8)}v16M${x0 + 4} ${fmt(y - 8)}v16`, 'rr-end') + h(x0, y, 20);
	body += item.render(x0 + 20, y, item.width);
	const xe = x0 + 20 + item.width;
	body += h(xe, y, 20) + path(`M${fmt(xe + 16)} ${fmt(y - 8)}v16M${fmt(xe + 20)} ${fmt(y - 8)}v16`, 'rr-end');
	return `<svg class="railroad" xmlns="http://www.w3.org/2000/svg" width="${fmt(width)}" height="${fmt(height)}" viewBox="0 0 ${fmt(width)} ${fmt(height)}" role="img" aria-label="${escapeHtml(label)}"><g transform="translate(.5 .5)">${body}</g></svg>`;
}
