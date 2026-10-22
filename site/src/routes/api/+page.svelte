<script>
	import api from '$lib/generated/api.json';
	import ApiEntry from '$lib/components/ApiEntry.svelte';
	import Sidebar from '$lib/components/Sidebar.svelte';
</script>

<svelte:head>
	<title>API reference · FMinus</title>
	<meta name="description" content={api.summary} />
</svelte:head>

<div class="page-grid">
	<Sidebar label="Declarations">
		<p class="heading">FMinus.h</p>
		<ul class="code">
			<li><a href="#introduction" style="font-family: var(--font-sans)">Introduction</a></li>
			{#each api.sidebar as item (item.id)}
				<li><a href={`#${item.id}`} style={`padding-left: ${0.5 + item.depth * 0.85}rem`}>{item.label}</a></li>
			{/each}
		</ul>
	</Sidebar>

	<main id="content" class="page-main">
		<header class="page-head">
			<h1>API reference</h1>
			<p class="from">
				Generated from <a href={api.fileUrl}><code>{api.file}</code></a>, whose comments are the documentation.
			</p>
		</header>

		<section id="introduction" class="intro">
			<h2>Introduction</h2>
			<div class="doc prose-ish">{@html api.introHtml}</div>
			{#if api.includeHtml}
				<h3>Includes</h3>
				{@html api.includeHtml}
			{/if}
		</section>

		{#each api.entries as entry (entry.id)}
			<ApiEntry {entry} depth={2} />
		{/each}
	</main>
</div>

<style>
	.intro {
		margin-bottom: 2rem;
	}

	.intro :global(.comment-table) {
		font-size: 0.875rem;
	}

	.intro :global(.comment-table td:first-child) {
		font-family: var(--font-mono);
		font-size: 0.8125rem;
		white-space: nowrap;
	}
</style>
