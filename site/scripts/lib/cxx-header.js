// A reader for C++ headers written as include/fminus/FMinus.h is: namespaces, classes and structs, enums, functions
// (declared, or defined inline), fields, macros, and the // comments that document them, above a declaration or after
// it on its line. It is not a C++ parser: it tracks brackets and declarations, and it stops with the file and line of
// anything it cannot place, so a change to the header can't silently drop part of it.
import { GenerateError } from './repo.js';

// ---------------------------------------------------------------------------------------------------------- tokens

function lineStarts(src) {
	const starts = [0];
	for (let i = 0; i < src.length; i++) if (src[i] === '\n') starts.push(i + 1);
	return starts;
}

function locate(starts, offset) {
	let lo = 0;
	let hi = starts.length - 1;
	while (lo < hi) {
		const mid = (lo + hi + 1) >> 1;
		if (starts[mid] <= offset) lo = mid;
		else hi = mid - 1;
	}
	return { line: lo + 1, col: offset - starts[lo] + 1 };
}

/** Tokens: comment, pp (a whole directive line), ident, number, string, char, punct. */
export function tokenize(src, file) {
	const starts = lineStarts(src);
	const toks = [];
	const n = src.length;
	let i = 0;
	let lineBegin = true; // nothing but blanks since the line began
	const push = (kind, start, end, extra = {}) => {
		const a = locate(starts, start);
		const b = locate(starts, Math.max(start, end - 1));
		toks.push({ kind, start, end, line: a.line, col: a.col, endLine: b.line, endCol: b.col, text: src.slice(start, end), ...extra });
	};
	const fail = (offset, message) => {
		throw new GenerateError(`${file}:${locate(starts, offset).line}: ${message}`);
	};
	const skipQuoted = (j, quote) => {
		j++;
		while (j < n && src[j] !== quote) {
			if (src[j] === '\\') j++;
			if (src[j] === '\n') fail(j, `unterminated ${quote === '"' ? 'string' : 'character'} literal`);
			j++;
		}
		if (j >= n) fail(j, 'unterminated literal');
		return j + 1;
	};
	while (i < n) {
		const c = src[i];
		if (c === '\n') {
			lineBegin = true;
			i++;
			continue;
		}
		if (c === ' ' || c === '\t' || c === '\r' || c === '\f') {
			i++;
			continue;
		}
		const start = i;
		if (c === '/' && src[i + 1] === '/') {
			let j = src.indexOf('\n', i);
			if (j < 0) j = n;
			push('comment', start, j, { style: 'line', body: src.slice(i + 2, j) });
			i = j;
			lineBegin = false;
			continue;
		}
		if (c === '/' && src[i + 1] === '*') {
			const j = src.indexOf('*/', i + 2);
			if (j < 0) fail(i, 'unterminated /* comment');
			push('comment', start, j + 2, { style: 'block', body: src.slice(i + 2, j) });
			i = j + 2;
			lineBegin = false;
			continue;
		}
		if (c === '#' && lineBegin) {
			let j = i;
			while (j < n && src[j] !== '\n') {
				if (src[j] === '\\' && src[j + 1] === '\n') {
					j += 2;
					continue;
				}
				if (src[j] === '/' && (src[j + 1] === '/' || src[j + 1] === '*')) break;
				if (src[j] === '"') {
					j = skipQuoted(j, '"');
					continue;
				}
				j++;
			}
			let end = j;
			while (end > i && /\s/.test(src[end - 1])) end--;
			push('pp', start, end);
			i = end;
			lineBegin = false;
			continue;
		}
		lineBegin = false;
		if (/[A-Za-z_]/.test(c)) {
			let j = i + 1;
			while (j < n && /[A-Za-z0-9_]/.test(src[j])) j++;
			push('ident', start, j);
			i = j;
			continue;
		}
		if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(src[i + 1]))) {
			let j = i + 1;
			while (j < n && (/[A-Za-z0-9_.']/.test(src[j]) || (/[+-]/.test(src[j]) && /[eEpP]/.test(src[j - 1])))) j++;
			push('number', start, j);
			i = j;
			continue;
		}
		if (c === '"' || c === "'") {
			const j = skipQuoted(i, c);
			push(c === '"' ? 'string' : 'char', start, j);
			i = j;
			continue;
		}
		const two = src.slice(i, i + 2);
		const len = two === '::' || two === '->' ? 2 : 1;
		push('punct', start, i + len);
		i += len;
	}
	for (let k = 0; k < toks.length; k++) toks[k].blankBefore = k > 0 && toks[k].line - toks[k - 1].endLine >= 2;
	return toks;
}

// ---------------------------------------------------------------------------------------------------------- parser

const SPECIFIERS = new Set(['public', 'private', 'protected']);

/**
 * The header as a tree of scopes. Each declaration is
 *   { kind, name, line, endLine, access, doc: [comment tokens above it], trailing: [comment tokens after it],
 *     tokens, signature, definition, children, enumerators, ... }
 * kind: namespace, class, struct, union, enum, forward, function, field, macro, other.
 */
export function parseHeader(src, file) {
	const p = new Parser(src, file);
	return p.parseFile();
}

class Parser {
	constructor(src, file) {
		this.src = src;
		this.file = file;
		this.toks = tokenize(src, file);
		this.i = 0;
		this.includes = [];
		this.floating = [];
	}

	peek(k = 0) {
		return this.toks[this.i + k];
	}

	is(tok, kind, text) {
		return tok && tok.kind === kind && (text === undefined || tok.text === text);
	}

	fail(tok, message) {
		throw new GenerateError(`${this.file}:${tok ? tok.line : 'end'}: ${message}`);
	}

	expect(kind, text) {
		const t = this.peek();
		if (!this.is(t, kind, text)) this.fail(t, `expected '${text ?? kind}', found '${t ? t.text : 'the end of the file'}'`);
		this.i++;
		return t;
	}

	parseFile() {
		const intro = [];
		while (this.is(this.peek(), 'comment') && (intro.length === 0 || !this.peek().blankBefore)) intro.push(this.toks[this.i++]);
		const root = { kind: 'file', name: '', qualified: [], access: 'public', children: [] };
		this.parseBody(root, false);
		return { intro, root, includes: this.includes, floating: this.floating };
	}

	/**
	 * A scope's declarations, up to its '}' (left for the caller) or the end of the file.
	 *
	 * A comment directly above a #define documents that macro. One directly above an #if documents the block: it goes
	 * on to the declaration right after the block's #endif (fminus::math, after the #if that defines
	 * FMINUS_MATH_CONST), and the block's macros are documented by it only where it names them (api.js checks that);
	 * with no declaration right after the block, the comment is its macros' own.
	 */
	parseBody(scope, braced) {
		let pending = [];
		let lastDecl = null;
		let lastTrailing = null;
		let access = scope.kind === 'class' ? 'private' : 'public';
		const macros = new Map();
		const conditions = [];
		let carried = null; // a block's comment, after its #endif, waiting for the declaration after the block
		const settle = (decl) => {
			if (!carried) return;
			const block = carried;
			carried = null;
			if (decl) {
				decl.doc = block.doc;
				for (const m of block.macros) if (!m.doc.length) m.blockOwner = decl;
				return;
			}
			const [first, ...rest] = block.macros.filter((m) => !m.doc.length);
			if (!first) {
				this.floating.push({ scope, access: block.access, comments: block.doc });
				return;
			}
			first.doc = block.doc;
			for (const m of rest) m.blockOwner = first;
		};
		for (;;) {
			const t = this.peek();
			if (!t) {
				if (braced) this.fail(null, `the body of ${scope.kind} ${scope.name} has no closing '}'`);
				if (conditions.length) this.fail(null, `an #if with no #endif in ${scope.kind} ${scope.name || 'the file'}`);
				settle(null);
				if (pending.length) this.floating.push({ scope, access, comments: pending });
				return;
			}
			if (t.kind === 'comment') {
				this.i++;
				if (lastDecl && t.line === lastDecl.endLine) {
					lastDecl.trailing.push(t);
					lastTrailing = t;
					continue;
				}
				if (lastTrailing && t.line === lastTrailing.line + 1 && t.col === lastTrailing.col && t.style === 'line') {
					lastDecl.trailing.push(t);
					lastTrailing = t;
					continue;
				}
				settle(null);
				lastDecl = null;
				lastTrailing = null;
				if (pending.length && t.blankBefore) {
					this.floating.push({ scope, access, comments: pending });
					pending = [];
				}
				pending.push(t);
				continue;
			}
			lastTrailing = null;
			if (this.is(t, 'punct', '}')) {
				if (!braced) this.fail(t, "a '}' that closes nothing");
				if (conditions.length) this.fail(t, `an #if with no #endif in ${scope.kind} ${scope.name}`);
				settle(null);
				if (pending.length) this.floating.push({ scope, access, comments: pending });
				return;
			}
			let doc = [];
			if (pending.length) {
				if (t.blankBefore) this.floating.push({ scope, access, comments: pending });
				else doc = pending;
				pending = [];
			}
			if (t.kind === 'pp') {
				this.i++;
				settle(null);
				const result = this.directive(t, scope, access, doc, macros, conditions);
				if (result?.kind === 'pragma') pending = doc; // a comment above a #pragma goes on to what follows
				else if (result?.kind === 'endif') carried = result.block;
				else if (doc.length && !result?.takesDoc) this.floating.push({ scope, access, comments: doc });
				lastDecl = result?.record ?? { trailing: [], endLine: t.endLine };
				continue;
			}
			if (t.kind === 'ident' && SPECIFIERS.has(t.text) && this.is(this.peek(1), 'punct', ':')) {
				settle(null);
				if (doc.length) this.floating.push({ scope, access, comments: doc });
				access = t.text;
				this.i += 2;
				lastDecl = { trailing: [], endLine: t.line };
				continue;
			}
			const takes = carried && !t.blankBefore && !doc.length;
			const decl = this.declaration(scope, access, doc);
			settle(takes ? decl : null);
			scope.children.push(decl);
			lastDecl = decl;
		}
	}

	/**
	 * One preprocessor line. Returns what it is ({ kind }), the declaration a trailing comment on its line belongs to
	 * (record), whether it took the comment above it (takesDoc), and for an #endif the block it closes.
	 */
	directive(t, scope, access, doc, macros, conditions) {
		const m = t.text.match(/^#\s*(\w+)\s*(.*)$/s);
		if (!m) this.fail(t, `a preprocessor line I can't read: ${t.text}`);
		const [, name, rest] = m;
		switch (name) {
			case 'pragma':
				return { kind: 'pragma', takesDoc: true };
			case 'include': {
				const record = { kind: 'include', text: t.text, line: t.line, endLine: t.endLine, doc, trailing: [] };
				this.includes.push(record);
				return { kind: 'include', record, takesDoc: true };
			}
			case 'if':
			case 'ifdef':
			case 'ifndef':
				conditions.push({ text: t.text, doc, access, macros: [] });
				return { kind: 'if', takesDoc: true };
			case 'elif':
			case 'else':
				if (!conditions.length) this.fail(t, `#${name} without #if`);
				conditions[conditions.length - 1].text = t.text;
				return { kind: name };
			case 'endif': {
				if (!conditions.length) this.fail(t, '#endif without #if');
				const block = conditions.pop();
				const outer = conditions.findLast((c) => c.doc.length);
				if (outer) {
					// a block inside a documented block: its macros are the outer block's
					for (const macro of block.macros) if (!outer.macros.includes(macro)) outer.macros.push(macro);
				}
				return { kind: 'endif', block: block.doc.length ? block : null };
			}
			case 'define': {
				const dm = rest.match(/^([A-Za-z_]\w*)(\([^)]*\))?\s*(.*)$/s);
				if (!dm) this.fail(t, `a #define I can't read: ${t.text}`);
				let macro = macros.get(dm[1]);
				if (!macro) {
					macro = {
						kind: 'macro', name: dm[1], line: t.line, endLine: t.endLine, access, doc: [], trailing: [],
						definitions: [], parent: scope,
					};
					macros.set(dm[1], macro);
					scope.children.push(macro);
				}
				if (doc.length) {
					// a second definition's comment (an #else's) adds a paragraph
					macro.doc = macro.doc.length ? [...macro.doc, { kind: 'comment', style: 'line', body: '', endCol: 0 }, ...doc] : doc;
				}
				const block = conditions.findLast((c) => c.doc.length);
				if (block && !block.macros.includes(macro)) block.macros.push(macro);
				macro.definitions.push({ condition: conditions.length ? conditions[conditions.length - 1].text : null, text: t.text, line: t.line });
				macro.endLine = t.endLine;
				return { kind: 'define', record: macro, takesDoc: true };
			}
			default:
				this.fail(t, `a #${name} line: the API reader knows #include, #pragma, #define and #if blocks only`);
		}
	}

	declaration(scope, access, doc) {
		const first = this.peek();
		let template = null;
		if (this.is(first, 'ident', 'template')) template = this.templateHeader();
		const t = this.peek();
		if (this.is(t, 'ident', 'namespace')) {
			if (template) this.fail(t, 'a template namespace');
			return this.namespace(scope, access, doc);
		}
		if (t.kind === 'ident' && ['class', 'struct', 'union', 'enum'].includes(t.text) && this.definesType()) {
			if (template) this.fail(t, 'a class template: the API reader does not know these yet');
			return this.typeDefinition(scope, access, doc);
		}
		return this.simpleDeclaration(scope, access, doc, template, first);
	}

	templateHeader() {
		const start = this.peek();
		this.i++;
		this.expect('punct', '<');
		let depth = 1;
		while (depth > 0) {
			const t = this.peek();
			if (!t) this.fail(start, 'unterminated template header');
			if (this.is(t, 'punct', '<')) depth++;
			else if (this.is(t, 'punct', '>')) depth--;
			this.i++;
		}
		const end = this.toks[this.i - 1];
		return { text: this.src.slice(start.start, end.end).replace(/\s+/g, ' '), line: start.line };
	}

	/** Whether the class/struct/union/enum keyword here begins a definition (a body before the ';'). */
	definesType() {
		for (let k = this.i + 1; k < this.toks.length; k++) {
			const t = this.toks[k];
			if (this.is(t, 'punct', '{')) return true;
			if (this.is(t, 'punct', ';') || this.is(t, 'punct', '(') || this.is(t, 'punct', '=')) return false;
		}
		return false;
	}

	namespace(scope, access, doc) {
		const kw = this.expect('ident', 'namespace');
		const parts = [];
		while (!this.is(this.peek(), 'punct', '{')) {
			const t = this.peek();
			if (!t) this.fail(kw, 'unterminated namespace');
			if (t.kind === 'ident') parts.push(t.text);
			else if (!this.is(t, 'punct', '::')) this.fail(t, `unexpected '${t.text}' in a namespace's name`);
			this.i++;
		}
		this.i++;
		const node = {
			kind: 'namespace', name: parts.join('::'), line: kw.line, access, doc, trailing: [],
			qualified: [...scope.qualified, ...parts], children: [], parent: scope,
		};
		this.parseBody(node, true);
		const close = this.expect('punct', '}');
		node.endLine = close.line;
		node.closing = true;
		return node;
	}

	typeDefinition(scope, access, doc) {
		const kw = this.peek();
		this.i++;
		let kind = kw.text;
		let scoped = false;
		if (kind === 'enum' && (this.is(this.peek(), 'ident', 'class') || this.is(this.peek(), 'ident', 'struct'))) {
			scoped = true;
			this.i++;
		}
		const nameTok = this.expect('ident');
		let head = this.src.slice(kw.start, nameTok.end);
		// a base or an underlying type, up to the body
		const afterName = this.i;
		while (!this.is(this.peek(), 'punct', '{')) this.i++;
		if (this.i > afterName) head += ' ' + this.src.slice(this.toks[afterName].start, this.toks[this.i - 1].end).replace(/\s+/g, ' ');
		this.i++; // {
		const node = {
			kind, scoped, name: nameTok.text, line: kw.line, access, doc, trailing: [], signature: head,
			qualified: [...scope.qualified, nameTok.text], children: [], parent: scope,
		};
		if (kind === 'enum') node.enumerators = this.enumerators(node);
		else this.parseBody(node, true);
		this.expect('punct', '}');
		const t = this.peek();
		if (!this.is(t, 'punct', ';')) this.fail(t, `a declarator after the body of ${kind} ${node.name}: the API reader wants '};'`);
		this.i++;
		node.endLine = t.line;
		node.closing = true;
		return node;
	}

	enumerators(node) {
		const list = [];
		let pending = [];
		let last = null;
		for (;;) {
			const t = this.peek();
			if (!t) this.fail(null, `enum ${node.name} has no closing '}'`);
			if (this.is(t, 'punct', '}')) break;
			if (t.kind === 'comment') {
				this.i++;
				if (last && t.line === last.line) last.trailing.push(t);
				else pending.push(t);
				continue;
			}
			if (this.is(t, 'punct', ',')) {
				this.i++;
				continue;
			}
			if (t.kind !== 'ident') this.fail(t, `enum ${node.name}: an enumerator I can't read, at '${t.text}'`);
			this.i++;
			let value = null;
			if (this.is(this.peek(), 'punct', '=')) {
				this.i++;
				const from = this.peek();
				let depth = 0;
				let to = from;
				while (this.peek() && !(depth === 0 && (this.is(this.peek(), 'punct', ',') || this.is(this.peek(), 'punct', '}')))) {
					const u = this.peek();
					if (this.is(u, 'punct', '(')) depth++;
					if (this.is(u, 'punct', ')')) depth--;
					to = u;
					this.i++;
				}
				value = this.src.slice(from.start, to.end);
			}
			last = { name: t.text, value, line: t.line, doc: pending, trailing: [] };
			pending = [];
			list.push(last);
		}
		if (pending.length) this.floating.push({ scope: node, access: 'public', comments: pending });
		return list;
	}

	matchBrace(k) {
		let depth = 0;
		for (; k < this.toks.length; k++) {
			const t = this.toks[k];
			if (this.is(t, 'punct', '{')) depth++;
			else if (this.is(t, 'punct', '}')) {
				depth--;
				if (depth === 0) return k;
			}
		}
		this.fail(this.toks[k - 1], "unbalanced '{'");
	}

	simpleDeclaration(scope, access, doc, template, first) {
		const begin = this.i;
		let paren = 0;
		let bracket = 0;
		let angle = 0; // a template's <...>, outside parentheses
		let firstParen = -1;
		let closeParen = -1;
		let assign = false;
		let body = null;
		let end = -1;
		for (;;) {
			const t = this.peek();
			if (!t) this.fail(this.toks[begin], 'a declaration with no end');
			if (t.kind === 'pp') this.fail(t, 'a preprocessor line inside a declaration');
			if (t.kind === 'punct') {
				const x = t.text;
				if (x === '(') {
					if (paren === 0 && bracket === 0 && firstParen < 0 && !assign) firstParen = this.i;
					paren++;
				} else if (x === ')') {
					paren--;
					if (paren === 0 && firstParen >= 0 && closeParen < 0) closeParen = this.i;
				} else if (x === '[') bracket++;
				else if (x === ']') bracket--;
				else if ((x === '<' || x === '>') && paren === 0 && bracket === 0 && !assign && !this.afterOperator(this.i)) {
					angle += x === '<' ? 1 : -1;
					if (angle < 0) this.fail(t, `a '>' that closes no '<' in a declaration`);
				} else if (x === ',' && paren === 0 && bracket === 0 && angle === 0 && !this.afterOperator(this.i)) {
					this.fail(t, 'more than one declarator in one declaration: the API reader gives each declaration its ' +
						'own entry and comment, so declare each name on its own line');
				} else if (x === '=' && paren === 0 && bracket === 0 && firstParen < 0 && !this.afterOperator(this.i)) assign = true;
				else if (x === ';' && paren === 0 && bracket === 0) {
					end = this.i;
					this.i++;
					break;
				} else if (x === '{' && paren === 0 && bracket === 0) {
					const close = this.matchBrace(this.i);
					if (firstParen >= 0 && closeParen >= 0 && !assign) {
						body = { open: this.i, close };
						end = close;
						this.i = close + 1;
						if (this.is(this.peek(), 'punct', ';')) this.i++;
						break;
					}
					this.i = close + 1; // a braced initialiser
					continue;
				} else if (x === '}') this.fail(t, "a '}' inside a declaration");
			}
			this.i++;
		}
		if (angle !== 0) this.fail(this.toks[begin], `a '<' that no '>' closes in a declaration`);
		const lastIndex = body ? body.open - 1 : end;
		const toks = this.toks.slice(begin, lastIndex + 1).filter((t) => t.kind !== 'comment');
		const inner = this.toks.slice(begin, lastIndex + 1).filter((t) => t.kind === 'comment');
		if (inner.length) this.fail(inner[0], 'a comment inside a declaration: put it above the declaration, or after it');
		const node = {
			kind: 'other', name: null, line: (template ?? first).line, endLine: this.toks[end].endLine, access, doc,
			trailing: [], template, parent: scope, tokens: toks,
		};
		node.signature = (template ? template.text + ' ' : '') + this.text(toks[0], toks[toks.length - 1]);
		if (body) node.definition = this.src.slice((template ? first : toks[0]).start, this.toks[body.close].end);
		const isFunction = firstParen >= 0 && !assign;
		if (isFunction) {
			node.kind = 'function';
			const name = this.functionName(firstParen);
			if (!name) this.fail(this.toks[firstParen], 'a function whose name I cannot find');
			node.name = name;
			const words = new Set(this.toks.slice(begin, firstParen).filter((t) => t.kind === 'ident').map((t) => t.text));
			node.isStatic = words.has('static');
			const after = this.toks.slice(closeParen + 1, lastIndex + 1).map((t) => t.text).join(' ');
			node.deleted = /=\s*delete/.test(after);
			node.isConst = /^const\b/.test(after);
			if (name === scope.name) node.special = 'constructor';
			else if (name === '~' + scope.name) node.special = 'destructor';
			else if (name.startsWith('operator')) node.special = 'operator';
		} else if (toks.length === 3 && ['class', 'struct', 'union', 'enum'].includes(toks[0].text)) {
			node.kind = 'forward';
			node.name = toks[1].text;
		} else {
			node.kind = 'field';
			const at = this.fieldName(toks);
			if (at < 0) this.fail(toks[0], `a declaration I cannot read: ${node.signature}`);
			node.name = toks[at].text;
			node.typeText = at > 0 ? this.text(toks[0], toks[at - 1]) : '';
			if (toks.some((t) => this.is(t, 'ident', 'using') || this.is(t, 'ident', 'typedef'))) node.kind = 'alias';
			node.isStatic = toks.some((t) => this.is(t, 'ident', 'static'));
		}
		node.qualified = [...scope.qualified, node.name];
		return node;
	}

	/** Whether the token at k is part of an operator's name (the = of operator=), not an initialiser's =. */
	afterOperator(k) {
		k--;
		while (k >= 0 && this.toks[k].kind === 'punct') k--;
		return k >= 0 && this.is(this.toks[k], 'ident', 'operator');
	}

	functionName(paren) {
		const t = this.toks[paren - 1];
		if (!t) return null;
		if (t.kind === 'ident') {
			if (this.is(this.toks[paren - 2], 'punct', '~')) return '~' + t.text;
			return t.text;
		}
		// operator=, operator==, operator()...
		let k = paren - 1;
		let op = '';
		while (k >= 0 && this.toks[k].kind === 'punct') op = this.toks[k--].text + op;
		if (k >= 0 && this.is(this.toks[k], 'ident', 'operator')) return 'operator' + op;
		return null;
	}

	/** The index of a field's name in its tokens, or -1. */
	fieldName(toks) {
		let k = 0;
		// leading attributes, [[...]]
		while (this.is(toks[k], 'punct', '[') && this.is(toks[k + 1], 'punct', '[')) {
			let depth = 0;
			for (; k < toks.length; k++) {
				if (this.is(toks[k], 'punct', '[')) depth++;
				if (this.is(toks[k], 'punct', ']') && --depth === 0) break;
			}
			k++;
		}
		let angle = 0;
		for (; k < toks.length; k++) {
			const t = toks[k];
			if (this.is(t, 'punct', '<')) angle++;
			if (this.is(t, 'punct', '>')) angle--;
			if (angle === 0 && t.kind === 'punct' && ['=', '[', '{', ';', ':'].includes(t.text) && toks[k - 1]?.kind === 'ident') {
				return k - 1;
			}
		}
		return -1;
	}

	/** Source text from one token to another, its line breaks and indentation folded into single spaces. */
	text(from, to) {
		return this.src.slice(from.start, to.end).replace(/\s*\n\s*/g, ' ');
	}
}
