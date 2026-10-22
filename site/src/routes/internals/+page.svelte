<script>
	import internals from '$lib/generated/internals.json';
	import Sidebar from '$lib/components/Sidebar.svelte';
	import DocsNav from '$lib/components/DocsNav.svelte';

	const doc = internals.index;
</script>

<svelte:head>
	<title>Internals · FMinus</title>
	<meta name="description" content="FMinus's manual: the architecture, writing programs, extending the compiler, SIMD, building and testing." />
</svelte:head>

<div class="page-grid">
	<Sidebar label="Documents">
		<DocsNav list={internals.list} />
	</Sidebar>
	<main id="content" class="page-main">
		<article class="prose">{@html doc.html}</article>
		<p class="from muted">Source: <a href={doc.sourceUrl}><code>{doc.source}</code></a></p>
	</main>
</div>

<style>
	.from {
		font-size: 0.875rem;
		margin-top: 3rem;
	}
</style>
