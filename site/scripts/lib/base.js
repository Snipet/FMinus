// The base path the site is served under, from the environment: BASE_PATH=/FMinus on GitHub Pages, nothing locally.
// svelte.config.js and the generators both read it, so the pages and the links generated into them agree.
export function basePath() {
	const value = (process.env.BASE_PATH ?? '').trim();
	if (value === '' || value === '/') return '';
	if (!/^\/[A-Za-z0-9._~\/-]*[^\/]$/.test(value)) {
		throw new Error(`BASE_PATH must start with / and not end with one (as /FMinus), not '${value}'`);
	}
	return value;
}
