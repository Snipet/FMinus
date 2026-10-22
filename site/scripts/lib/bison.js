// A reader for Bison grammars as src/frontend/Parser.y writes them: the declarations (%token, %nterm, precedence,
// %define and the rest), and the rules, each with its alternatives as symbols, without their C++ actions. A mid-rule
// action stays as a marker where it was. Anything it can't read stops the build with the file and line.
import { GenerateError } from './repo.js';

function lineStarts(src) {
	const starts = [0];
	for (let i = 0; i < src.length; i++) if (src[i] === '\n') starts.push(i + 1);
	return starts;
}

function lineOf(starts, offset) {
	let lo = 0;
	let hi = starts.length - 1;
	while (lo < hi) {
		const mid = (lo + hi + 1) >> 1;
		if (starts[mid] <= offset) lo = mid;
		else hi = mid - 1;
	}
	return lo + 1;
}

/** Skips C or C++ code in braces from src[i] === '{': strings, characters and comments may hold braces. */
export function skipBraces(src, i, fail) {
	let depth = 0;
	for (; i < src.length; i++) {
		const c = src[i];
		if (c === '"' || c === "'") {
			const quote = c;
			for (i++; i < src.length && src[i] !== quote; i++) if (src[i] === '\\') i++;
			continue;
		}
		if (c === '/' && src[i + 1] === '/') {
			i = src.indexOf('\n', i);
			if (i < 0) i = src.length;
			continue;
		}
		if (c === '/' && src[i + 1] === '*') {
			const j = src.indexOf('*/', i + 2);
			if (j < 0) fail(i, 'unterminated /* comment in code');
			i = j + 1;
			continue;
		}
		if (c === '{') depth++;
		else if (c === '}' && --depth === 0) return i + 1;
	}
	fail(i, "code in braces with no closing '}'");
}

/** Bison's tokens in src[from, to): comment, directive, code, tag, string, char, ident, number, named, punct. */
function tokenize(src, from, to, starts, file) {
	const fail = (offset, message) => {
		throw new GenerateError(`${file}:${lineOf(starts, offset)}: ${message}`);
	};
	const toks = [];
	let i = from;
	const push = (kind, start, end, extra = {}) => {
		const line = lineOf(starts, start);
		const endLine = lineOf(starts, Math.max(start, end - 1));
		const col = start - starts[line - 1] + 1;
		const endCol = Math.max(start, end - 1) - starts[endLine - 1] + 1;
		toks.push({ kind, start, end, line, col, endLine, endCol, text: src.slice(start, end), ...extra });
	};
	while (i < to) {
		const c = src[i];
		if (/\s/.test(c)) {
			i++;
			continue;
		}
		const start = i;
		if (c === '/' && src[i + 1] === '/') {
			let j = src.indexOf('\n', i);
			if (j < 0 || j > to) j = to;
			push('comment', start, j, { style: 'line', body: src.slice(i + 2, j) });
			i = j;
		} else if (c === '/' && src[i + 1] === '*') {
			const j = src.indexOf('*/', i + 2);
			if (j < 0) fail(i, 'unterminated /* comment');
			push('comment', start, j + 2, { style: 'block', body: src.slice(i + 2, j) });
			i = j + 2;
		} else if (c === '%' && src[i + 1] === '{') {
			const j = src.indexOf('%}', i + 2);
			if (j < 0) fail(i, "a %{ with no %}");
			push('code', start, j + 2);
			i = j + 2;
		} else if (c === '%' && src[i + 1] === '?' && src[i + 2] === '{') {
			const j = skipBraces(src, i + 2, fail);
			push('code', start, j, { predicate: true });
			i = j;
		} else if (c === '%') {
			const m = /^%[A-Za-z][\w.-]*/.exec(src.slice(i, i + 64));
			if (!m) fail(i, `a '%' I can't read`);
			push('directive', start, i + m[0].length, { name: m[0] });
			i += m[0].length;
		} else if (c === '{') {
			const j = skipBraces(src, i, fail);
			push('code', start, j);
			i = j;
		} else if (c === '<') {
			let depth = 0;
			let j = i;
			for (; j < to; j++) {
				if (src[j] === '<') depth++;
				else if (src[j] === '>' && src[j - 1] !== '-' && --depth === 0) break;
				else if (src[j] === '\n') fail(i, 'a <type> tag that runs past its line');
			}
			push('tag', start, j + 1);
			i = j + 1;
		} else if (c === '"' || c === "'") {
			let j = i + 1;
			for (; j < to && src[j] !== c; j++) {
				if (src[j] === '\\') j++;
				if (src[j] === '\n') fail(i, 'a literal that runs past its line');
			}
			push(c === '"' ? 'string' : 'char', start, j + 1);
			i = j + 1;
		} else if (/[A-Za-z_.]/.test(c)) {
			const m = /^[A-Za-z_.][\w.]*(?:-[A-Za-z_][\w.]*)*/.exec(src.slice(i, i + 256));
			push('ident', start, i + m[0].length);
			i += m[0].length;
		} else if (/[0-9]/.test(c)) {
			const m = /^[0-9]+/.exec(src.slice(i, i + 32));
			push('number', start, i + m[0].length);
			i += m[0].length;
		} else if (c === '[') {
			const m = /^\[[A-Za-z_][\w.-]*\]/.exec(src.slice(i, i + 128));
			if (!m) fail(i, "a '[' that is not a named reference");
			push('named', start, i + m[0].length);
			i += m[0].length;
		} else if (':|;=,'.includes(c)) {
			push('punct', start, i + 1);
			i++;
		} else {
			fail(i, `a character I can't read in the grammar: '${c}'`);
		}
	}
	for (let k = 0; k < toks.length; k++) toks[k].blankBefore = k > 0 && toks[k].line - toks[k - 1].endLine >= 2;
	return toks;
}

