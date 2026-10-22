// Syntax highlighting at build time, with Shiki: C, C++, LLVM IR, CMake, bash, assembly, diffs, Bison and Flex, and
// FMinus itself through the VSCode extension's grammar (editors/vscode/syntaxes/fminus.tmLanguage.json), so the site
// colours FMinus exactly as the editor does. The colours become classes (one stylesheet, written beside the data)
// with a light and a dark theme, which the page picks by the system's setting.
import { createHighlighter } from 'shiki';
import { transformerStyleToClass } from '@shikijs/transformers';
import { read } from './repo.js';
import { escapeHtml } from './text.js';

const THEMES = { light: 'github-light', dark: 'github-dark' };

// Bison and Flex, enough to read the docs' excerpts of Parser.y and Lexer.l: directives, strings, character classes,
// <types>, $$ and @1, and the C++ in braces.
const cppInBraces = {
	begin: '\\{',
	end: '\\}',
	beginCaptures: { 0: { name: 'punctuation.section.block.begin' } },
	endCaptures: { 0: { name: 'punctuation.section.block.end' } },
	patterns: [{ include: '#braces' }, { include: '#semantic' }, { include: 'source.cpp' }],
};
const comments = {
	patterns: [
		{ name: 'comment.block', begin: '/\\*', end: '\\*/' },
		{ name: 'comment.line.double-slash', match: '//.*$' },
	],
};
const yacc = {
	name: 'yacc',
	displayName: 'Bison',
	scopeName: 'source.yacc',
	aliases: ['bison', 'y'],
	embeddedLangs: ['cpp'],
	patterns: [
		{ include: '#comments' },
		{ name: 'keyword.control.directive.yacc', match: '%%|%[A-Za-z][\\w.-]*' },
		{ include: '#action' },
		{ name: 'string.quoted.double.yacc', match: '"(?:[^"\\\\]|\\\\.)*"' },
		{ name: 'string.quoted.single.yacc', match: "'(?:[^'\\\\]|\\\\.)*'" },
		{ name: 'storage.type.yacc', match: '<(?:[^<>]|<(?:[^<>]|<[^<>]*>)*>)*>' },
		{ include: '#semantic' },
		{ match: '^\\s*([A-Za-z_][\\w.]*)\\s*(?=:)', captures: { 1: { name: 'entity.name.function.yacc' } } },
		{ name: 'keyword.operator.yacc', match: '[:|;]' },
		{ name: 'constant.other.token.yacc', match: '\\b[A-Z_][A-Z0-9_]*\\b' },
	],
	repository: {
		comments,
		action: cppInBraces,
		braces: { ...cppInBraces, name: 'meta.block.cpp' },
		semantic: { name: 'variable.language.semantic.yacc', match: '\\$(?:\\$|-?\\d+|<[^>]+>\\$)|@(?:\\$|\\d+)' },
	},
};
const lex = {
	name: 'lex',
	displayName: 'Flex',
	scopeName: 'source.lex',
	aliases: ['flex', 'l'],
	embeddedLangs: ['cpp'],
	patterns: [
		{ include: '#comments' },
		{ name: 'keyword.control.directive.lex', match: '^%(?:%|[A-Za-z]+|\\{|\\})' },
		{ name: 'entity.name.tag.condition.lex', match: '<<EOF>>|<[A-Za-z_][\\w,]*>' },
		{ name: 'string.quoted.double.lex', match: '"(?:[^"\\\\]|\\\\.)*"' },
		{ name: 'constant.character.class.lex', match: '\\[\\^?\\]?(?:\\\\.|[^\\]\\\\])*\\]' },
		{ name: 'variable.other.definition.lex', match: '\\{[A-Za-z_]\\w*\\}' },
		{ name: 'keyword.operator.quantifier.lex', match: '\\{\\d+(?:,\\d*)?\\}|[*+?|]' },
		{ match: '^([A-Za-z_]\\w*)(?=\\s)', captures: { 1: { name: 'entity.name.function.lex' } } },
		{ ...cppInBraces, begin: '(?<=\\s)\\{' },
	],
	repository: {
		comments,
		braces: { ...cppInBraces, name: 'meta.block.cpp' },
		semantic: { match: '(?!)' },
	},
};

const fminus = (() => {
	const grammar = JSON.parse(read('editors/vscode/syntaxes/fminus.tmLanguage.json'));
	return { ...grammar, name: 'fminus', displayName: 'FMinus', aliases: ['fm'] };
})();

// A fence's language to Shiki's. `c` fences in the docs are FMinus programs (a subset of C): the FMinus grammar
// colours them, lit's RUN: and CHECK: lines included.
const LANGS = {
	c: 'fminus',
	fminus: 'fminus',
	fm: 'fminus',
	cpp: 'cpp',
	'c++': 'cpp',
	llvm: 'llvm',
	ll: 'llvm',
	cmake: 'cmake',
	bash: 'bash',
	sh: 'bash',
	shell: 'bash',
	console: 'bash',
	asm: 'asm',
	diff: 'diff',
	patch: 'diff',
	json: 'json',
	lex: 'lex',
	flex: 'lex',
	yacc: 'yacc',
	bison: 'yacc',
};
const LABELS = { fminus: 'FMinus', cpp: 'C++', llvm: 'LLVM IR', cmake: 'CMake', bash: 'bash', asm: 'assembly', diff: 'diff', json: 'JSON', lex: 'Flex', yacc: 'Bison' };

const toClass = transformerStyleToClass({ classPrefix: 'sk-' });

const highlighter = await createHighlighter({
	themes: Object.values(THEMES),
	langs: ['c', 'cpp', 'llvm', 'cmake', 'bash', 'asm', 'diff', 'json', fminus, yacc, lex],
});

export const unknownLanguages = new Set();

/** A code block: <pre class="shiki">, or plain text for a fence with no language (or one Shiki lacks). */
export function highlightBlock(code, lang = '') {
	const key = (lang || '').trim().toLowerCase().split(/\s+/)[0];
	const shikiLang = LANGS[key];
	const text = code.replace(/\n$/, '');
	if (!shikiLang) {
		if (key) unknownLanguages.add(key);
		return `<pre class="code plain"><code>${escapeHtml(text)}</code></pre>`;
	}
	const html = highlighter.codeToHtml(text, { lang: shikiLang, themes: THEMES, transformers: [toClass] });
	return html.replace('<pre class="shiki', `<pre data-lang="${LABELS[shikiLang]}" class="code shiki`);
}

/** One line of code, inline: spans in a <code>, for a signature in running text or a list. */
export function highlightInline(code, lang = 'cpp') {
	const html = highlighter.codeToHtml(code, { lang: LANGS[lang] ?? lang, themes: THEMES, structure: 'inline', transformers: [toClass] });
	return `<code class="shiki-inline">${html}</code>`;
}

/** The stylesheet of every colour the highlighted code used. */
export function highlightCss() {
	return toClass.getCSS();
}
