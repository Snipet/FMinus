// Checks the built site (build/, after npm run build, with the same BASE_PATH): every link and asset between its
// pages resolves under the base path, every #fragment names an id on its page, and the pages hold what the
// generators found: every public declaration of FMinus.h, every rule and token of the grammar, every document.
import fs from 'node:fs';
import path from 'node:path';
import { basePath } from './lib/base.js';
import { generatedDir, siteDir } from './lib/repo.js';

const build = path.join(siteDir, 'build');
const base = basePath();
const problems = [];

if (!fs.existsSync(build)) {
	console.error('check: no build/: run npm run build first');
	process.exit(1);
}

const files = [];
(function walk(dir) {
	for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
		const file = path.join(dir, entry.name);
		if (entry.isDirectory()) walk(file);
		else files.push(path.relative(build, file).split(path.sep).join('/'));
	}
})(build);
const pages = files.filter((f) => f.endsWith('.html'));

const decode = (s) => s.replace(/&amp;/g, '&').replace(/&quot;/g, '"').replace(/&#39;/g, "'").replace(/&lt;/g, '<').replace(/&gt;/g, '>');
const idsOf = new Map();
const ids = (page) => {
	if (!idsOf.has(page)) {
		const html = fs.readFileSync(path.join(build, page), 'utf8');
		idsOf.set(page, new Set([...html.matchAll(/\sid="([^"]*)"/g)].map((m) => decode(m[1]))));
	}
	return idsOf.get(page);
};

/** The built file a path of the site names, or null. */
function fileFor(pathname) {
	if (!pathname.startsWith(`${base}/`)) return null;
	let rel = decodeURIComponent(pathname.slice(base.length + 1));
	if (rel === '' || rel.endsWith('/')) rel += 'index.html';
	return files.includes(rel) ? rel : files.includes(`${rel}/index.html`) ? `${rel}/index.html` : null;
}

let links = 0;
for (const page of pages) {
	if (page === '404.html') continue; // the fallback page: its links are the app's own
	const html = fs.readFileSync(path.join(build, page), 'utf8');
	for (const m of html.matchAll(/\s(href|src)="([^"]*)"/g)) {
		const value = decode(m[2]);
		if (/^(https?:|mailto:|data:)/.test(value)) continue;
		links++;
		const hashAt = value.indexOf('#');
		const target = hashAt >= 0 ? value.slice(0, hashAt) : value;
		const hash = hashAt >= 0 ? decodeURIComponent(value.slice(hashAt + 1)) : '';
		let file = page;
		if (target !== '') {
			if (!target.startsWith('/')) {
				problems.push(`${page}: a relative link, ${value} (the site's links are absolute, under '${base || '/'}')`);
				continue;
			}
			file = fileFor(target);
			if (!file) {
				problems.push(`${page}: ${value} is not a file of the site${base && !target.startsWith(base) ? ` (it lacks the base path ${base})` : ''}`);
				continue;
			}
		}
		if (hash && file.endsWith('.html') && !ids(file).has(hash)) problems.push(`${page}: ${value}: ${file} has no id "${hash}"`);
	}
}

// What the generators found is on the pages.
const generated = (name) => JSON.parse(fs.readFileSync(path.join(generatedDir, name), 'utf8'));
const api = generated('api.json');
const apiIds = ids('api/index.html');
for (const d of api.declarations) if (!apiIds.has(d.id)) problems.push(`api/index.html: no entry for ${d.id} (FMinus.h:${d.line})`);
const grammar = generated('grammar.json');
const grammarIds = ids('grammar/index.html');
for (const r of grammar.rules) if (!grammarIds.has(r.id)) problems.push(`grammar/index.html: no rule ${r.name}`);
for (const t of grammar.tokens) if (!grammarIds.has(t.id)) problems.push(`grammar/index.html: no token ${t.name}`);
const internals = generated('internals.json');
for (const doc of internals.list) {
	const file = fileFor(doc.url);
	if (!file) problems.push(`no page for ${doc.source} at ${doc.url}`);
}

if (problems.length) {
	console.error(`check: ${problems.length} problem(s):\n  ${problems.join('\n  ')}`);
	process.exit(1);
}
console.log(`check: ${pages.length} pages, ${links} links and assets under '${base || '/'}' resolve; ` +
	`${api.declarations.length} declarations, ${grammar.rules.length} rules, ${grammar.tokens.length} tokens and ` +
	`${internals.list.length + 1} documents are on their pages`);
