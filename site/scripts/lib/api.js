// The public API, from include/fminus/FMinus.h: every public declaration in namespace fminus, the comment that
// documents it (verbatim: the header's comments are the documentation), its signature and its place in the nesting,
// and the header's opening comment as the introduction.
//
// Every public declaration needs a comment, and the build stops on one that has none. A declaration's comment is the
// one above it or the one after it on its line. A declaration without one is still documented
//   - by the declaration above it, when it overloads it with no blank line between (compile, compile);
//   - by the field above it, on the line before, when that field's comment after it runs on into this one's (the
//     first ends in ';' or ',': minimum and maximum), or when this one has no comment, is of the same type and is
//     named in parallel (streamOffset, valueOffset): the two show as one entry's comment;
//   - by the comment that documents its type or namespace, when that comment names it: as a `code` span, as the term
//     of an indented list of terms (an enum's "  Knob  the slot's value..."), or as a word when the name is no
//     English word (valueOffset, FMINUS_MATH_CONST); a macro defined in an #if block, by the comment above the block;
//   - by its type or namespace without being named, only when COVERED below lists it: what the header leaves to its
//     type's comment today. A new member without a comment of its own stops the build;
//   - by its members, when it is a type and every one of them has a comment of its own (Options);
//   - by the header's opening comment, when it is the first declaration in namespace fminus (Program);
//   - by being what it is: a destructor, or a deleted copy.
// Program's own members are never documented by Program, since the header's introduction is not about each of them.
import { parseHeader } from './cxx-header.js';
import { GenerateError, githubLine, githubUrl, read, route } from './repo.js';
import { escapeHtml, lineCommentLines, plainText, renderComment } from './text.js';

export const HEADER = 'include/fminus/FMinus.h';

const TYPE_KINDS = new Set(['class', 'struct', 'union', 'enum']);
const SCOPE_KINDS = new Set(['class', 'struct', 'union', 'enum', 'namespace']);

/**
 * The members FMinus.h documents with their type's or namespace's comment without naming them (the header as it is):
 * Binding's comment describes its kinds, which knob(), stream() and frozen() make; Module's describes a module and
 * its instances, and Field's members with it; fminus::math's names its functions as C does (sinf, ...). Each entry
 * is a type or namespace, without fminus::, and the members its comment covers. A member that gets a comment of its
 * own can leave the list (the build warns of it).
 */
const COVERED = new Map([
	['Program::Binding', ['Kind', 'kind', 'knob', 'stream', 'frozen']],
	['Program::Module', ['name', 'Field']],
	['Program::Module::Field', ['name']],
	['math', ['sin', 'cos', 'exp', 'tanh', 'log']],
]);

