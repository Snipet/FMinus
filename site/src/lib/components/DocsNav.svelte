<script>
	// The internals' sidebar: every document, and the current one's headings.
	import { resolve } from '$app/paths';

	let { list, current = null, headings = [] } = $props();
	const toc = $derived(headings.filter((h) => h.level === 2 || h.level === 3));
</script>

<p class="heading"><a href={resolve('/internals/')} class:current={current === null}>Internals</a></p>
<ul>
	{#each list as doc (doc.slug)}
		<li>
			<a href={doc.url} aria-current={doc.slug === current ? 'page' : undefined}>{doc.title}</a>
			{#if doc.slug === current && toc.length}
				<ul class="toc">
					{#each toc as h (h.id)}
						<li class="level-{h.level}"><a href={`#${h.id}`}>{@html h.html}</a></li>
					{/each}
				</ul>
			{/if}
		</li>
	{/each}
</ul>

<style>
	.heading a {
		display: inline;
		padding: 0;
		color: inherit;
		background: none;
		font-weight: 600;
	}

	.toc {
		margin: 0.25rem 0 0.5rem 0.5rem;
		border-left: 1px solid var(--border-soft);
	}

	.toc a {
		font-size: 0.8125rem;
		color: var(--muted);
		padding: 0.1rem 0.5rem;
	}

	.toc a :global(code) {
		font-size: 0.95em;
		background: none;
		padding: 0;
	}

	.toc .level-3 a {
		padding-left: 1.25rem;
	}
</style>
