// A reader for Flex scanners as src/frontend/Lexer.l writes them: the opening comment, the options, the start
// conditions, the named definitions (DIGITS, IDENTIFIER, ...), and every rule in order, with its start conditions, its
// pattern verbatim, and what its action does: the tokens it returns (Parser::make_X), an error it throws, a start
// condition it enters, or nothing (the input is skipped).
import { GenerateError } from './repo.js';
import { skipBraces } from './bison.js';

export function parseFlex(src, file) {
	const starts = [0];
	for (let i = 0; i < src.length; i++) if (src[i] === '\n') starts.push(i + 1);
	const lineOf = (offset) => {
		let line = 1;
		while (line < starts.length && starts[line] <= offset) line++;
		return line;
	};
	const fail = (offset, message) => {
		throw new GenerateError(`${file}:${lineOf(offset)}: ${message}`);
	};
	const seps = [...src.matchAll(/^%%[ \t]*$/gm)].map((m) => m.index);
	if (seps.length < 1) throw new GenerateError(`${file}: no %% line: not a Flex scanner`);

	// ------------------------------------------------------------------------------------------------ definitions
	let header = null;
	const options = [];
	const conditions = [];
	const definitions = [];
	let i = 0;
	const defsEnd = seps[0];
	while (i < defsEnd) {
		const eol = src.indexOf('\n', i) < 0 ? src.length : src.indexOf('\n', i);
		const line = src.slice(i, eol);
		if (line.trim() === '') {
			i = eol + 1;
			continue;
		}
		if (/^\s*\/\*/.test(line)) {
			const open = src.indexOf('/*', i);
			const close = src.indexOf('*/', open + 2);
			if (close < 0) fail(i, 'unterminated /* comment');
			if (!header && definitions.length === 0 && options.length === 0) {
				header = { text: src.slice(open, close + 2), line: lineOf(open), col: open - i + 1 };
			}
			i = src.indexOf('\n', close) + 1 || src.length;
			continue;
		}
		if (/^%top\s*\{/.test(line)) {
			i = skipBraces(src, src.indexOf('{', i), fail);
			continue;
		}
		if (/^%\{/.test(line)) {
			const close = src.indexOf('\n%}', i);
			if (close < 0) fail(i, 'a %{ with no %}');
			i = src.indexOf('\n', close + 1) + 1 || src.length;
			continue;
		}
		let m;
		if ((m = /^%option\s+(.*)$/.exec(line))) options.push(...m[1].trim().split(/\s+/));
		else if ((m = /^%([xs])\s+(.*)$/.exec(line))) {
			for (const name of m[2].trim().split(/\s+/)) conditions.push({ name, exclusive: m[1] === 'x', line: lineOf(i) });
		} else if ((m = /^([A-Za-z_][\w-]*)\s+(\S.*?)\s*$/.exec(line))) definitions.push({ name: m[1], pattern: m[2], line: lineOf(i) });
		else if (!/^\s/.test(line)) fail(i, `a line of the definitions I can't read: ${line}`);
		i = eol + 1;
	}

	// ------------------------------------------------------------------------------------------------- rules
	const rules = [];
	const end = seps.length > 1 ? seps[1] : src.length;
	i = seps[0] + 2;
	while (i < end) {
		const c = src[i];
		if (c === '\n') {
			i++;
			continue;
		}
		if (c === '%' && src[i + 1] === '{') {
			const close = src.indexOf('%}', i);
			if (close < 0) fail(i, 'a %{ with no %}');
			i = close + 2;
			continue;
		}
		if (c === ' ' || c === '\t') {
			// indented text before a rule is code Flex copies, or a comment
			const eol = src.indexOf('\n', i);
			const rest = src.slice(i, eol < 0 ? end : eol).trim();
			if (rest.startsWith('/*')) {
				const close = src.indexOf('*/', i);
				i = close + 2;
			} else i = eol < 0 ? end : eol + 1;
			continue;
		}
		const ruleStart = i;
		let conds = [];
		if (src[i] === '<' && !src.startsWith('<<EOF>>', i)) {
			const close = src.indexOf('>', i);
			conds = src.slice(i + 1, close).split(',').map((s) => s.trim());
			i = close + 1;
		}
		const patternStart = i;
		i = scanPattern(src, i, fail);
		const pattern = src.slice(patternStart, i);
		while (src[i] === ' ' || src[i] === '\t') i++;
		let action;
		if (src[i] === '|') {
			action = '|';
			i++;
		} else if (src[i] === '{') {
			const close = skipBraces(src, i, fail);
			action = src.slice(i, close);
			i = close;
		} else {
			const eol = src.indexOf('\n', i);
			action = src.slice(i, eol < 0 ? end : eol);
			i = eol < 0 ? end : eol;
		}
		if (pattern === '') fail(ruleStart, 'a rule with no pattern');
		rules.push({ conditions: conds, pattern, action, line: lineOf(ruleStart) });
		const eol = src.indexOf('\n', i);
		const rest = src.slice(i, eol < 0 ? end : eol).trim();
		if (rest && !rest.startsWith('//') && !rest.startsWith('/*')) fail(i, `text after a rule's action: ${rest}`);
		i = eol < 0 ? end : eol + 1;
	}

	// A rule whose action is | shares the next rule's.
	for (let r = rules.length - 1; r >= 0; r--) {
		if (rules[r].action === '|') {
			if (r + 1 >= rules.length) fail(starts[rules[r].line - 1], "a '|' action on the last rule");
			rules[r].shared = true;
			rules[r].action = rules[r + 1].action;
		}
	}
	for (const r of rules) r.outcome = outcome(r.action);
	return { header, options, conditions, definitions, rules };
}

/** The end of a pattern: the first whitespace outside quotes, a character class and braces. */
function scanPattern(src, i, fail) {
	const start = i;
	while (i < src.length) {
		const c = src[i];
		if (c === ' ' || c === '\t' || c === '\n') break;
		if (c === '\\') {
			i += 2;
		} else if (c === '"') {
			i++;
			while (i < src.length && src[i] !== '"') {
				if (src[i] === '\\') i++;
				if (src[i] === '\n') fail(start, 'a quoted string in a pattern that runs past its line');
				i++;
			}
			i++;
		} else if (c === '[') {
			i++;
			if (src[i] === '^') i++;
			if (src[i] === ']') i++;
			while (i < src.length && src[i] !== ']') {
				if (src[i] === '\\') i++;
				if (src[i] === '\n') fail(start, 'a character class that runs past its line');
				i++;
			}
			i++;
		} else if (c === '{') {
			const close = src.indexOf('}', i);
			if (close < 0 || src.slice(i, close).includes('\n')) fail(start, "a '{' in a pattern with no '}' on its line");
			i = close + 1;
		} else if (src.startsWith('<<EOF>>', i)) {
			i += 7;
		} else {
			i++;
		}
	}
	return i;
}

/** What an action does, as the docs show it. */
function outcome(action) {
	const tokens = [...action.matchAll(/\bmake_([A-Za-z_][A-Za-z0-9_]*)\s*\(/g)].map((m) => m[1]);
	const begin = action.match(/\bBEGIN\s*\(?\s*([A-Za-z_]\w*)/);
	const thrown = action.match(/\bsyntax_error\s*\(([\s\S]*)\)\s*;/);
	if (tokens.length) return { kind: 'tokens', tokens };
	if (thrown) {
		const literals = [...thrown[1].matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((m) => m[1].replace(/\\(.)/g, '$1'));
		return { kind: 'error', message: literals.join('…') };
	}
	if (begin) return { kind: 'begin', condition: begin[1] };
	return { kind: 'skip' };
}