export function generateApi({ highlightBlock, highlightInline }) {
	const src = read(HEADER);
	const { intro, root, includes, floating } = parseHeader(src, HEADER);
	const problems = [];
	const warnings = [];
	const problem = (line, message) => problems.push(`${HEADER}:${line}: ${message}`);

	const fminus = root.children.find((c) => c.kind === 'namespace' && c.name === 'fminus');
	if (!fminus) throw new GenerateError(`${HEADER}: no namespace fminus: the API reader has nothing to document`);
	if (!intro.length) problem(1, 'the header has no opening comment, which is the API page\'s introduction');

	for (const f of floating) {
		if (f.access !== 'public' || !within(f.scope, fminus)) continue;
		problem(f.comments[0].line, 'a comment that documents nothing: a blank line, or a preprocessor line, separates it from ' +
			'the declaration after it. Remove the blank line, or move the comment into the comment above or below.');
	}

	// ------------------------------------------------------------------------------------------- which comment
	const isPublic = (n) => n.access === 'public' && n.kind !== 'forward' && n.kind !== 'include';
	const commentLines = (tokens) =>
		tokens.length ? lineCommentLines(tokens.filter((t) => t.style === 'line')).concat(blockLines(tokens)) : [];
	const ownLines = (n) => {
		if (n.sharedWith) return [];
		return commentLines([...(n.doc ?? []), ...(n.closing ? [] : n.trailing ?? []), ...(n.continued ?? [])]);
	};
	const covered = new Set();
	for (const [scope, names] of COVERED) for (const name of names) covered.add(`${scope}::${name}`);
	const coveredUsed = new Set();

	/** Adjacent fields that share a comment: one's comment after it runs on into the next's, or the next has none. */
	function shareComments(scope) {
		const list = scope.children;
		for (let k = 1; k < list.length; k++) {
			const a = list[k - 1];
			const b = list[k];
			if (a.kind !== 'field' || b.kind !== 'field' || !isPublic(a) || !isPublic(b)) continue;
			if (b.line !== a.endLine + 1 || b.doc.length) continue;
			const head = a.sharedWith ?? a;
			const last = a.trailing[a.trailing.length - 1];
			if (b.trailing.length) {
				// a sentence that goes on: the first ends in ';' or ',', and the next goes on at its column
				if (last && /[;,]\s*$/.test(last.body) && b.trailing[0].col === last.col) {
					b.sharedWith = head;
					head.continued = [...(head.continued ?? []), ...b.trailing];
				}
			} else if ((a.trailing.length || a.sharedWith) && a.typeText === b.typeText && parallel(a.name, b.name)) {
				b.sharedWith = head;
			}
		}
		for (const c of list) if (SCOPE_KINDS.has(c.kind) && c.children) shareComments(c);
	}
	shareComments(fminus);

	/**
	 * Gives each public declaration of a scope its documentation; the problems of those that have none. `cover` is
	 * the comment that documents the scope ({ owner, lines }), for the members it names, or null.
	 */
	function resolveScope(scope, cover) {
		const found = [];
		const members = scope.children.filter(isPublic);
		let previous = null;
		for (const node of members) {
			resolveNode(node, scope, cover, previous, members[0] === node, found);
			previous = node;
		}
		return found;
	}

	function coveredBy(node, scope, cover) {
		if (!cover) return false;
		const id = `${label(scope)}::${node.name}`;
		if (covered.has(id)) {
			coveredUsed.add(id);
			return true;
		}
		return names(cover.lines, node.name);
	}

	function resolveNode(node, scope, cover, previous, isFirst, found) {
		const own = ownLines(node);
		if (own.length) {
			node.docFrom = 'own';
			node.docLines = own;
		} else if (node.kind === 'function' && previous?.kind === 'function' && previous.name === node.name &&
			node.line - previous.endLine <= 1 && previous.docFrom) {
			node.docFrom = 'overload';
			node.overloadOf = previous.overloadOf ?? previous;
		} else if (node.sharedWith) {
			node.docFrom = 'shared';
		} else if (node.special === 'destructor' || node.deleted) {
			node.docFrom = 'special';
		} else if (coveredBy(node, scope, cover)) {
			node.docFrom = 'parent';
			node.docOwner = cover.owner;
		} else if (scope === fminus && isFirst && intro.length) {
			node.docFrom = 'file';
		} else if (node.kind === 'macro' && node.blockOwner && names(ownLines(node.blockOwner), node.name)) {
			node.docFrom = 'parent';
			node.docOwner = node.blockOwner;
		} else if (TYPE_KINDS.has(node.kind) && node.kind !== 'enum') {
			// documented by its members, if every one has a comment of its own
			const inner = resolveScope(node, null);
			if (inner.length === 0 && node.children.some(isPublic)) {
				node.docFrom = 'members';
				return;
			}
			found.push({ node, inner, cover });
		} else {
			found.push({ node, inner: [], cover: node.kind === 'macro' && node.blockOwner ? { owner: node.blockOwner, block: true } : cover });
		}
		if (!node.docFrom) return;
		// What documents its members: its own comment, or the comment that documents it. Program's members are
		// documented one by one.
		let inner = null;
		if (node.docFrom === 'own') inner = { owner: node, lines: node.docLines };
		else if (node.docFrom === 'parent' && cover && node.docOwner === cover.owner) inner = cover;
		if (node.kind === 'enum') {
			for (const e of node.enumerators) {
				if (e.doc.length || e.trailing.length || (inner && names(inner.lines, e.name))) continue;
				found.push({ node: { kind: 'enumerator', name: e.name, line: e.line, qualified: [...node.qualified, e.name] }, inner: [], cover: inner });
			}
		}
		if (SCOPE_KINDS.has(node.kind)) found.push(...resolveScope(node, node.docFrom === 'file' ? null : inner));
	}

	for (const { node, inner, cover } of resolveScope(fminus, null)) {
		const what = `${kindName(node)} ${qualified(node)}`;
		const detail = inner.length ? ` (and not every member has one: ${inner.map(({ node: m }) => m.name).join(', ')})` : '';
		let parent = '';
		if (cover?.block) parent = `, and the comment above its #if block (${label(cover.owner)}'s) does not name it`;
		else if (cover) parent = `, and ${label(cover.owner)}'s comment does not name it`;
		problem(node.line, `the public ${what} has no comment${detail}${parent}. FMinus.h's comments are its ` +
			'documentation: write one above the declaration, or after it on its line.');
	}
	if (problems.length) throw new GenerateError(`the public API's documentation is incomplete:\n  ${problems.join('\n  ')}`);
	for (const id of covered) {
		if (!coveredUsed.has(id)) warnings.push(`${HEADER}: api.js's COVERED lists ${id}, which is no member without a comment of its own: take it off the list`);
	}

	// ------------------------------------------------------------------------------------------------ the model
	const byId = new Map();
	const all = [];
	const idOf = (n) => qualified(n).replace(/^fminus::/, '');
	(function index(scope) {
		for (const n of scope.children.filter(isPublic)) {
			if (n.docFrom === 'overload') continue;
			n.id = idOf(n);
			if (byId.has(n.id) && byId.get(n.id) !== n) {
				throw new GenerateError(`${HEADER}:${n.line}: ${n.id} is declared twice, apart (the API page gives each name one entry)`);
			}
			byId.set(n.id, n);
			all.push(n);
			for (const e of n.enumerators ?? []) {
				e.id = `${n.id}::${e.name}`;
				e.kind = 'enumerator';
				e.parent = n;
				byId.set(e.id, e);
				all.push(e);
			}
			if (SCOPE_KINDS.has(n.kind)) index(n);
		}
	})(fminus);

	const byName = new Map();
	for (const n of all) {
		const list = byName.get(n.name) ?? [];
		list.push(n);
		byName.set(n.name, list);
	}

	/** Links from a comment to the declarations it names, each once: qualified names, calls (name()), camelCase
	 *  names and type names in the text, and any name that is a whole `code` span. */
	function linker(current) {
		const scopes = [];
		for (let s = current; s; s = s.parent) if (s.id !== undefined || s === fminus) scopes.push(s === fminus ? '' : s.id);
		// the introduction is about Program
		if (!current) scopes.push('Program', '');
		const linked = new Set();
		return (word, { code }) => {
			let name = word.replace(/\(\)$/, '');
			const call = name !== word;
			name = name.replace(/^fminus::/, '');
			let target = null;
			for (const s of scopes) {
				target = byId.get(s ? `${s}::${name}` : name);
				if (target?.special) target = null; // Program names the class, not its constructor
				if (target) break;
			}
			if (!target && !name.includes('::')) {
				let list = byName.get(name) ?? [];
				if (call) list = list.filter((n) => n.kind === 'function');
				if (list.length === 1) target = list[0];
			}
			if (!target && name.includes('::')) {
				const list = all.filter((n) => n.id.endsWith(`::${name}`));
				if (list.length === 1) target = list[0];
			}
			if (!target || target === current || target === current?.overloadOf) return null;
			const isType = TYPE_KINDS.has(target.kind) || target.kind === 'namespace';
			const wanted = code || name.includes('::') || (call && target.kind === 'function') ||
				(/^[a-z]+[A-Z]/.test(name) && target.kind !== 'enumerator') || (isType && /^[A-Z]/.test(name));
			if (!wanted || linked.has(target.id)) return null;
			linked.add(target.id);
			return `#${target.id}`;
		};
	}

	const docHtml = (lines, current) =>
		renderComment(lines, { link: linker(current), highlight: highlightBlock });

	function signatures(n) {
		const overloads = n.parent.children.filter((s) => s.overloadOf === n);
		return [n, ...overloads].map((d) => ({
			code: d.signature,
			html: highlightInline(d.signature),
			source: githubLine(HEADER, d.line),
			line: d.line,
		}));
	}

	function codeOf(n) {
		if (n.kind === 'enum') return dedent(src.slice(lineStart(src, n.line), lineEnd(src, n.endLine)));
		if (n.definition) return dedent(indentTo(n, n.definition));
		if (n.kind === 'macro') return n.definitions.map((d) => d.condition ? `${d.condition}\n${d.text}` : d.text).join('\n') + (n.definitions.some((d) => d.condition) ? '\n#endif' : '');
		return null;
	}

	function indentTo(n, text) {
		// the definition's first line starts at the declaration; the rest keep the source's indentation
		const col = src.slice(lineStart(src, n.line)).match(/^ */)[0].length;
		return ' '.repeat(col) + text;
	}

	const note = (n) => {
		switch (n.docFrom) {
			case 'file':
				return { text: 'Documented by the header\'s introduction', href: '#introduction' };
			case 'parent':
				return { text: `Documented with ${label(n.docOwner)}`, href: `#${n.docOwner.id}` };
			case 'shared':
				return { text: `Documented with ${n.sharedWith.name}, the field above`, href: `#${n.sharedWith.id}` };
			case 'members':
				return { text: 'Documented by its members' };
			case 'special':
				return { text: n.special === 'destructor' ? 'The destructor' : 'Deleted: a Program is not copied' };
			default:
				return null;
		}
	};

	function model(n) {
		const code = codeOf(n);
		const entry = {
			id: n.id,
			kind: n.kind,
			kindLabel: kindName(n),
			name: n.name,
			label: label(n),
			qualified: qualified(n),
			isStatic: !!n.isStatic,
			special: n.special ?? null,
			source: githubLine(HEADER, n.line),
			signatures: n.kind === 'macro' || n.kind === 'enum' || n.kind === 'namespace' ? [] : signatures(n),
			codeHtml: code ? highlightBlock(code, 'cpp') : null,
			docHtml: n.docFrom === 'own' ? docHtml(n.docLines, n) : null,
			note: note(n),
			// the field whose comment this one shares: the page shows them as one entry's
			sharedWith: n.sharedWith?.id ?? null,
			enumerators: (n.enumerators ?? []).map((e) => ({
				id: e.id,
				name: e.name,
				value: e.value,
				docHtml: [...e.doc, ...e.trailing].length ? docHtml(lineCommentLines([...e.doc, ...e.trailing]), e) : null,
			})),
			children: SCOPE_KINDS.has(n.kind) ? n.children.filter((c) => isPublic(c) && c.docFrom !== 'overload').map(model) : [],
		};
		return entry;
	}

	const entries = fminus.children.filter((c) => isPublic(c) && c.docFrom !== 'overload').map(model);

	// The sidebar: every type and function, nested as in the header.
	const sidebar = [];
	(function walk(list, depth) {
		for (const e of list) {
			if (e.kind === 'field' || e.kind === 'alias') continue;
			sidebar.push({ id: e.id, label: e.kind === 'function' ? `${e.name}()` : e.name, kind: e.kind, depth });
			walk(e.children, depth + 1);
		}
	})(entries, 0);

	const introLines = lineCommentLines(intro.filter((t) => t.style === 'line')).concat(blockLines(intro));
	const introHtml = renderComment(introLines, { link: linker(null), highlight: highlightBlock });
	const includeBlock = includes.length
		? highlightBlock(includes.map((r) => r.text + (r.trailing.length ? '   //' + r.trailing.map((t) => t.body).join(' ') : '')).join('\n'), 'cpp')
		: null;

	const search = all.map((n) => ({
		t: n.id + (n.kind === 'function' ? '()' : ''),
		s: `API · ${kindName(n)}`,
		u: `${route('/api/')}#${n.id}`,
	}));

	const declarations = all.map((n) => ({ id: n.id, kind: n.kind, line: n.line }));
	const firstParagraph = introLines.slice(0, Math.max(0, introLines.findIndex((l) => l.text.trim() === '')) || introLines.length);
	const summary = sentences(plainText(firstParagraph).replace(/^include\/fminus\/FMinus\.h:\s*/, ''), 2);

	return {
		warnings,
		api: {
			file: HEADER,
			fileUrl: githubUrl(HEADER),
			summary,
			introHtml,
			includeHtml: includeBlock,
			entries,
			sidebar,
			declarations,
		},
		search,
	};
}

