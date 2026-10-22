<script>
	import Sidebar from '$lib/components/Sidebar.svelte';
	import DocsNav from '$lib/components/DocsNav.svelte';

	let { data } = $props();
	const doc = $derived(data.doc);
	const at = $derived(data.list.findIndex((d) => d.slug === doc.slug));
	const previous = $derived(at > 0 ? data.list[at - 1] : null);
	const next = $derived(at >= 0 && at + 1 < data.list.length ? data.list[at + 1] : null);
</script>

<svelte:head>
	<title>{doc.title} · FMinus</title>
</svelte:head>

<div class="page-grid">
	<Sidebar label="Documents">
		<DocsNav list={data.list} current={doc.slug} headings={doc.headings} />
	</Sidebar>
	<main id="content" class="page-main">
		<article class="prose">{@html doc.html}</article>
		<nav class="pager" aria-label="Documents">
			{#if previous}<a class="prev" href={previous.url}>← {previous.title}</a>{:else}<span></span>{/if}
			{#if next}<a class="next" href={next.url}>{next.title} →</a>{/if}
		</nav>
		<p class="from muted">Source: <a href={doc.sourceUrl}><code>{doc.source}</code></a></p>
	</main>
</div>

<style>
	.pager {
		display: flex;
		justify-content: space-between;
		gap: 1rem;
		margin-top: 3rem;
		padding-top: 1rem;
		border-top: 1px solid var(--border-soft);
		max-width: var(--measure);
	}

	.next {
		text-align: right;
		margin-left: auto;
	}

	.from {
		font-size: 0.875rem;
	}
</style>
