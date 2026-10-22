// The grammar, from src/frontend/Parser.y and Lexer.l: the summary at the top of Parser.y, every rule with its
// alternatives (without their C++ actions) and its comment, a railroad diagram of each, every token with its spelling
// and the patterns of Lexer.l that produce it, the scanner's rules in order, and the precedence declarations.
import { parseBison } from './bison.js';
import { parseFlex } from './flex.js';
import * as rr from './railroad.js';
import { GenerateError, githubLine, githubUrl, read, route } from './repo.js';
import { blockCommentLines, escapeHtml, lineCommentLines, plainText, renderComment } from './text.js';

export const PARSER = 'src/frontend/Parser.y';
export const LEXER = 'src/frontend/Lexer.l';

const ruleId = (name) => `rule-${name}`;
const tokenId = (name) => `token-${name}`;
const defId = (name) => `def-${name}`;
const precId = (level) => `prec-${level}`;

/** Comment tokens as lines, a blank line between those that aren't on consecutive lines of the source. */
function commentLines(tokens) {
	const out = [];
	for (const [k, t] of tokens.entries()) {
		if (k > 0 && t.line !== tokens[k - 1].line + 1 && t.body !== '' && tokens[k - 1].body !== '') out.push({ body: '', endCol: 0 });
		out.push(t);
	}
	return lineCommentLines(out);
}