const PRECEDENCE = new Set(['%left', '%right', '%nonassoc', '%precedence']);

export function parseBison(src, file) {
	const starts = lineStarts(src);
	const fail = (offset, message) => {
		throw new GenerateError(`${file}:${lineOf(starts, offset)}: ${message}`);
	};
	const seps = [...src.matchAll(/^%%[ \t]*$/gm)].map((m) => m.index);
	if (seps.length < 1) throw new GenerateError(`${file}: no %% line: not a Bison grammar`);
	const rulesFrom = seps[0] + 2;
	const rulesTo = seps.length > 1 ? seps[1] : src.length;

	// ---------------------------------------------------------------------------------------------- declarations
	const pro = tokenize(src, 0, seps[0], starts, file);
	const header = [];
	let k = 0;
	while (pro[k]?.kind === 'comment' && (header.length === 0 || !pro[k].blankBefore)) header.push(pro[k++]);

	const tokens = [];
	const tokenByName = new Map();
	const tokenByAlias = new Map();
	const nterms = new Map();
	const precedence = [];
	const declarations = [];
	let pending = [];
	while (k < pro.length) {
		const t = pro[k];
		if (t.kind === 'comment') {
			if (pending.length && t.blankBefore) pending = [];
			pending.push(t);
			k++;
			continue;
		}
		if (t.kind === 'code') {
			fail(t.start, 'code outside a directive (a %{ %} prologue): the reader knows %code blocks only');
		}
		if (t.kind !== 'directive') fail(t.start, `'${t.text}' outside a directive`);
		const doc = pending.length && !t.blankBefore ? pending : [];
		pending = [];
		// a directive's arguments: everything up to the next directive or comment
		let j = k + 1;
		while (j < pro.length && pro[j].kind !== 'directive' && pro[j].kind !== 'comment') j++;
		const args = pro.slice(k + 1, j);
		const last = args.length ? args[args.length - 1] : t;
		const text = src.slice(t.start, last.end);
		const name = t.name;
		if (name === '%token') {
			let type = null;
			for (const a of args) {
				if (a.kind === 'tag') type = a.text.slice(1, -1);
				else if (a.kind === 'ident') {
					const token = { name: a.text, alias: null, type, line: a.line };
					tokens.push(token);
					tokenByName.set(token.name, token);
				} else if (a.kind === 'string') {
					const token = tokens[tokens.length - 1];
					if (!token || token.alias) fail(a.start, `an alias with no token before it: ${a.text}`);
					token.alias = a.text;
					tokenByAlias.set(a.text, token);
				} else if (a.kind !== 'number') fail(a.start, `%token: '${a.text}' is not a token, an alias or a <type>`);
			}
		} else if (name === '%nterm' || name === '%type') {
			let type = null;
			for (const a of args) {
				if (a.kind === 'tag') type = a.text.slice(1, -1);
				else if (a.kind === 'ident') nterms.set(a.text, type);
				else if (a.kind !== 'string') fail(a.start, `${name}: '${a.text}' is not a symbol or a <type>`);
			}
		} else if (PRECEDENCE.has(name)) {
			const symbols = [];
			for (const a of args) {
				if (a.kind === 'ident' || a.kind === 'string' || a.kind === 'char') symbols.push(a.text);
				else if (a.kind !== 'tag') fail(a.start, `${name}: '${a.text}' is not a symbol`);
			}
			precedence.push({ assoc: name.slice(1), symbols, doc, line: t.line });
		} else if (name === '%code') {
			const qualifier = args.find((a) => a.kind === 'ident');
			declarations.push({ text: `%code${qualifier ? ' ' + qualifier.text : ''} { … }`, line: t.line, code: true, doc });
		} else {
			declarations.push({ text: text.replace(/\s*\n\s*/g, ' '), line: t.line, doc });
		}
		k = j;
	}
	// The declarations as Parser.y writes them, comments included, with the code of %code and %{ %} left out.
	const fromOffset = header.length ? header[header.length - 1].end : 0;
	let prologue = '';
	let cursor = fromOffset;
	for (const t of pro) {
		if (t.kind !== 'code' || t.start < fromOffset) continue;
		prologue += src.slice(cursor, t.start) + (t.text.startsWith('%{') ? '%{ … %}' : '{ … }');
		cursor = t.end;
	}
	prologue = (prologue + src.slice(cursor, seps[0])).replace(/^\s*\n/, '').replace(/\s+$/, '');

	// a precedence name that no %token declares (THEN, UNARY) is a token for %prec only
	for (const level of precedence) {
		for (const s of level.symbols) {
			if (s.startsWith('"') || s.startsWith("'")) {
				if (!tokenByAlias.has(s)) fail(starts[level.line - 1], `${s} has precedence, but no %token has it as its alias`);
			} else if (!tokenByName.has(s)) {
				const token = { name: s, alias: null, type: null, line: level.line, precedenceOnly: true };
				tokens.push(token);
				tokenByName.set(s, token);
			}
		}
	}

	// ------------------------------------------------------------------------------------------------- rules
	// Comments: one above a rule documents it; one after a rule's ';', on its line, too. Inside a rule, one on its own
	// line documents the alternative after it (or the one it opens, before its first symbol), and one at the end of a
	// line the alternative on that line, with the lines that go on below it at its column. Comments on their own lines
	// inside an action are the alternative's too. Any other comment documents nothing, and stops the build.
	const toks = tokenize(src, rulesFrom, rulesTo, starts, file);
	const rules = [];
	let rule = null;
	let alt = null;
	let prev = null; // the last token that is not a comment
	let between = []; // comments on their own lines inside a rule, for the alternative after them
	let lastComment = null; // the last comment, where it went and whether it ends a line: { token, into, trailing }
	pending = [];
	const floatingAt = (comments, why) =>
		fail(comments[0].start, `a comment that documents nothing: ${why}. Put it directly above the rule or the ` +
			'alternative it is about, or after it on its line.');
	const newAlt = (line) => ({ items: [], prec: null, notes: [], actionNotes: [], line });
	const nextSolid = (k) => {
		while (toks[k]?.kind === 'comment') k++;
		return k;
	};
	for (let i = 0; i < toks.length; i++) {
		const t = toks[i];
		if (t.kind === 'comment') {
			const place = commentPlace(t);
			place.into.push(t);
			lastComment = { token: t, ...place };
			continue;
		}
		lastComment = null;
		const at = nextSolid(i + 1);
		const next = toks[at];
		const startsRule = t.kind === 'ident' && (next?.kind === 'punct' && next.text === ':' ||
			(next?.kind === 'named' && toks[nextSolid(at + 1)]?.text === ':'));
		if (startsRule) {
			if (rule && !rule.closed) {
				// a rule with no ';': the comments on their own lines after it are the next rule's
				pending = between;
				between = [];
				closeRule(null);
			}
			if (pending.length && t.blankBefore) floatingAt(pending, `a blank line separates it from rule ${t.text}`);
			rule = { name: t.text, line: t.line, doc: pending, after: [], alternatives: [], closed: false };
			pending = [];
			between = [];
			rules.push(rule);
			// comments between the rule's name and its ':' are the rule's
			for (let k = i + 1; k < toks.length && toks[k].kind === 'comment'; k++) rule.after.push(toks[k]);
			i = next.kind === 'named' ? nextSolid(at + 1) : at;
			prev = toks[i];
			alt = newAlt(t.line);
			rule.alternatives.push(alt);
			continue;
		}
		if (!rule || rule.closed) fail(t.start, `'${t.text}' outside a rule`);
		if (t.kind === 'punct' && t.text === '|') {
			alt = newAlt(t.line);
			alt.notes.push(...between);
			between = [];
			rule.alternatives.push(alt);
		} else if (t.kind === 'punct' && t.text === ';') {
			closeRule(t);
			rule.endLine = t.line;
		} else if (t.kind === 'ident') {
			alt.items.push({ kind: 'symbol', name: t.text, line: t.line });
			if (next?.kind === 'named') i = at;
		} else if (t.kind === 'string' || t.kind === 'char') {
			alt.items.push({ kind: 'literal', text: t.text, line: t.line });
			if (next?.kind === 'named') i = at;
		} else if (t.kind === 'directive' && t.name === '%empty') {
			alt.items.push({ kind: 'empty' });
		} else if (t.kind === 'directive' && t.name === '%prec') {
			if (!next || !['ident', 'string', 'char'].includes(next.kind)) fail(t.start, '%prec without a symbol');
			alt.prec = next.text;
			i = at;
		} else if (t.kind === 'directive' && ['%dprec', '%merge', '%expect', '%expect-rr'].includes(t.name)) {
			i = at;
		} else if (t.kind === 'tag' && next?.kind === 'code') {
			alt.items.push({ kind: 'action', tag: t.text.slice(1, -1), line: t.line });
			alt.actionNotes.push(...actionComments(next));
			i = at;
		} else if (t.kind === 'code') {
			alt.items.push({ kind: 'action', line: t.line, predicate: !!t.predicate });
			alt.actionNotes.push(...actionComments(t));
		} else {
			fail(t.start, `'${t.text}' in rule ${rule.name}: not a symbol, an action, %empty or %prec`);
		}
		prev = toks[i];
	}
	if (rule && !rule.closed) closeRule(null);
	if (pending.length) floatingAt(pending, 'no rule follows it');

	/**
	 * Where a comment goes, by where it is: { into, trailing }, trailing when it ends a line of the grammar (the lines
	 * right below it, at its column, go on with it).
	 */
	function commentPlace(t) {
		if (lastComment?.trailing && t.style === 'line' && t.line === lastComment.token.line + 1 && t.col === lastComment.token.col) {
			return { into: lastComment.into, trailing: true };
		}
		const sameLine = prev && t.line === prev.endLine;
		if (rule?.closed && sameLine && prev.kind === 'punct' && prev.text === ';') return { into: rule.after, trailing: true };
		if (rule && !rule.closed) {
			if (sameLine) return { into: alt.notes, trailing: true };
			if (alt.items.length === 0 && !between.length) return { into: alt.notes, trailing: false };
			return { into: between, trailing: false };
		}
		if (pending.length && t.blankBefore) floatingAt(pending, 'a blank line separates it from what follows');
		return { into: pending, trailing: false };
	}

	function closeRule(t) {
		if (between.length) {
			floatingAt(between, `it follows the last alternative of rule ${rule.name}${t ? ", before its ';'" : ''}`);
		}
		rule.closed = true;
	}

	/** The comments on their own lines in an action's code: what they say about the alternative. */
	function actionComments(code) {
		const out = [];
		const lines = code.text.split('\n');
		let last = -2;
		lines.forEach((text, k) => {
			// the first line starts with the action's '{'
			const m = (k === 0 ? /^\{\s*\/\/(.*)$/ : /^\s*\/\/(.*)$/).exec(text);
			if (!m) return;
			const endCol = (k === 0 ? code.col - 1 : 0) + text.trimEnd().length;
			if (out.length && last !== k - 1) out.push({ style: 'line', body: '', endCol: 0, line: code.line + k });
			out.push({ style: 'line', body: m[1], endCol, line: code.line + k });
			last = k;
		});
		return out;
	}

	if (!rules.length) throw new GenerateError(`${file}: no rules after %%`);

	return { header, tokens, tokenByName, tokenByAlias, nterms, precedence, declarations, prologue, rules, lineOf: (o) => lineOf(starts, o) };
}
