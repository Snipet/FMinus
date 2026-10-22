<script>
	import { resolve } from '$app/paths';
	import home from '$lib/generated/home.json';
</script>

<svelte:head>
	<title>FMinus</title>
	<meta name="description" content="FMinus's documentation: the public C++ API, the grammar, and the compiler's internals." />
</svelte:head>

<main id="content" class="page-single home">
	<h1>FMinus</h1>
	<div class="prose intro">{@html home.introHtml}</div>

	<div class="sections">
		<section>
			<h2><a href={resolve('/api/')}>API reference</a></h2>
			<p>{home.api.summary}</p>
			<p class="muted small"><code>{home.api.file}</code>: {home.api.count} declarations.</p>
		</section>

		<section>
			<h2><a href={resolve('/grammar/')}>Grammar</a></h2>
			<p>{home.grammar.summary}</p>
			<p class="muted small">{home.grammar.rules} rules, {home.grammar.tokens} tokens, with railroad diagrams.</p>
		</section>

		<section>
			<h2><a href={resolve('/internals/')}>Internals</a></h2>
			<ul class="docs">
				{#each home.docs as doc (doc.slug)}
					<li>
						<a href={doc.url}>{doc.title}</a>{#if doc.descriptionHtml}<span class="muted">: {@html doc.descriptionHtml}</span>{/if}
					</li>
				{/each}
			</ul>
		</section>
	</div>
</main>

<style>
	.home h1 {
		font-size: 2.2rem;
		margin-top: 0.75rem;
	}

	.intro {
		margin-bottom: 1.5rem;
	}

	.sections section {
		margin-bottom: 2rem;
	}

	.sections h2 {
		font-size: 1.25rem;
		margin-bottom: 0.5rem;
	}

	.small {
		font-size: 0.875rem;
	}

	.docs {
		padding-left: 1.1rem;
	}

	.docs li + li {
		margin-top: 0.45rem;
	}
</style>
