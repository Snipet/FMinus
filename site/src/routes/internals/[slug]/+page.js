import { error } from '@sveltejs/kit';
import internals from '$lib/generated/internals.json';

// one chunk a document, loaded by its own page only
const documents = import.meta.glob('/src/lib/generated/docs/*.json', { import: 'default' });

export const entries = () => internals.list.map((doc) => ({ slug: doc.slug }));

export async function load({ params }) {
	const loader = documents[`/src/lib/generated/docs/${params.slug}.json`];
	if (!loader) error(404, `No document named ${params.slug}`);
	return { doc: await loader(), list: internals.list };
}