export function generateGrammar({ highlightBlock }) {
	const y = parseBison(read(PARSER), PARSER);
	const l = parseFlex(read(LEXER), LEXER);
	const ruleByName = new Map(y.rules.map((r) => [r.name, r]));
	const warnings = [];

	// ------------------------------------------------------------------------------------------ symbols resolved
	const resolve = (item, rule) => {
		if (item.kind === 'symbol') {
			if (ruleByName.has(item.name)) return { kind: 'rule', name: item.name };
			const token = y.tokenByName.get(item.name);
			if (token && !token.precedenceOnly) return { kind: 'token', name: token.name, text: token.name };
			if (item.name === 'error') return { kind: 'error', name: 'error' };
			throw new GenerateError(`${PARSER}:${item.line}: rule ${rule.name} uses ${item.name}, which is neither a rule nor a %token`);
		}
		if (item.kind === 'literal') {
			const token = y.tokenByAlias.get(item.text);
			if (!token) throw new GenerateError(`${PARSER}:${item.line}: rule ${rule.name} uses ${item.text}, which no %token has as its alias`);
			return { kind: 'token', name: token.name, text: item.text };
		}
		if (item.kind === 'empty') return { kind: 'empty' };
		return { kind: 'mid', tag: item.tag ?? null };
	};
	for (const r of y.rules) for (const a of r.alternatives) a.resolved = a.items.map((it) => resolve(it, r));

	// who uses what
	const usedBy = new Map();
	const use = (key, rule) => {
		const set = usedBy.get(key) ?? new Set();
		set.add(rule);
		usedBy.set(key, set);
	};
	for (const r of y.rules) {
		for (const a of r.alternatives) {
			for (const s of a.resolved) if (s.kind === 'rule' || s.kind === 'token') use(`${s.kind}:${s.name}`, r.name);
			if (a.prec) use(`prec:${a.prec}`, r.name);
		}
	}

	function grammarLink(word, { code }) {
		if (code && ruleByName.has(word)) return `#${ruleId(word)}`;
		const token = y.tokenByName.get(word);
		if (token && /^[A-Z][A-Z0-9_]*$/.test(word)) return token.precedenceOnly ? `#${precId(levelOf.get(word))}` : `#${tokenId(word)}`;
		if (code && y.tokenByAlias.has(`"${word}"`)) return `#${tokenId(y.tokenByAlias.get(`"${word}"`).name)}`;
		return null;
	}

	// ------------------------------------------------------------------------------------------- the precedence
	const levelOf = new Map();
	for (const [i, p] of y.precedence.entries()) {
		for (const s of p.symbols) {
			const token = s.startsWith('"') || s.startsWith("'") ? y.tokenByAlias.get(s) : y.tokenByName.get(s);
			levelOf.set(token.name, i + 1);
		}
	}
	const precedence = y.precedence.map((p, i) => ({
		id: precId(i + 1),
		level: i + 1,
		assoc: p.assoc,
		symbols: p.symbols.map((s) => {
			const token = s.startsWith('"') || s.startsWith("'") ? y.tokenByAlias.get(s) : y.tokenByName.get(s);
			return {
				name: token.name,
				text: token.alias ?? token.name,
				precedenceOnly: !!token.precedenceOnly,
				usedBy: token.precedenceOnly ? [...(usedBy.get(`prec:${token.name}`) ?? [])] : [],
			};
		}),
		docHtml: p.doc.length ? renderComment(lineCommentLines(p.doc), { link: grammarLink }) : null,
		source: githubLine(PARSER, p.line),
	}));
	const precOfAlt = (a) => {
		if (!a.prec) return null;
		const token = a.prec.startsWith('"') ? y.tokenByAlias.get(a.prec) : y.tokenByName.get(a.prec);
		if (!token || !levelOf.has(token.name)) throw new GenerateError(`${PARSER}:${a.line}: %prec ${a.prec} has no precedence declaration`);
		return { name: token.name, text: a.prec, level: levelOf.get(token.name), id: precId(levelOf.get(token.name)) };
	};

	// ---------------------------------------------------------------------------------------- the scanner's rules
	const defNames = new Set(l.definitions.map((d) => d.name));
	const producedBy = new Map();
	const lexRules = l.rules.map((r, i) => {
		const id = `lex-${i + 1}`;
		if (r.outcome.kind === 'tokens') {
			for (const name of r.outcome.tokens) {
				if (name === 'YYEOF') continue;
				if (!y.tokenByName.has(name)) throw new GenerateError(`${LEXER}:${r.line}: the action returns ${name}, which Parser.y has no %token for`);
				const list = producedBy.get(name) ?? [];
				list.push({ id, pattern: r.pattern, conditions: r.conditions });
				producedBy.set(name, list);
			}
		}
		return {
			id,
			line: r.line,
			source: githubLine(LEXER, r.line),
			conditions: r.conditions,
			pattern: r.pattern,
			patternHtml: patternHtml(r.pattern, defNames),
			shared: !!r.shared,
			outcome: r.outcome,
		};
	});

	// ------------------------------------------------------------------------------------------------ the tokens
	const tokens = y.tokens.filter((t) => !t.precedenceOnly).map((t) => {
		const patterns = producedBy.get(t.name) ?? [];
		if (!patterns.length) warnings.push(`${PARSER}:${t.line}: %token ${t.name} is never returned by ${LEXER}`);
		return {
			id: tokenId(t.name),
			name: t.name,
			alias: t.alias,
			type: t.type,
			patterns: patterns.map((p) => ({ ...p, patternHtml: patternHtml(p.pattern, defNames) })),
			usedBy: [...(usedBy.get(`token:${t.name}`) ?? [])],
			level: levelOf.get(t.name) ?? null,
			source: githubLine(PARSER, t.line),
		};
	});

	// ------------------------------------------------------------------------------------------------ the rules
	const rules = y.rules.map((r) => {
		const alternatives = r.alternatives.map((a) => ({
			items: a.resolved.map((s) => (s.kind === 'mid' ? { kind: 'mid', tag: s.tag } : s)),
			prec: precOfAlt(a),
			// its comments: above it, or after it on its line; and those on their own lines in its action
			docHtml: a.notes.length ? renderComment(commentLines(a.notes), { link: grammarLink }) : null,
			actionHtml: a.actionNotes.length ? renderComment(commentLines(a.actionNotes), { link: grammarLink }) : null,
		}));
		const docTokens = [...r.doc, ...(r.doc.length && r.after.length ? [{ body: '', endCol: 0, line: -1 }] : []), ...r.after];
		const loop = loopOf(r.name, alternatives);
		return {
			id: ruleId(r.name),
			name: r.name,
			type: y.nterms.get(r.name) ?? null,
			line: r.line,
			source: githubLine(PARSER, r.line),
			docHtml: docTokens.length ? renderComment(commentLines(docTokens), { link: grammarLink }) : null,
			doc: plainText(commentLines(docTokens)),
			alternatives,
			usedBy: [...(usedBy.get(`rule:${r.name}`) ?? [])],
			diagram: rr.diagram(railroadOf(alternatives, loop), `Railroad diagram of ${r.name}`),
			loop: loop?.kind ?? null,
		};
	});
	for (const r of rules.slice(1)) if (r.usedBy.length === 0) warnings.push(`${PARSER}:${r.line}: rule ${r.name} is used by no other rule`);

	// ------------------------------------------------------------------------------------- the opening comments
	const { before, ebnf, after } = splitSummary(lineCommentLines(y.header));
	const summaryHtml = renderComment(before, { link: grammarLink }) + (ebnf.length ? ebnfHtml(ebnf, ruleByName, y) : '') +
		renderComment(after, { link: grammarLink });
	// in the scanner's comment, {IDENTIFIER} is the definition, not the token
	const lexerLink = (word, how) => (defNames.has(word) ? `#${defId(word)}` : grammarLink(word, how));
	const lexerHtml = l.header ? renderComment(blockCommentLines(l.header.text, l.header.col), { link: lexerLink }) : '';

	// the declarations as Parser.y writes them, comments included, without the C++ of its %code blocks
	const declarationsHtml = highlightBlock(y.prologue, 'yacc');
	const definitions = l.definitions.map((d) => ({
		id: defId(d.name),
		name: d.name,
		pattern: d.pattern,
		patternHtml: patternHtml(d.pattern, defNames),
		source: githubLine(LEXER, d.line),
	}));

	const page = route('/grammar/');
	const search = [
		...rules.map((r) => ({ t: r.name, s: 'Grammar · rule', u: `${page}#${r.id}` })),
		...tokens.map((t) => ({ t: t.alias ? `${t.name} ${t.alias}` : t.name, s: 'Grammar · token', u: `${page}#${t.id}` })),
		...definitions.map((d) => ({ t: `{${d.name}}`, s: 'Grammar · scanner definition', u: `${page}#${d.id}` })),
	];

	return {
		grammar: {
			parser: { file: PARSER, url: githubUrl(PARSER) },
			lexer: { file: LEXER, url: githubUrl(LEXER), options: l.options, conditions: l.conditions },
			summary: capitalise(plainText(before).replace(/^src\/frontend\/Parser\.y:\s*/, '').replace(/\s*\(Bison[^)]*\)/, '')),
			summaryHtml,
			lexerHtml,
			declarationsHtml,
			rules,
			tokens,
			precedence,
			definitions,
			lexRules,
		},
		search,
		warnings,
	};
}