function sentences(text, count) {
	const parts = text.match(/[^.]+(?:\.(?=\s|$)|$)/g) ?? [text];
	return parts.slice(0, count).join('').trim();
}

function blockLines(tokens) {
	return tokens.filter((t) => t.style === 'block').flatMap((t) => t.body.split('\n').map((l) => ({ text: l.replace(/^\s*\* ?/, ''), end: 0 })));
}

function within(scope, ancestor) {
	for (let s = scope; s; s = s.parent) if (s === ancestor) return true;
	return false;
}

function qualified(n) {
	if (n.kind === 'macro') return n.name;
	return (n.qualified ?? [n.name]).join('::');
}

function label(n) {
	return qualified(n).replace(/^fminus::/, '');
}

function kindName(n) {
	if (n.kind === 'function') {
		if (n.special === 'destructor') return 'destructor';
		if (n.special === 'constructor') return 'constructor';
		if (n.special === 'operator') return 'operator';
		return n.isStatic ? 'static function' : 'function';
	}
	if (n.kind === 'enum') return n.scoped ? 'enum class' : 'enum';
	return n.kind;
}

function lineStart(src, line) {
	let offset = 0;
	for (let l = 1; l < line; l++) offset = src.indexOf('\n', offset) + 1;
	return offset;
}

