<script>
	// Site-wide search over every heading and declaration, in the browser, from the generated index. The index loads
	// the first time the box is focused (or / is pressed), so pages that never search never fetch it.
	import { goto } from '$app/navigation';

	let query = $state('');
	let index = $state(null);
	let open = $state(false);
	let active = $state(0);
	let input;
	const listId = 'search-results';

	async function load() {
		if (!index) index = (await import('$lib/generated/search.json')).default;
	}

	function score(entry, words) {
		const title = entry.t.toLowerCase();
		const where = entry.s.toLowerCase();
		let total = 0;
		for (const w of words) {
			const at = title.indexOf(w);
			if (at < 0) {
				if (!where.includes(w)) return 0;
				total += 1;
				continue;
			}
			if (title === w) total += 100;
			else if (at === 0) total += 40;
			else if (/[^a-z0-9]/.test(title[at - 1])) total += 20;
			else total += 8;
		}
		return total - title.length / 100;
	}

	const results = $derived.by(() => {
		const words = query.toLowerCase().trim().split(/\s+/).filter(Boolean);
		if (!index || !words.length) return [];
		const scored = [];
		for (const entry of index) {
			const s = score(entry, words);
			if (s > 0) scored.push({ entry, s });
		}
		scored.sort((a, b) => b.s - a.s);
		return scored.slice(0, 12).map((x) => x.entry);
	});

	$effect(() => {
		// a new query starts at its first result
		query;
		active = 0;
	});

	function keydown(event) {
		if (event.key === 'ArrowDown') {
			event.preventDefault();
			open = true;
			active = Math.min(active + 1, results.length - 1);
		} else if (event.key === 'ArrowUp') {
			event.preventDefault();
			active = Math.max(active - 1, 0);
		} else if (event.key === 'Enter' && results[active]) {
			event.preventDefault();
			choose(results[active]);
		} else if (event.key === 'Escape') {
			open = false;
			input.blur();
		}
	}

	function choose(entry) {
		open = false;
		query = '';
		goto(entry.u);
	}

	function shortcut(event) {
		if (event.key !== '/' || event.metaKey || event.ctrlKey || event.altKey) return;
		const target = event.target;
		if (target instanceof HTMLElement && (target.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName))) return;
		event.preventDefault();
		input.focus();
	}
</script>

<svelte:window onkeydown={shortcut} />

<div class="search" role="search">
	<input
		bind:this={input}
		bind:value={query}
		type="search"
		placeholder="Search  /"
		aria-label="Search the documentation"
		autocomplete="off"
		spellcheck="false"
		role="combobox"
		aria-expanded={open && results.length > 0}
		aria-controls={open && query.trim() ? listId : undefined}
		aria-activedescendant={open && results.length ? `search-${active}` : undefined}
		onfocus={() => {
			load();
			open = true;
		}}
		oninput={() => (open = true)}
		onblur={() => setTimeout(() => (open = false), 150)}
		onkeydown={keydown}
	/>
	{#if open && query.trim()}
		<ul class="results" id={listId} role="listbox">
			{#each results as entry, i (entry.u + entry.t)}
				<li role="option" id={`search-${i}`} aria-selected={i === active}>
					<a
						href={entry.u}
						class:active={i === active}
						onmousedown={(e) => e.preventDefault()}
						onclick={(e) => {
							e.preventDefault();
							choose(entry);
						}}
					>
						<span class="title" class:code={/^(API|Grammar)/.test(entry.s)}>{entry.t}</span>
						<span class="where">{entry.s}</span>
					</a>
				</li>
			{:else}
				<li class="none">{index ? 'Nothing found' : 'Loading…'}</li>
			{/each}
		</ul>
	{/if}
</div>

<style>
	.search {
		position: relative;
		flex: 0 1 20rem;
		min-width: 0;
	}

	input {
		width: 100%;
		font: inherit;
		font-size: 0.9rem;
		color: var(--fg);
		background: var(--bg-soft);
		border: 1px solid var(--border);
		border-radius: 6px;
		padding: 0.35rem 0.6rem;
	}

	input::placeholder {
		color: var(--muted);
	}

	.results {
		position: absolute;
		right: 0;
		top: calc(100% + 0.35rem);
		width: min(30rem, calc(100vw - 32px));
		max-height: min(70vh, 32rem);
		overflow-y: auto;
		list-style: none;
		margin: 0;
		padding: 0.35rem;
		background: var(--bg);
		border: 1px solid var(--border);
		border-radius: 8px;
		box-shadow: 0 8px 24px rgba(0, 0, 0, 0.12);
		z-index: 50;
	}

	li {
		margin: 0;
	}

	a {
		display: flex;
		flex-direction: column;
		padding: 0.4rem 0.6rem;
		border-radius: 5px;
		color: var(--fg);
	}

	a:hover,
	a.active {
		background: var(--accent-soft);
		text-decoration: none;
	}

	.title {
		font-size: 0.9rem;
		overflow-wrap: anywhere;
	}

	.title.code {
		font-family: var(--font-mono);
		font-size: 0.85rem;
	}

	.where {
		font-size: 0.75rem;
		color: var(--muted);
	}

	.none {
		padding: 0.5rem 0.6rem;
		color: var(--muted);
		font-size: 0.875rem;
	}

	@media (max-width: 40rem) {
		.search {
			flex-basis: 100%;
		}

		.results {
			left: 0;
			right: auto;
			width: 100%;
		}
	}
</style>
