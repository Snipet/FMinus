// Comments to HTML: the sources' comments are the documentation, so their words are kept as they are. Only the line
// breaks of the source's 120 columns go: a paragraph is reflowed, and the blocks a comment indents (code, a list of
// terms, a table) keep their own shape.
//
// A comment is a list of lines, each { text, end }: its text without the comment's markers, and the column the source
// line ends at, which tells a line the author broke on purpose (the next word would have fitted) from one that is full.

export const COLUMN_LIMIT = 120; // .clang-format's ColumnLimit, which the sources wrap their comments at

export function escapeHtml(text) {
	return text.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
}

const NAME = /[A-Za-z_~][A-Za-z0-9_]*(?:::[A-Za-z_~][A-Za-z0-9_]*)*(?:\(\))?/g;

/**
 * Inline text: `code` spans, and the names ctx.link resolves to a link (ctx.link(name, { code }) gives an href or
 * nothing). A name inside a longer code span is left alone.
 */
export function renderInline(text, ctx = {}) {
	let html = '';
	let last = 0;
	const spans = /`([^`]+)`/g;
	for (let m; (m = spans.exec(text)); ) {
		html += linkNames(text.slice(last, m.index), ctx);
		html += codeSpan(m[1], ctx);
		last = m.index + m[0].length;
	}
	html += linkNames(text.slice(last), ctx);
	return html;
}

function codeSpan(code, ctx) {
	const href = ctx.link ? ctx.link(code.trim(), { code: true }) : null;
	const inner = `<code>${escapeHtml(code)}</code>`;
	return href ? `<a href="${escapeHtml(href)}">${inner}</a>` : inner;
}

function linkNames(text, ctx) {
	if (!ctx.link) return escapeHtml(text);
	let html = '';
	let last = 0;
	NAME.lastIndex = 0;
	for (let m; (m = NAME.exec(text)); ) {
		// a name's start, not the middle of one ("x_y", "a::b" is one match already)
		const href = ctx.link(m[0], { code: false });
		if (!href) continue;
		html += escapeHtml(text.slice(last, m.index));
		html += `<a href="${escapeHtml(href)}">${escapeHtml(m[0])}</a>`;
		last = m.index + m[0].length;
	}
	return html + escapeHtml(text.slice(last));
}

const indentOf = (s) => s.match(/^ */)[0].length;

/**
 * Two lines of a paragraph as one: a space between, but none after a word the line break hyphenated ("value-" and
 * "initialised" are "value-initialised"). A hyphen before "and", "or", "to" or "nor" stays apart ("pre- and post-").
 */
export function joinLines(a, b) {
	if (/[A-Za-z]-$/.test(a) && /^[a-z]/.test(b) && !/^(?:and|or|to|nor)\b/.test(b)) return a + b;
	return `${a} ${b}`;
}
const isBlank = (line) => line.text.trim() === '';

/**
 * Whether the author ended this line on purpose: the next line's first word would have fitted on it with room to spare.
 * The sources aren't wrapped greedily to the column (a line stops a word or two early as often as not), so a line counts
 * as ended on purpose only when the word would still leave a few columns free.
 */
const SLACK = 6;
function brokenOnPurpose(line, next, width) {
	if (!next || isBlank(next)) return false;
	const word = next.text.trim().split(/\s+/)[0];
	return line.end + 1 + word.length <= width - SLACK;
}

/**
 * A comment's lines to HTML blocks: paragraphs, bullet lists, and the indented blocks (C++ code, highlighted; a list of
 * terms and their definitions; a table of columns; anything else as preformatted text). ctx.block(lines) may render an
 * indented block itself (the grammar's summary does), returning HTML or nothing.
 */
export function renderComment(lines, ctx = {}) {
	const width = ctx.width ?? COLUMN_LIMIT;
	const out = [];
	let para = [];
	const flush = () => {
		if (para.length) out.push(`<p>${renderInline(para.reduce(joinLines), ctx)}</p>`);
		para = [];
	};
	let i = 0;
	while (i < lines.length) {
		const line = lines[i];
		if (isBlank(line)) {
			flush();
			i++;
			continue;
		}
		if (indentOf(line.text) >= 2) {
			flush();
			let j = i;
			while (j < lines.length && !isBlank(lines[j]) && indentOf(lines[j].text) >= 2) j++;
			out.push(renderBlock(lines.slice(i, j), ctx));
			i = j;
			continue;
		}
		if (/^[-*] /.test(line.text)) {
			flush();
			const items = [];
			while (i < lines.length && /^[-*] /.test(lines[i].text)) {
				let item = lines[i].text.slice(2).trim();
				i++;
				while (i < lines.length && !isBlank(lines[i]) && /^ {2}\S/.test(lines[i].text)) {
					item = joinLines(item, lines[i].text.trim());
					i++;
				}
				items.push(`<li>${renderInline(item, ctx)}</li>`);
			}
			out.push(`<ul>${items.join('')}</ul>`);
			continue;
		}
		para.push(line.text.trim());
		if (brokenOnPurpose(line, lines[i + 1], width)) flush();
		i++;
	}
	flush();
	return out.join('\n');
}

function renderBlock(lines, ctx) {
	if (ctx.block) {
		const html = ctx.block(lines);
		if (html) return html;
	}
	const indent = Math.min(...lines.map((l) => indentOf(l.text)));
	const texts = lines.map((l) => l.text.slice(indent).trimEnd());
	if (isCode(texts) && ctx.highlight) return ctx.highlight(texts.join('\n'), 'cpp');
	const columns = columnTable(texts);
	if (columns) return columns.header ? tableHtml(columns, ctx) : termsHtml(columns, ctx);
	return `<pre class="plain">${escapeHtml(texts.join('\n'))}</pre>`;
}

/** C++ statements: every line, without its trailing // comment, ends a statement or a block. */
function isCode(texts) {
	return texts.every((t) => {
		const code = t.replace(/\/\/.*$/, '').trim();
		return code === '' ? t.trim().startsWith('//') : /[;{}]$/.test(code);
	});
}

/**
 * Columns, as the sources align them: the first line sets where each column starts (two spaces or more before it),
 * and every other line either starts a row, with a space before each column it fills, or continues the row above in
 * one of its columns. A first row of short labels is a header (a table); without one, two columns are terms and their
 * definitions.
 */
function columnTable(texts) {
	const starts = [0];
	for (let m, re = /\s{2,}(?=\S)/g; (m = re.exec(texts[0])); ) starts.push(m.index + m[0].length);
	if (starts.length < 2) return null;
	const rows = [];
	for (const t of texts) {
		const lead = indentOf(t);
		if (lead > 0) {
			// a continuation: it must begin exactly at a column, and there must be a row to continue
			const col = starts.indexOf(lead);
			if (col < 1 || rows.length === 0) return null;
			const cells = rows[rows.length - 1];
			cells[col] = cells[col] ? joinLines(cells[col], t.trim()) : t.trim();
			continue;
		}
		const cells = [];
		for (let c = 0; c < starts.length; c++) {
			const from = starts[c];
			const to = c + 1 < starts.length ? starts[c + 1] : Infinity;
			if (c > 0 && t.length > from && t[from - 1] !== ' ') return null;
			if (c + 1 < starts.length && t.length > to && t[to - 1] !== ' ') return null;
			cells.push(t.slice(from, to).trim());
		}
		if (cells[0] === '') return null;
		rows.push(cells);
	}
	const first = rows[0];
	const header = rows.length > 2 && first.every((c) => c.length > 0 && c.length <= 30) && texts.length > 1 &&
		indentOf(texts[1]) === 0;
	if (header) return { header: first, rows: rows.slice(1) };
	if (starts.length !== 2 || rows.some((r) => /\s/.test(r[0]))) return null;
	return { header: null, rows };
}

function tableHtml({ header, rows }, ctx) {
	const head = header.map((h) => `<th>${renderInline(h, ctx)}</th>`).join('');
	const body = rows.map((r) => `<tr>${r.map((c) => `<td>${renderInline(c ?? '', ctx)}</td>`).join('')}</tr>`).join('');
	return `<div class="table-wrap"><table class="comment-table"><thead><tr>${head}</tr></thead><tbody>${body}</tbody></table></div>`;
}

function termsHtml({ rows }, ctx) {
	const items = rows.map(([term, def]) => `<dt><code>${escapeHtml(term)}</code></dt><dd>${renderInline(def ?? '', ctx)}</dd>`);
	return `<dl class="terms">${items.join('')}</dl>`;
}

/** The lines of // comments, without "//" and the one space after it. */
export function lineCommentLines(tokens) {
	return tokens.map((t) => {
		const body = t.body ?? t.text;
		const text = body.startsWith(' ') ? body.slice(1) : body;
		return { text, end: t.endCol };
	});
}

/** The lines of a block comment, without its "/*", "*\/" and the " * " each line starts with. */
export function blockCommentLines(text, startCol = 1) {
	const lines = text.split('\n');
	return lines.map((raw, k) => {
		let t = raw;
		if (k === 0) t = t.replace(/^\/\*+/, '');
		if (k === lines.length - 1) t = t.replace(/\*+\/\s*$/, '');
		if (k > 0) t = t.replace(/^\s*\* ?/, '');
		else t = t.replace(/^ /, '');
		const end = (k === 0 ? startCol - 1 : 0) + raw.replace(/\*+\/\s*$/, '').trimEnd().length;
		return { text: t.trimEnd(), end };
	}).filter((l, k, all) => !(l.text === '' && (k === 0 || k === all.length - 1)));
}

/** The plain text of comment lines, as one string (for search and descriptions). */
export function plainText(lines) {
	const parts = lines.map((l) => l.text.trim()).filter(Boolean);
	return (parts.length ? parts.reduce(joinLines) : '').replace(/`/g, '');
}

/** The first sentence of a text. */
export function firstSentence(text) {
	const m = text.match(/^(.+?[.:])(\s|$)/);
	return m ? m[1] : text;
}
