// SvelteKit, prerendered to static files (adapter-static) for GitHub Pages. The base path comes from BASE_PATH:
// /FMinus on Pages (the workflow sets it), nothing locally.
import adapter from '@sveltejs/adapter-static';
import { basePath } from './scripts/lib/base.js';

/** @type {import('@sveltejs/kit').Config} */
const config = {
	kit: {
		adapter: adapter({ pages: 'build', assets: 'build', fallback: '404.html', strict: true }),
		paths: { base: basePath(), relative: false },
		prerender: {
			// a broken link or a link to a heading that isn't there fails the build
			handleHttpError: 'fail',
			handleMissingId: 'fail',
		},
	},
};

export default config;
