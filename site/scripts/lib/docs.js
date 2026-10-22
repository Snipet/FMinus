// The internals: docs/*.md (architecture, programs, extending, simd, building, testing, recipes/) and the README's
// language section, as pages. Markdown is rendered at build time with markdown-it; headings get GitHub's ids (so
// `building.md#windows` still lands), links between docs go to the site's pages, links and `paths` to the
// repository's files go to GitHub, and code is highlighted with Shiki.
import fs from 'node:fs';
import path from 'node:path';
import MarkdownIt from 'markdown-it';
import GithubSlugger from 'github-slugger';
import { GenerateError, githubUrl, read, repoDir, route, tracked } from './repo.js';
import { escapeHtml } from './text.js';

const README = 'README.md';
const DOCS = 'docs';
const LANGUAGE = /^## The language\b.*$/m;

export function generateDocs({ highlightBlock }) {
	const md = new MarkdownIt({ html: false, linkify: false, typographer: false, highlight: (code, lang) => highlightBlock(code, lang) });
	md.renderer.rules.table_open = () => '<div class="table-wrap"><table>\n';
	md.renderer.rules.table_close = () => '</table></div>\n';
	md.renderer.rules.heading_close = (tokens, idx) => {
		const id = tokens[idx].meta?.id;
		const anchor = id ? `<a class="anchor" href="#${escapeHtml(id)}" aria-label="Link to this section">#</a>` : '';
		return `${anchor}</${tokens[idx].tag}>\n`;
	};
	md.core.ruler.push('fminus_docs', (state) => transform(state, md));

	// ------------------------------------------------------------------------------------------------ the pages
	const index = read(`${DOCS}/README.md`);
	const listed = listedDocuments(md, index);
	const pages = [];
	const add = (page) => {
		if (pages.some((p) => p.slug === page.slug)) throw new GenerateError(`two documents would be the page /internals/${page.slug}/`);
		pages.push(page);
	};
	const language = readLanguage();
	add({ slug: 'language', source: README, markdown: language.markdown, description: language.firstParagraph, excerpt: true, anchor: language.anchor });
	for (const { target, description } of listed) {
		const source = docFile(target);
		if (!source) throw new GenerateError(`${DOCS}/README.md lists ${target}, which is not a document in the repository`);
		const slug = slugOf(source);
		add({ slug, source, markdown: read(source), description });
	}
	for (const file of fs.readdirSync(path.join(repoDir, DOCS)).sort()) {
		const source = `${DOCS}/${file}`;
		if (!file.endsWith('.md') || file === 'README.md' || pages.some((p) => p.source === source)) continue;
		add({ slug: slugOf(source), source, markdown: read(source), description: '' });
	}
	// the recipes' patches, shown with their page
	const recipes = pages.find((p) => p.source === `${DOCS}/recipes/README.md`);
	const patches = new Map();
	if (recipes) {
		for (const file of fs.readdirSync(path.join(repoDir, DOCS, 'recipes')).filter((f) => f.endsWith('.patch')).sort()) {
			const patch = read(`${DOCS}/recipes/${file}`);
			const fence = '`'.repeat(Math.max(3, ...[...patch.matchAll(/^ *(`{3,})/gm)].map((m) => m[1].length + 1)));
			const heading = `The patch: ${file}`;
			recipes.markdown += `\n\n## The patch: \`${file}\`\n\n${fence}diff\n${patch.replace(/\n$/, '')}\n${fence}\n`;
			patches.set(`${DOCS}/recipes/${file}`, new GithubSlugger().slug(heading));
		}
	}
	const indexPage = { slug: '', source: `${DOCS}/README.md`, markdown: index, description: '' };

	// Ids first, every page's, so a link to another page's heading can be checked.
	const all = [indexPage, ...pages];
	for (const page of all) page.ids = headingIds(md, page.markdown);
	const pageBySource = new Map();
	for (const page of all) pageBySource.set(page.source, page);
	pageBySource.set(DOCS, indexPage);
	if (recipes) pageBySource.set(`${DOCS}/recipes`, recipes);

	const context = { pages: all, pageBySource, patches, recipes, language: pages.find((p) => p.slug === 'language') };
	const rendered = all.map((page) => renderPage(md, page, context));

	const search = rendered.flatMap((p) => p.headings.filter((h) => h.level <= 3).map((h) => ({
		t: h.text,
		s: h.level === 1 ? 'Internals' : `Internals · ${p.title}`,
		u: h.level === 1 ? p.url : `${p.url}#${h.id}`,
	})));

	// The README's opening paragraph, for the home page.
	const intro = readIntro(md);

	return {
		index: rendered[0],
		pages: rendered.slice(1),
		list: rendered.slice(1).map(({ slug, title, url, descriptionHtml, source, sourceUrl }) => ({ slug, title, url, descriptionHtml, source, sourceUrl })),
		intro,
		search,
	};

	// ---------------------------------------------------------------------------------------------- rendering
	function renderPage(md, page, context) {
		const env = { page, context, headings: [], slugger: new GithubSlugger() };
		const html = md.render(page.markdown, env);
		const h1 = env.headings.find((h) => h.level === 1);
		return {
			slug: page.slug,
			title: h1 ? h1.text : page.slug,
			url: page.slug ? route(`/internals/${page.slug}/`) : route('/internals/'),
			source: page.source,
			// an excerpt's source is its section of the file, which GitHub shows rendered: its heading's anchor
			sourceUrl: githubUrl(page.source, page.anchor ? `#${page.anchor}` : ''),
			descriptionHtml: page.description ? renderDescription(md, page.description, page, context) : '',
			html,
			headings: env.headings,
		};
	}
}

/**
 * The documents docs/README.md's table lists: each row whose first cell is a link, and its second cell, the
 * description, as markdown-it parsed it (a cell's `\|` is a '|', as GitHub reads a table).
 */
function listedDocuments(md, index) {
	const tokens = md.parse(index, {});
	const listed = [];
	for (let i = 0; i < tokens.length; i++) {
		if (tokens[i].type !== 'tr_open') continue;
		const cells = [];
		for (let j = i + 1; j < tokens.length && tokens[j].type !== 'tr_close'; j++) {
			if (tokens[j].type === 'td_open') cells.push(tokens[j + 1]);
		}
		const children = (cells[0]?.children ?? []).filter((c) => !(c.type === 'text' && c.content.trim() === ''));
		if (children[0]?.type !== 'link_open' || children[children.length - 1]?.type !== 'link_close') continue;
		if (children.slice(1, -1).some((c) => c.type === 'link_open')) continue;
		const target = path.posix.normalize(path.posix.join(DOCS, children[0].attrGet('href')));
		listed.push({ target, description: cells[1]?.content.trim() ?? '' });
	}
	return listed;
}

function renderDescription(md, text, page, context) {
	const env = { page: { ...page, source: `${DOCS}/README.md`, ids: new Set() }, context, headings: [], slugger: new GithubSlugger(), inlineOnly: true };
	return md.renderInline(text, env);
}

/** The heading ids a document will have, as GitHub gives them. */
function headingIds(md, markdown) {
	const slugger = new GithubSlugger();
	const ids = new Set();
	const tokens = md.parse(markdown, { idsOnly: true });
	for (let i = 0; i < tokens.length; i++) if (tokens[i].type === 'heading_open') ids.add(slugger.slug(headingText(tokens[i + 1])));
	return ids;
}

function headingText(inline) {
	return (inline.children ?? []).filter((c) => c.type === 'text' || c.type === 'code_inline').map((c) => c.content).join('');
}

function headingHtml(inline) {
	return (inline.children ?? []).map((c) => {
		if (c.type === 'code_inline') return `<code>${escapeHtml(c.content)}</code>`;
		if (c.type === 'text') return escapeHtml(c.content);
		return '';
	}).join('');
}

/** markdown-it's core rule: heading ids, link targets, links on `paths`. */
function transform(state, md) {
	const env = state.env;
	if (!env || !env.page) return;
	const tokens = state.tokens;
	for (let i = 0; i < tokens.length; i++) {
		const t = tokens[i];
		if (t.type === 'heading_open' && !env.inlineOnly) {
			const inline = tokens[i + 1];
			const text = headingText(inline);
			const id = env.slugger.slug(text);
			t.attrSet('id', id);
			tokens[i + 2].meta = { id };
			env.headings.push({ level: Number(t.tag.slice(1)), text, html: headingHtml(inline), id });
		}
		if (t.type === 'inline' && t.children) {
			const inHeading = tokens[i - 1]?.type === 'heading_open';
			t.children = rewriteInline(t.children, env, state, inHeading);
		}
	}
}

function rewriteInline(children, env, state, inHeading) {
	const out = [];
	let depth = 0;
	for (const c of children) {
		if (c.type === 'link_open') {
			c.attrSet('href', rewriteHref(c.attrGet('href'), env));
			depth++;
		} else if (c.type === 'link_close') {
			depth--;
		} else if (c.type === 'code_inline' && depth === 0 && !inHeading) {
			const href = pathHref(c.content);
			if (href) {
				const open = new state.Token('link_open', 'a', 1);
				open.attrs = [['href', href], ['class', 'source']];
				const close = new state.Token('link_close', 'a', -1);
				out.push(open, c, close);
				continue;
			}
		}
		out.push(c);
	}
	return out;
}

/** A link in a document, to the site's page for it, or to GitHub. */
function rewriteHref(href, env) {
	if (!href || /^[a-z][a-z0-9+.-]*:/i.test(href) || href.startsWith('//')) return href;
	const { page, context } = env;
	const hashAt = href.indexOf('#');
	const target = hashAt >= 0 ? href.slice(0, hashAt) : href;
	const hash = hashAt >= 0 ? href.slice(hashAt) : '';
	if (target === '') {
		if (page.ids.has(decodeURIComponent(hash.slice(1)))) return hash;
		if (page.excerpt) return githubUrl(page.source, hash); // a README section the excerpt doesn't hold
		throw new GenerateError(`${page.source}: the link ${href} names no heading of this document`);
	}
	const resolved = path.posix.normalize(path.posix.join(path.posix.dirname(page.source), decodeURI(target))).replace(/\/$/, '');
	if (resolved.startsWith('..')) throw new GenerateError(`${page.source}: the link ${href} leaves the repository`);
	let dest = context.pageBySource.get(resolved);
	if (resolved === README) dest = hash && context.language?.ids.has(hash.slice(1)) ? context.language : null;
	if (dest) {
		if (hash && !dest.ids.has(decodeURIComponent(hash.slice(1)))) {
			throw new GenerateError(`${page.source}: the link ${href} names no heading of ${dest.source}`);
		}
		return (dest.slug ? route(`/internals/${dest.slug}/`) : route('/internals/')) + hash;
	}
	if (context.patches?.has(resolved) && !hash) return `${route(`/internals/${context.recipes.slug}/`)}#${context.patches.get(resolved)}`;
	if (!tracked(resolved)) throw new GenerateError(`${page.source}: the link ${href} points to ${resolved}, which is not in the repository`);
	return githubUrl(resolved, hash);
}

/** A `path` in the text that names a file or directory of the repository (from its root, src/ or include/). */
function pathHref(code) {
	if (!code.includes('/') || !/^[\w.+-]+(?:\/[\w.+-]+)*\/?$/.test(code)) return null;
	for (const prefix of ['', 'src/', 'include/']) {
		const candidate = prefix + code;
		if (tracked(candidate)) return githubUrl(candidate.replace(/\/$/, ''));
	}
	return null;
}

function docFile(target) {
	const clean = target.replace(/\/$/, '');
	if (clean.endsWith('.md') && tracked(clean)) return clean;
	if (tracked(`${clean}/README.md`)) return `${clean}/README.md`;
	return null;
}

function slugOf(source) {
	const rel = source.replace(/^docs\//, '').replace(/\/README\.md$/, '').replace(/\.md$/, '');
	return rel.toLowerCase().replace(/[^a-z0-9]+/g, '-');
}

/** The README's language section: from its heading to the next of its level, as a page of its own. */
function readLanguage() {
	const readme = read(README);
	const m = LANGUAGE.exec(readme);
	if (!m) throw new GenerateError(`${README}: no "## The language" section, which is the language page`);
	const after = readme.slice(m.index + m[0].length);
	const next = after.search(/^## /m);
	const body = next >= 0 ? after.slice(0, next) : after;
	// its heading's anchor on GitHub: the README's headings slugged in order, as GitHub does
	const line = readme.slice(0, m.index).split('\n').length;
	const slugger = new GithubSlugger();
	let anchor = null;
	const parsed = new MarkdownIt().parse(readme, {});
	for (let i = 0; i < parsed.length; i++) {
		if (parsed[i].type !== 'heading_open') continue;
		const slug = slugger.slug(headingText(parsed[i + 1]));
		if (parsed[i].map?.[0] === line - 1) anchor = slug;
	}
	if (!anchor) throw new GenerateError(`${README}:${line}: the language section's heading has no anchor`);
	// its first paragraph describes it in the list of documents
	const firstParagraph = (body.trim().split(/\n\s*\n/)[0] ?? '').replace(/\s*\n\s*/g, ' ');
	return { markdown: `#${m[0].slice(2)}\n${body}`, anchor, firstParagraph };
}

function readIntro(md) {
	const readme = read(README);
	const m = /^# .*\n+((?:.+\n)+)/m.exec(readme);
	if (!m) throw new GenerateError(`${README}: no paragraph after its title, which is the home page's introduction`);
	return md.render(m[1].trim(), { page: { source: README, ids: new Set() }, context: { pageBySource: new Map() }, headings: [], slugger: new GithubSlugger(), inlineOnly: true });
}
