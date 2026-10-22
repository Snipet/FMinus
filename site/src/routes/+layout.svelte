<script>
	import '../app.css';
	import '$lib/generated/highlight.css';
	import { page } from '$app/state';
	import { resolve } from '$app/paths';
	import Search from '$lib/components/Search.svelte';

	let { children } = $props();

	const sections = [
		{ href: resolve('/api/'), label: 'API' },
		{ href: resolve('/grammar/'), label: 'Grammar' },
		{ href: resolve('/internals/'), label: 'Internals' },
	];
	const current = (href) => page.url.pathname.startsWith(href);
</script>

<a class="skip" href="#content">Skip to the content</a>
<header class="site-header">
	<div class="bar">
		<a class="brand" href={resolve('/')}>FMinus</a>
		<nav aria-label="Sections">
			{#each sections as s (s.href)}
				<a href={s.href} aria-current={current(s.href) ? 'page' : undefined}>{s.label}</a>
			{/each}
		</nav>
		<Search />
	</div>
</header>

{@render children()}

<footer class="site-footer">
	<p>
		Generated from <a href="https://github.com/Snipet/FMinus">Snipet/FMinus</a>: the API from
		<code>include/fminus/FMinus.h</code>, the grammar from <code>src/frontend/Parser.y</code> and
		<code>Lexer.l</code>, the internals from <code>docs/</code>.
	</p>
</footer>

<style>
	.site-header {
		position: sticky;
		top: 0;
		z-index: 40;
		background: color-mix(in srgb, var(--bg) 92%, transparent);
		backdrop-filter: blur(6px);
		border-bottom: 1px solid var(--border-soft);
	}

	.bar {
		display: flex;
		align-items: center;
		gap: 1.25rem;
		max-width: 92rem;
		margin: 0 auto;
		min-height: var(--header);
		padding: 0.4rem 1.25rem;
	}

	.brand {
		font-weight: 700;
		font-size: 1.05rem;
		color: var(--fg);
		letter-spacing: -0.01em;
	}

	nav {
		display: flex;
		gap: 0.25rem;
		flex: 1 1 auto;
	}

	nav a {
		color: var(--fg-soft);
		padding: 0.25rem 0.55rem;
		border-radius: 5px;
		font-size: 0.95rem;
	}

	nav a:hover {
		background: var(--bg-hover);
		text-decoration: none;
	}

	nav a[aria-current='page'] {
		color: var(--fg);
		background: var(--accent-soft);
	}

	.site-footer {
		border-top: 1px solid var(--border-soft);
		color: var(--muted);
		font-size: 0.8125rem;
		padding: 1.25rem 16px 2rem;
		text-align: center;
	}

	.site-footer p {
		max-width: 46rem;
		margin: 0 auto;
	}

	@media (max-width: 40rem) {
		.bar {
			flex-wrap: wrap;
			gap: 0.4rem 0.75rem;
			padding: 0.5rem 16px;
		}

		.site-header {
			position: static;
		}

		nav {
			flex: 1 1 auto;
		}
	}
</style>
