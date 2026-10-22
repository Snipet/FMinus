// The repository the generators read (site/.., never a machine path), where they write, and the site's URLs.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { basePath } from './base.js';

export const siteDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
export const repoDir = path.resolve(siteDir, '..');
export const generatedDir = path.join(siteDir, 'src/lib/generated');

export const githubRepo = 'https://github.com/Snipet/FMinus';
export const githubBranch = 'main';

/** The site's base path: '' locally, '/FMinus' on GitHub Pages (BASE_PATH). */
export const base = basePath();

/** A failure the build reports as it is: the file and line, and what to change. */
export class GenerateError extends Error {
	constructor(message) {
		super(message);
		this.name = 'GenerateError';
	}
}

/** A repository file, by its path from the repository's root. */
export function read(relative) {
	const file = path.join(repoDir, relative);
	if (!fs.existsSync(file)) {
		throw new GenerateError(`${relative}: not found (the generators read the repository at site/..)`);
	}
	return fs.readFileSync(file, 'utf8');
}

/** Whether the repository has this path, and not only a build tree or a dependency cache (both are ignored). */
export function tracked(relative) {
	const clean = relative.replace(/\/+$/, '');
	if (clean === '' || clean.startsWith('..') || path.isAbsolute(clean)) return false;
	const first = clean.split('/')[0];
	if (/^build/.test(first) || first.startsWith('.') || first === 'node_modules') return false;
	if (clean.split('/').includes('node_modules')) return false;
	return fs.existsSync(path.join(repoDir, clean));
}

export function isDirectory(relative) {
	const file = path.join(repoDir, relative);
	return fs.existsSync(file) && fs.statSync(file).isDirectory();
}

/** The file or directory on GitHub, at the default branch. */
export function githubUrl(relative, hash = '') {
	const clean = relative.replace(/\/+$/, '');
	const kind = isDirectory(clean) ? 'tree' : 'blob';
	return `${githubRepo}/${kind}/${githubBranch}/${clean}${hash}`;
}

/** A line of a file on GitHub. */
export function githubLine(relative, line) {
	return `${githubRepo}/blob/${githubBranch}/${relative}#L${line}`;
}

/** A page of the site, from its route ('/api/'): the base path in front. */
export function route(pathname) {
	return `${base}${pathname}`;
}

export function writeJson(relative, value) {
	const file = path.join(generatedDir, relative);
	fs.mkdirSync(path.dirname(file), { recursive: true });
	fs.writeFileSync(file, JSON.stringify(value));
	return file;
}

export function writeText(relative, text) {
	const file = path.join(generatedDir, relative);
	fs.mkdirSync(path.dirname(file), { recursive: true });
	fs.writeFileSync(file, text);
	return file;
}