// --------------------------------------------------------------------------------------------- railroad diagrams

const same = (a, b) => a.length === b.length && a.every((x, i) => JSON.stringify(x) === JSON.stringify(b[i]));
const isEmpty = (a) => a.length === 0 || (a.length === 1 && a[0].kind === 'empty');

/** A left-recursive list, drawn as a loop: `x: %empty | x a | x b` is (a | b)*, and `x: a | x sep a` is a (sep a)*. */
function loopOf(name, alternatives) {
	if (alternatives.some((a) => a.prec)) return null;
	const alts = alternatives.map((a) => a.items.filter((s) => s.kind !== 'mid'));
	if (alts.length !== alternatives.length || alternatives.some((a) => a.items.some((s) => s.kind === 'mid'))) return null;
	const self = (s) => s.kind === 'rule' && s.name === name;
	const recursive = alts.filter((a) => a.length > 1 && self(a[0]) && !a.slice(1).some(self));
	const empties = alts.filter(isEmpty);
	const others = alts.filter((a) => !isEmpty(a) && !recursive.includes(a));
	if (recursive.length && empties.length === 1 && others.length === 0 && recursive.length + 1 === alts.length) {
		return { kind: 'zeroOrMore', items: recursive.map((a) => a.slice(1)) };
	}
	if (recursive.length === 1 && empties.length === 0 && others.length === 1 && alts.length === 2) {
		const base = others[0];
		const tail = recursive[0].slice(1);
		if (tail.length >= base.length && same(tail.slice(tail.length - base.length), base) && !base.some(self)) {
			return { kind: 'oneOrMore', item: base, separator: tail.slice(0, tail.length - base.length) };
		}
	}
	return null;
}

function railroadOf(alternatives, loop) {
	const node = (s) => {
		if (s.kind === 'rule') return rr.nonterminal(s.name, `#${ruleId(s.name)}`);
		if (s.kind === 'token') return rr.terminal(s.text, `#${tokenId(s.name)}`);
		if (s.kind === 'mid') return rr.marker('a mid-rule action (C++), left out');
		return rr.terminal(s.name ?? '?');
	};
	const seq = (items) => {
		const parts = items.filter((s) => s.kind !== 'empty').map(node);
		return parts.length ? rr.sequence(parts) : rr.skip();
	};
	if (loop?.kind === 'zeroOrMore') return rr.zeroOrMore(rr.choice(loop.items.map(seq)));
	if (loop?.kind === 'oneOrMore') return rr.oneOrMore(seq(loop.item), loop.separator.length ? seq(loop.separator) : undefined);
	const alts = alternatives.map((a) => a.items);
	const emptyAt = alts.findIndex(isEmpty);
	if (emptyAt >= 0 && alts.length > 1) {
		// something optional: the empty way above the others
		return rr.optional(rr.choice(alts.filter((_, i) => i !== emptyAt).map(seq)));
	}
	return rr.choice(alts.map(seq));
}

