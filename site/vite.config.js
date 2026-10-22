import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig } from 'vite';

export default defineConfig({
	plugins: [sveltekit()],
	build: {
		// Each document's page is one chunk of generated content (docs/extending.md is about 0.5 MB of highlighted
		// HTML), loaded only on its own page: not code that splitting would help.
		chunkSizeWarningLimit: 1500,
	},
});