function lineEnd(src, line) {
	const start = lineStart(src, line);
	const end = src.indexOf('\n', start);
	return end < 0 ? src.length : end;
}

/** Text without the indentation every line shares. */
export function dedent(text) {
	const lines = text.replace(/\s+$/, '').split('\n');
	const indent = Math.min(...lines.filter((l) => l.trim()).map((l) => l.match(/^ */)[0].length));
	return lines.map((l) => l.slice(indent)).join('\n');
}

/**
 * Whether a comment names `name`: as a `code` span (`size`, `knob()`, `Binding::knob`), as the term of an indented
 * list of terms ("  Knob    the slot's value..."), or as a whole word when the name can't be an English word: it has
 * a capital after its first letter, a digit or an underscore (valueOffset, FMINUS_MATH_CONST).
 */
export function names(lines, name) {
	const word = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
	const term = new RegExp(`^ {2,}${word}(?: {2,}\\S|\\s*$)`);
	const span = new RegExp('`(?:[\\w:~]*::)?' + word + '(?:\\(\\))?`');
	const plain = /^.+[A-Z0-9_]/.test(name) || /_/.test(name) ? new RegExp(`(?<!\\w)${word}(?!\\w)`) : null;
	return lines.some((l) => term.test(l.text) || span.test(l.text) || (plain && plain.test(l.text)));
}

/** Two names in parallel: the same last word, after a different first (streamOffset and valueOffset). */
function parallel(a, b) {
	const last = (n) => n.match(/[a-z0-9]([A-Z][a-z0-9]*)$/)?.[1];
	return a !== b && !!last(a) && last(a) === last(b);
}

export { escapeHtml };