// ------------------------------------------------------------------------------------------- the EBNF summary

/** The header comment around its indented block, which is the grammar's EBNF summary. */
function splitSummary(lines) {
	const indented = (l) => /^ {2,}\S/.test(l.text);
	const start = lines.findIndex(indented);
	if (start < 0) return { before: lines, ebnf: [], after: [] };
	let end = start;
	while (end < lines.length && lines[end].text.trim() !== '' && indented(lines[end])) end++;
	return { before: lines.slice(0, start), ebnf: lines.slice(start, end), after: lines.slice(end) };
}

/** The summary as written, its symbols linked: a rule's name to the rule, a token's to the token. */
function ebnfHtml(lines, ruleByName, y) {
	const indent = Math.min(...lines.map((l) => l.text.match(/^ */)[0].length));
	const summaryNames = new Set();
	for (const l of lines) {
		const m = l.text.slice(indent).match(/^([A-Za-z_]\w*)\s+:/);
		if (m) summaryNames.add(m[1]);
	}
	const out = lines.map((l) => {
		const text = l.text.slice(indent);
		const lhs = text.match(/^([A-Za-z_]\w*)(\s+:\s)/);
		const bar = text.match(/^(\s+\|\s)/);
		let head;
		let rest;
		if (lhs) {
			const name = lhs[1];
			const nameHtml = ruleByName.has(name)
				? `<a class="ebnf-lhs" href="#${ruleId(name)}">${name}</a>`
				: `<span class="ebnf-lhs">${name}</span>`;
			head = `<span class="ebnf-anchor" id="ebnf-${name}"></span>${nameHtml}${escapeHtml(lhs[2])}`;
			rest = text.slice(lhs[0].length);
		} else if (bar) {
			head = escapeHtml(bar[1]);
			rest = text.slice(bar[0].length);
		} else {
			return `<span class="ebnf-note">${escapeHtml(text)}</span>`;
		}
		const split = rest.search(/ {3,}\S/);
		const body = split >= 0 ? rest.slice(0, split) : rest;
		const note = split >= 0 ? rest.slice(split) : '';
		const bodyHtml = body.replace(/"(?:[^"\\]|\\.)*"|[A-Za-z_]\w*|[^"A-Za-z_]+/g, (tok) => {
			if (tok.startsWith('"')) {
				const token = y.tokenByAlias.get(tok);
				return token
					? `<a class="ebnf-terminal" href="#${tokenId(token.name)}">${escapeHtml(tok)}</a>`
					: `<span class="ebnf-terminal">${escapeHtml(tok)}</span>`;
			}
			if (/^[A-Za-z_]/.test(tok)) {
				if (ruleByName.has(tok)) return `<a class="ebnf-rule" href="#${ruleId(tok)}">${tok}</a>`;
				const token = y.tokenByName.get(tok);
				if (token && !token.precedenceOnly) return `<a class="ebnf-token" href="#${tokenId(tok)}">${tok}</a>`;
				if (summaryNames.has(tok)) return `<a class="ebnf-rule" href="#ebnf-${tok}">${tok}</a>`;
				return escapeHtml(tok);
			}
			return `<span class="ebnf-op">${escapeHtml(tok)}</span>`;
		});
		return head + bodyHtml + (note ? `<span class="ebnf-note">${escapeHtml(note)}</span>` : '');
	});
	return `<pre class="ebnf">${out.join('\n')}</pre>`;
}

/** A Flex pattern, its {DEFINITIONS} linked to their definitions. */
function patternHtml(pattern, defNames) {
	return escapeHtml(pattern).replace(/\{([A-Za-z_][\w-]*)\}/g, (all, name) => (defNames.has(name) ? `<a href="#${defId(name)}">${all}</a>` : all));
}

function capitalise(text) {
	return text.charAt(0).toUpperCase() + text.slice(1);
}
